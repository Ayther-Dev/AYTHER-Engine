// ---------------------------------------------------------------------------
// ayther_sdk.cpp — la implementación de la API C (-).
//
// Todo lo de acá es traducción: la lógica vive en `AytherSession` y esta capa
// la envuelve sin decidir nada por su cuenta. Es a propósito — una capa C que
// además decide se convierte en un segundo motor, y entonces hay dos
// comportamientos que mantener alineados.
//
// Las tres cosas que sí resuelve, porque son de la FRONTERA y no del motor:
//
//   1. **Los errores no cruzan como excepciones.** Toda entrada está envuelta:
//      lo que salga por adentro se convierte en un `AyStatus`. Una excepción
//      cruzando a C es comportamiento indefinido, no un error feo.
//   2. **La memoria del llamador es del llamador.** Nada de lo que sale de acá
//      hay que liberar; lo que se copia, se copia a un buffer que el llamador
//      trajo.
//   3. **Las extensiones se aíslan.** Un filtro que falla se desactiva; uno que
//      tira una excepción, también — y en los dos casos la sesión sigue.
// ---------------------------------------------------------------------------
#include "ayther_sdk.h"

#include "ayther_session.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

/// Un handler registrado. Los tres tipos comparten estructura porque comparten
/// ciclo de vida: registrar, fallar, desactivarse, darse de baja.
struct Ext {
    enum class Kind { Frame, Audio, Post } kind = Kind::Frame;
    uint32_t id = 0;
    void*    user = nullptr;
    AyFrameObserver frame = nullptr;
    AyAudioObserver audio = nullptr;
    AyPostFilter    post  = nullptr;
    uint32_t failures = 0;
    bool     active   = true;
};

std::string& hilo_error_create() {
    // Por hilo: `ay_create` puede fallar en dos hilos a la vez y el segundo no
    // puede pisarle el motivo al primero.
    thread_local std::string e;
    return e;
}

}  // namespace

/// Lo que hay detrás del tipo opaco. El consumidor nunca ve esto — que es la
/// mitad del punto de la capa: cambiar lo de acá adentro no lo recompila.
struct AySession {
    std::unique_ptr<ayther::AytherSession> session;
    std::string error;
    const ayther::FrameView* fv = nullptr;
    uint32_t input[2] = { 0, 0 };
    std::vector<Ext> exts;
    uint32_t next_id = 1;
    /// EM-7.1: las opciones declaradas, cacheadas. El getter de la sesión
    /// devuelve el vector POR VALOR, así que un `c_str()` sobre él apuntaría a
    /// un temporal muerto — el modo más silencioso de romper una API C.
    /// No cambian nunca: el core las declara una vez, al inicializar.
    std::vector<std::pair<std::string, std::string>> opts;
    bool opts_leidas = false;
    std::vector<uint8_t> ram_view;   // la vista 68k, materializada al pedirla
    uint32_t audio_reportados = 0;   // para no re-notificar lo ya observado
};

namespace {

/// Envuelve una entrada: traduce excepciones y deja el motivo en la sesión.
template <class F>
AyStatus guarded(AySession* s, F&& f) {
    if (!s) return AY_ERR_ARGS;
    try {
        return f();
    } catch (const std::exception& e) {
        s->error = e.what();
        return AY_ERR_INTERNAL;
    } catch (...) {
        s->error = "excepción desconocida cruzando la frontera C";
        return AY_ERR_INTERNAL;
    }
}

void llenar_frame(const AySession* s, AyFrame* out) {
    std::memset(out, 0, sizeof(*out));
    if (!s->fv) return;
    out->pixels = s->fv->fb_pixels;
    out->width  = s->fv->fb_width;
    out->height = s->fv->fb_height;
    out->pitch  = s->fv->fb_pitch;
    out->format = s->fv->fb_format;
    out->index  = s->fv->frame_index;
}

/// Corre los observadores, aislando fallos. Una excepción de una extensión no
/// puede subir: cruzaría a C desde adentro de nuestro propio step.
void notificar(AySession* s) {
    AyFrame f{};
    llenar_frame(s, &f);
    for (Ext& e : s->exts) {
        if (!e.active) continue;
        try {
            if (e.kind == Ext::Kind::Frame && e.frame) {
                e.frame(&f, e.user);
                e.failures = 0;
            } else if (e.kind == Ext::Kind::Audio && e.audio) {
                const uint32_t total = ay_audio_events(s, nullptr, 0);
                if (total > s->audio_reportados) {
                    std::vector<AyAudioEvent> ev(total);
                    ay_audio_events(s, ev.data(), total);
                    e.audio(ev.data() + s->audio_reportados,
                            total - s->audio_reportados, e.user);
                }
                e.failures = 0;
            }
        } catch (...) {
            // Una extensión que tira es una que falla: mismo camino que la que
            // devuelve error. Tratarlas distinto premiaría a la peor de las dos.
            if (++e.failures >= AY_EXT_MAX_FAILURES) e.active = false;
        }
    }
    // Se marca DESPUÉS de notificar a todos: si se marcara adentro del bucle,
    // el segundo observador de audio no vería los eventos que vio el primero.
    s->audio_reportados = ay_audio_events(s, nullptr, 0);
}

}  // namespace

// ===========================================================================
// Errores
// ===========================================================================
extern "C" const char* ay_error_message(const AySession* s) {
    return s ? s->error.c_str() : "";
}
extern "C" const char* ay_last_create_error(void) {
    return hilo_error_create().c_str();
}

// ===========================================================================
// Ciclo de vida
// ===========================================================================
extern "C" AyStatus ay_create(const AySessionConfig* cfg, AySession** out) {
    if (!cfg || !out) return AY_ERR_ARGS;
    *out = nullptr;
    try {
        ayther::AytherSession::Config c;
        c.core_path = cfg->core_path ? cfg->core_path : "";
        c.rom_path  = cfg->rom_path  ? cfg->rom_path  : "";
        c.pack_path = cfg->pack_path ? cfg->pack_path : "";
        c.enable_audio = cfg->enable_audio != 0;
        // Sin derivar el pack del nombre del core: un SDK no puede levantar un
        // pack que el llamador no pidió — sería contenido que aparece solo.
        c.derive_core_pack = false;

        // EM-7.1: las opciones del core. Una clave nula corta el barrido en vez
        // de saltarla: un array a medio llenar es un error del llamador, y
        // seguir leyendo detrás de un NULL es cómo se lee memoria ajena.
        if (cfg->option_keys && cfg->option_values) {
            for (uint32_t i = 0; i < cfg->option_count; ++i) {
                if (!cfg->option_keys[i] || !cfg->option_values[i]) break;
                c.core_options.emplace_back(cfg->option_keys[i],
                                            cfg->option_values[i]);
            }
        }

        auto r = ayther::AytherSession::create(c);
        if (!r) {
            hilo_error_create() = r.error.message;
            // El motivo se distingue para que el consumidor pueda reaccionar
            // sin leer el texto: es la diferencia entre reintentar con otra
            // ruta y avisarle al usuario que falta el archivo.
            const std::string& m = r.error.message;
            if (m.find("core") != std::string::npos) return AY_ERR_CORE;
            if (m.find("ROM")  != std::string::npos ||
                m.find("rom")  != std::string::npos) return AY_ERR_ROM;
            if (m.find("pack") != std::string::npos) return AY_ERR_PACK;
            return AY_ERR_INTERNAL;
        }
        auto* s = new AySession();
        s->session = std::move(*r.value);
        *out = s;
        return AY_OK;
    } catch (const std::exception& e) {
        hilo_error_create() = e.what();
        return AY_ERR_INTERNAL;
    } catch (...) {
        hilo_error_create() = "excepción desconocida al crear la sesión";
        return AY_ERR_INTERNAL;
    }
}

extern "C" void ay_destroy(AySession* s) { delete s; }

// ===========================================================================
// Frame
// ===========================================================================
extern "C" AyStatus ay_step(AySession* s) {
    return guarded(s, [&]() -> AyStatus {
        if (!s->session) return AY_ERR_STATE;
        s->fv = &s->session->step();
        notificar(s);
        return AY_OK;
    });
}

extern "C" AyStatus ay_frame(const AySession* s, AyFrame* out) {
    if (!s || !out) return AY_ERR_ARGS;
    if (!s->fv) return AY_ERR_STATE;      // todavía no se avanzó ningún frame
    llenar_frame(s, out);
    return AY_OK;
}

// ===========================================================================
// Contenido
// ===========================================================================
extern "C" AyStatus ay_set_pack(AySession* s, const char* pack_path) {
    return guarded(s, [&]() -> AyStatus {
        if (!pack_path || !*pack_path) return AY_ERR_ARGS;
        auto r = s->session->set_pack(pack_path);
        if (!r) { s->error = r.error.message; return AY_ERR_PACK; }
        return AY_OK;
    });
}

extern "C" AyStatus ay_clear_pack(AySession* s) {
    return guarded(s, [&]() -> AyStatus {
        auto r = s->session->set_pack("");
        if (!r) { s->error = r.error.message; return AY_ERR_PACK; }
        return AY_OK;
    });
}

extern "C" int ay_has_pack(const AySession* s) {
    return (s && s->session && s->session->has_pack()) ? 1 : 0;
}

extern "C" const char* ay_game_id(const AySession* s) {
    if (!s || !s->session) return "";
    const char* g = s->session->game_id();
    return g ? g : "";
}

// ===========================================================================
// Memoria — vista del 68000
// ===========================================================================
extern "C" uint32_t ay_memory_size(const AySession* s) {
    if (!s || !s->session) return 0;
    return (uint32_t)s->session->work_ram_size();
}

extern "C" AyStatus ay_read_memory(const AySession* s, uint32_t addr,
                                   void* dst, uint32_t len) {
    if (!s || !dst) return AY_ERR_ARGS;
    if (!s->session) return AY_ERR_STATE;
    const uint8_t* ram = s->session->work_ram();
    const uint32_t n   = (uint32_t)s->session->work_ram_size();
    if (!ram || n == 0) return AY_ERR_UNSUPPORTED;
    if (addr > n || len > n - addr) return AY_ERR_ARGS;

    // La vista del 68000: la work RAM llega WORD-SWAPPED, así que el byte de
    // la dirección `a` está en `a ^ 1`. Leerla sin esto da valores que parecen
    // datos y no lo son — el error más repetido de este repo, y por eso la
    // corrección vive acá y no en cada llamador.
    uint8_t* o = (uint8_t*)dst;
    for (uint32_t i = 0; i < len; ++i) o[i] = ram[(addr + i) ^ 1u];
    return AY_OK;
}

// ===========================================================================
// Entrada
// ===========================================================================
extern "C" AyStatus ay_set_input(AySession* s, uint32_t port, uint32_t buttons) {
    return guarded(s, [&]() -> AyStatus {
        if (port > 1) return AY_ERR_ARGS;
        s->input[port] = buttons;
        s->session->set_input((int)port, (uint16_t)buttons);
        return AY_OK;
    });
}

extern "C" uint32_t ay_get_input(const AySession* s, uint32_t port) {
    return (s && port <= 1) ? s->input[port] : 0u;
}

// ===========================================================================
// Audio
// ===========================================================================
extern "C" uint32_t ay_audio_events(const AySession* s, AyAudioEvent* out,
                                    uint32_t max) {
    if (!s || !s->session) return 0;
    const uint32_t total = s->session->audio_event_count();
    if (!out || max == 0) return total;
    const AytherAudioEvent* ev = s->session->audio_events();
    if (!ev) return 0;
    const uint32_t n = total < max ? total : max;
    for (uint32_t i = 0; i < n; ++i) {
        out[i].signature   = ev[i].signature;
        out[i].instrument  = ev[i].instrument;
        out[i].start_frame = ev[i].start_frame;
        out[i].end_frame   = ev[i].end_frame;
        out[i].chip        = ev[i].chip;
        out[i].channel     = ev[i].channel;
        out[i].pitch       = ev[i].pitch;
        out[i].velocity    = ev[i].velocity;
    }
    return total;   // el TOTAL, no lo copiado: así el llamador sabe si alcanzó
}

// ===========================================================================
// Capacidades — la degradación se declara
// ===========================================================================
extern "C" uint32_t ay_capabilities(const AySession* s) {
    if (!s || !s->session) return 0;
    uint32_t caps = AY_CAP_VIDEO | AY_CAP_INPUT | AY_CAP_AUDIO_EVENTS;
    if (s->session->work_ram() && s->session->work_ram_size() > 0)
        caps |= AY_CAP_MEMORY;
    // VDP e IDENTIDADES exigen el fork instrumentado. Con un core de upstream
    // las regiones vienen vacías, y un consumidor que no lo sabe lee ceros y
    // los toma por datos: preguntar es la diferencia entre «no hay» y «hay y
    // es cero».
    if (s->session->has_ayther_abi()) caps |= AY_CAP_VDP | AY_CAP_IDENTITIES;
    if (s->session->has_pack()) caps |= AY_CAP_PACK;
    return caps;
}

// ===========================================================================
// Export de frame
// ===========================================================================
extern "C" uint32_t ay_export_frame_size(const AySession* s) {
    if (!s || !s->fv || !s->fv->fb_pixels) return 0;
    return s->fv->fb_pitch * s->fv->fb_height;
}

extern "C" AyStatus ay_export_frame(const AySession* s, void* dst, uint32_t cap,
                                    AyFrame* out_desc) {
    if (!s || !out_desc) return AY_ERR_ARGS;
    if (!s->fv) return AY_ERR_STATE;
    llenar_frame(s, out_desc);
    const uint32_t required_size = ay_export_frame_size(s);
    if (required_size == 0) return AY_ERR_STATE;
    // El descriptor se llena AUNQUE no alcance: así el llamador reserva bien en
    // el segundo intento en vez de tantear.
    if (!dst || cap < required_size) return AY_ERR_CAPACITY;
    std::memcpy(dst, s->fv->fb_pixels, required_size);
    out_desc->pixels = dst;                 // ahora es del llamador

    // Los filtros de post corren sobre la COPIA, nunca sobre el buffer del
    // motor: una extensión no puede corromper el estado ni cambiar lo que ve
    // el resto de los consumidores.
    for (Ext& e : const_cast<AySession*>(s)->exts) {
        if (!e.active || e.kind != Ext::Kind::Post || !e.post) continue;
        int rc = 0;
        try {
            rc = e.post(dst, out_desc->width, out_desc->height,
                        out_desc->pitch, out_desc->format, e.user);
        } catch (...) {
            rc = -1;   // tirar es fallar: mismo camino que devolver error
        }
        if (rc != 0) {
            if (++e.failures >= AY_EXT_MAX_FAILURES) e.active = false;
        } else {
            e.failures = 0;
        }
    }
    return AY_OK;
}

// ===========================================================================
// Extensiones
// ===========================================================================
namespace {
AyStatus registrar(AySession* s, Ext e, uint32_t* id_out) {
    if (!s || !id_out) return AY_ERR_ARGS;
    e.id = s->next_id++;
    s->exts.push_back(e);
    *id_out = e.id;
    return AY_OK;
}
}  // namespace

extern "C" AyStatus ay_add_frame_observer(AySession* s, AyFrameObserver fn,
                                          void* user, uint32_t* id_out) {
    if (!fn) return AY_ERR_ARGS;
    Ext e; e.kind = Ext::Kind::Frame; e.frame = fn; e.user = user;
    return registrar(s, e, id_out);
}

extern "C" AyStatus ay_add_audio_observer(AySession* s, AyAudioObserver fn,
                                          void* user, uint32_t* id_out) {
    if (!fn) return AY_ERR_ARGS;
    Ext e; e.kind = Ext::Kind::Audio; e.audio = fn; e.user = user;
    return registrar(s, e, id_out);
}

extern "C" AyStatus ay_add_post_filter(AySession* s, AyPostFilter fn,
                                       void* user, uint32_t* id_out) {
    if (!fn) return AY_ERR_ARGS;
    Ext e; e.kind = Ext::Kind::Post; e.post = fn; e.user = user;
    return registrar(s, e, id_out);
}

extern "C" AyStatus ay_remove_extension(AySession* s, uint32_t id) {
    if (!s) return AY_ERR_ARGS;
    for (size_t i = 0; i < s->exts.size(); ++i) {
        if (s->exts[i].id == id) {
            s->exts.erase(s->exts.begin() + (ptrdiff_t)i);
            return AY_OK;
        }
    }
    return AY_ERR_ARGS;
}

extern "C" uint32_t ay_extension_failures(const AySession* s, uint32_t id) {
    if (!s) return UINT32_MAX;
    for (const Ext& e : s->exts) if (e.id == id) return e.failures;
    return UINT32_MAX;
}

extern "C" int ay_extension_active(const AySession* s, uint32_t id) {
    if (!s) return 0;
    for (const Ext& e : s->exts) if (e.id == id) return e.active ? 1 : 0;
    return 0;
}

// ===========================================================================
// Packs sin sesión ( desde C)
//
// Envuelve el lector del core. La lista de nombres se materializa al abrir y se
// conserva: `ay_pack_entry_name` devuelve punteros estables, que es lo que un
// consumidor de C espera de una función que devuelve `const char*` — recalcular
// la lista en cada llamada invalidaría el puntero anterior sin avisar.
// ===========================================================================
struct AyPack {
    AyArchive* pack = nullptr;               // del core; se cierra en ay_pack_close
    std::vector<std::string> names;          // Materialized when the pack opens.
    std::string game_id;
    ~AyPack() { if (pack) ayther_pack_close(pack); }
};

extern "C" AyPack* ay_pack_open(const char* path) {
    if (!path || !*path) return nullptr;
    AyArchive* raw = ayther_pack_open(path);
    if (!raw) return nullptr;
    auto* p = new AyPack();
    p->pack = raw;
    const uint32_t n = ayther_pack_entry_count(raw);
    p->names.reserve(n);
    char buf[512];
    for (uint32_t i = 0; i < n; ++i) {
        const int32_t w = ayther_pack_entry_name(raw, i, buf, (uint32_t)sizeof(buf));
        // Negativo = el nombre no entraba. Se reserva lo que pidió y se
        // reintenta: truncar una ruta produciría una entrada que no existe.
        if (w >= 0) {
            p->names.emplace_back(buf, (size_t)w);
        } else {
            std::vector<char> large_buffer((size_t)(-w));
            const int32_t w2 = ayther_pack_entry_name(raw, i, large_buffer.data(),
                                                       (uint32_t)large_buffer.size());
            p->names.emplace_back(large_buffer.data(), w2 > 0 ? (size_t)w2 : 0u);
        }
    }
    const char* g = ayther_pack_game_id(raw);
    p->game_id = g ? g : "";
    return p;
}

extern "C" void ay_pack_close(AyPack* p) { delete p; }

extern "C" uint32_t ay_pack_entry_count(const AyPack* p) {
    return p ? (uint32_t)p->names.size() : 0u;
}

extern "C" const char* ay_pack_entry_name(const AyPack* p, uint32_t i) {
    if (!p || i >= p->names.size()) return nullptr;
    return p->names[i].c_str();
}

extern "C" int64_t ay_pack_entry_size(const AyPack* p, const char* logical_path) {
    if (!p || !logical_path) return -1;
    return ayther_pack_file_size(p->pack, logical_path);
}

extern "C" int64_t ay_pack_read_entry(const AyPack* p, const char* logical_path,
                                      void* dst, uint32_t cap) {
    if (!p || !logical_path || !dst) return -1;
    return ayther_pack_read(p->pack, logical_path, (uint8_t*)dst, (int64_t)cap);
}

extern "C" int ay_pack_entry_streamable(const AyPack* p, const char* logical_path) {
    if (!p || !logical_path) return 0;
    return ayther_pack_entry_streamable(p->pack, logical_path) ? 1 : 0;
}

extern "C" int64_t ay_pack_read_range(const AyPack* p, const char* logical_path,
                                      uint64_t off, uint32_t len, void* dst) {
    if (!p || !logical_path || !dst) return -1;
    // El orden de los dos últimos es (buffer, largo) en el FFI del core.
    return ayther_pack_read_range(p->pack, logical_path, off, (uint8_t*)dst, len);
}

extern "C" const char* ay_pack_game_id(const AyPack* p) {
    return p ? p->game_id.c_str() : "";
}

// ---------------------------------------------------------------------------
// Compatibilidad: los cuatro grados ()
//
// Pasamanos puro sobre `ayther_pack_compat` del core. No hay lógica acá a
// propósito: el criterio vive en UN lugar (`pack_validate::compat_grade`), y
// una traducción que "ajustara" algo al pasar sería la tercera implementación
// que esta issue existe para no tener.
// ---------------------------------------------------------------------------
struct AyCompat { AytherCompat* raw = nullptr; };

extern "C" AyCompat* ay_pack_compat(const char* pack_path, const AyCompatCtx* ctx) {
    if (!pack_path || !*pack_path) return nullptr;
    AytherValidateCtx vc{};
    if (ctx) {
        vc.rom_crc32      = ctx->rom_crc32;
        vc.has_rom        = ctx->has_rom != 0;
        vc.platform       = ctx->platform;
        vc.core_build_id  = ctx->core_build_id;
        vc.engine_version = ctx->engine_version;
        vc.release_build  = ctx->release_build != 0;
    }
    AytherCompat* raw = ayther_pack_compat(pack_path, ctx ? &vc : nullptr);
    if (!raw) return nullptr;
    auto* c = new AyCompat();
    c->raw = raw;
    return c;
}

extern "C" void ay_compat_close(AyCompat* c) {
    if (!c) return;
    if (c->raw) ayther_compat_free(c->raw);
    delete c;
}

extern "C" AyCompatGrade ay_compat_grade(const AyCompat* c) {
    // Sin handle, lo seguro es el peor grado: un NULL que contestara «exacta»
    // haría que un error de uso se lea como luz verde.
    if (!c || !c->raw) return AY_COMPAT_INCOMPATIBLE;
    return (AyCompatGrade)ayther_compat_grade(c->raw);
}

extern "C" int ay_compat_runnable(const AyCompat* c) {
    return ay_compat_grade(c) != AY_COMPAT_INCOMPATIBLE ? 1 : 0;
}

extern "C" const char* ay_compat_reason(const AyCompat* c) {
    if (!c || !c->raw) return "";
    const char* s = ayther_compat_reason(c->raw);
    return s ? s : "";
}

extern "C" uint32_t ay_compat_unverified_count(const AyCompat* c) {
    return (!c || !c->raw) ? 0u : ayther_compat_unverified_count(c->raw);
}

extern "C" const char* ay_compat_unverified(const AyCompat* c, uint32_t i) {
    if (!c || !c->raw) return "";
    const char* s = ayther_compat_unverified(c->raw, i);
    return s ? s : "";
}

extern "C" const char* ay_compat_json(const AyCompat* c) {
    if (!c || !c->raw) return "{}";
    const char* s = ayther_compat_json(c->raw);
    return s ? s : "{}";
}

// -- Opciones del core (EM-7.1, ) ---------------------------------------
//
// Se devuelven crudas: partir «Descripción; a|b|c» acá obligaría a todo
// consumidor a aceptar NUESTRA forma de partirlo, y el formato es de libretro,
// no nuestro.

/// Las opciones declaradas, cacheadas en la sesión. El `const_cast` es sobre
/// un cache y no sobre estado observable: las tres funciones son `const` de
/// cara al consumidor porque preguntar no cambia nada.
static const std::vector<std::pair<std::string, std::string>>&
options_for(const AySession* s) {
    static const std::vector<std::pair<std::string, std::string>> kEmptyOptions;
    if (!s || !s->session) return kEmptyOptions;
    AySession& m = *const_cast<AySession*>(s);
    if (!m.opts_leidas) {
        m.opts = m.session->core_options_declared();
        m.opts_leidas = true;
    }
    return m.opts;
}

extern "C" uint32_t ay_core_option_count(const AySession* s) {
    return (uint32_t)options_for(s).size();
}

extern "C" const char* ay_core_option_key(const AySession* s, uint32_t i) {
    const auto& v = options_for(s);
    return i < v.size() ? v[i].first.c_str() : "";
}

extern "C" const char* ay_core_option_desc(const AySession* s, uint32_t i) {
    const auto& v = options_for(s);
    return i < v.size() ? v[i].second.c_str() : "";
}
