#include "libretro_host/retro_runner.h"
#include "ayther_core_ffi.h"   //  EM-7.4: el patcher IPS/BPS
#include "ayther_env.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

// ---------------------------------------------------------------------------
// Thread-scoped callback target — libretro callbacks are plain C and provide
// no userdata pointer. CallbackScope gives synchronous entry points stack-like
// binding semantics without process-global writable dispatch state.
// ---------------------------------------------------------------------------
thread_local RetroRunner* RetroRunner::s_active_instance_ = nullptr;

RetroRunner::CallbackScope::CallbackScope(RetroRunner& runner) noexcept
    : previous_(s_active_instance_) {
    s_active_instance_ = &runner;
}

RetroRunner::CallbackScope::~CallbackScope() noexcept {
    s_active_instance_ = previous_;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------
RetroRunner::RetroRunner()  = default;

RetroRunner::~RetroRunner() { shutdown(); }

// ---------------------------------------------------------------------------
// init — load DLL + ROM, wire callbacks, kick off the core
// ---------------------------------------------------------------------------
bool RetroRunner::init(const std::string& core_path, const std::string& rom_path) {
    if (!loader_.load(core_path)) return false;
    if (!load_symbols())          return false;

    CallbackScope callback_scope{*this};
    // El core pregunta por el system directory ANTES de cargar el juego (en
    // retro_init), así que hay que tenerlo resuelto acá.
    //
    // Por defecto es el directorio de la ROM, que es donde el propio core cae
    // cuando el frontend no contesta. `AYTHER_SYSTEM_DIR` lo mueve: los BIOS
    // (Sega CD, MD, Game Genie) no tienen por qué vivir mezclados con los
    // juegos, y de hecho acá no lo hacen — las ISOs están en la carpeta de
    // ROMs del usuario y los BIOS en el repo.
    {
        const char* env = ayther::env_get("AYTHER_SYSTEM_DIR");
        if (env && *env) {
            system_dir_ = env;
        } else {
            const size_t sep = rom_path.find_last_of("/\\");
            system_dir_ = sep == std::string::npos ? std::string(".")
                                                   : rom_path.substr(0, sep);
            if (system_dir_.empty()) system_dir_ = ".";
        }
        std::fprintf(stdout, "[RetroRunner] system dir: %s\n", system_dir_.c_str());
    }

    // ¿El medio es un DISCO? Es lo que distingue al Sega CD, y con él a los dos
    // chips que sólo existen ahí: el RF5C164 y el CDDA. Importa para el audio —
    // el router de voces no espeja ninguno de los dos, así que en este medio su
    // mezcla se SUMA al buffer del core en vez de ocupar su lugar ().
    //
    // Por la EXTENSIÓN y no por el contenido: es la misma decisión que toma el
    // core al elegir qué hardware montar, es determinista antes del primer
    // frame, y no depende de que el juego llegue a tocar el chip. `.bin` queda
    // afuera a propósito: en Mega Drive es un cartucho.
    {
        const size_t dot = rom_path.find_last_of('.');
        std::string ext = dot == std::string::npos ? std::string()
                                                   : rom_path.substr(dot + 1);
        for (char& c : ext) c = static_cast<char>(std::tolower(c));
        cd_media_ = ext == "iso" || ext == "cue" || ext == "chd";
    }

    fn_retro_set_environment(s_environment);
    fn_retro_set_video_refresh(s_video_refresh);
    fn_retro_set_audio_sample(s_audio_sample);
    fn_retro_set_audio_sample_batch(s_audio_sample_batch);
    fn_retro_set_input_poll(s_input_poll);
    fn_retro_set_input_state(s_input_state);

    fn_retro_init();

    retro_system_info sysinfo{};
    fn_retro_get_system_info(&sysinfo);
    std::fprintf(stdout, "[RetroRunner] Core: %s %s\n",
                 sysinfo.library_name, sysinfo.library_version);

    if (!load_rom(rom_path)) return false;

    // Cache RAM pointer — 64 KB 68000 work RAM
    ram_ptr_  = fn_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    ram_size_ = fn_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    std::fprintf(stdout, "[RetroRunner] Work RAM: %p  size: %zu bytes\n",
                 ram_ptr_, ram_size_);

    running_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// run_frame — advance one hardware tick
// ---------------------------------------------------------------------------
void RetroRunner::run_frame() {
    if (!running_) return;
    // Multi-instancia (, shadow core): los trampolines libretro rutean por
    CallbackScope callback_scope{*this};
    fn_retro_run();
    poll_frame_delta_();   // E-6 (): el delta es de ESTE frame
}

// ---------------------------------------------------------------------------
// E-6 (): Frame Delta Stream
//
// Se pollea acá y no en el caller por el contrato del core: el poll es
// CONSUME-ON-POLL (vaciarlo es lo que hace que el próximo traiga lo ensuciado
// desde este). Con dos consumidores compitiendo, el segundo vería un delta
// vacío y creería que no cambió nada. Un solo poll, en el único punto por el
// que pasan todos los frames, y el dato queda en `last_delta_` para quien lo
// necesite.
// ---------------------------------------------------------------------------
void RetroRunner::poll_frame_delta_() {
    last_delta_ok_ = false;
    if (!ayther_api_) return;
    // Las TRES condiciones. El bit de capability solo, no alcanza: un core que
    // lo declare pero traiga un descriptor más corto (compilado contra una
    // versión anterior del struct) haría que leer `poll_frame_delta` sea leer
    // fuera del objeto del otro lado del DLL.
    if (!(ayther_api_->capabilities & AYTHER_CAP_FRAME_DELTA_V1)) return;
    if (ayther_api_->struct_size < sizeof(ayther_interface_v1))   return;
    if (!ayther_api_->poll_frame_delta)                           return;

    ayther_frame_delta_v1 d{};
    if (ayther_api_->poll_frame_delta(&d, sizeof(d)) != AYTHER_STATUS_OK) return;
    // Y el struct que devolvió tiene que ser el que este Engine entiende: un
    // core más nuevo con otro layout se descarta en vez de interpretarse mal.
    if (d.struct_size   != sizeof(d))                    return;
    if (d.delta_version != AYTHER_LAYOUT_FRAME_DELTA_V1) return;

    last_delta_    = d;
    last_delta_ok_ = true;
}

// ---------------------------------------------------------------------------
// shutdown
// ---------------------------------------------------------------------------
void RetroRunner::shutdown() {
    if (!running_) return;
    CallbackScope callback_scope{*this}; // unload/deinit may invoke callbacks
    fn_retro_unload_game();
    fn_retro_deinit();
    running_ = false;
    // E-1 (): el descriptor es propiedad del core y muere con él — dejarlo
    // colgando sería un puntero a un módulo que ya no está.
    ayther_api_              = nullptr;
    fn_ayther_get_interface_ = nullptr;
}

// ---------------------------------------------------------------------------
// load_symbols — resolve every function pointer from the DLL
// ---------------------------------------------------------------------------
bool RetroRunner::load_symbols() {
    bool ok = true;
    auto get = [&]<typename T>(T& fn, const char* name) {
        fn = loader_.sym<T>(name);
        if (!fn) ok = false;
    };

    get(fn_retro_set_environment,       "retro_set_environment");
    get(fn_retro_set_video_refresh,     "retro_set_video_refresh");
    get(fn_retro_set_audio_sample,      "retro_set_audio_sample");
    get(fn_retro_set_audio_sample_batch,"retro_set_audio_sample_batch");
    get(fn_retro_set_input_poll,        "retro_set_input_poll");
    get(fn_retro_set_input_state,       "retro_set_input_state");
    get(fn_retro_init,                  "retro_init");
    get(fn_retro_deinit,                "retro_deinit");
    get(fn_retro_load_game,             "retro_load_game");
    get(fn_retro_unload_game,           "retro_unload_game");
    get(fn_retro_run,                   "retro_run");
    get(fn_retro_get_memory_data,       "retro_get_memory_data");
    get(fn_retro_cheat_set,             "retro_cheat_set");
    get(fn_retro_cheat_reset,           "retro_cheat_reset");
    get(fn_retro_get_memory_size,       "retro_get_memory_size");
    get(fn_retro_get_system_info,       "retro_get_system_info");
    get(fn_retro_get_system_av_info,    "retro_get_system_av_info");
    // Savestates + reset (R2 base). Not fatal if absent — some cores lack them.
    fn_retro_serialize_size = loader_.sym<size_t(*)()>("retro_serialize_size");
    fn_retro_serialize      = loader_.sym<bool(*)(void*, size_t)>("retro_serialize");
    fn_retro_unserialize    = loader_.sym<bool(*)(const void*, size_t)>("retro_unserialize");
    fn_retro_reset          = loader_.sym<void(*)()>("retro_reset");

    // E-1 (): negociar la ABI AYTHER v1 si el core la exporta.
    //
    // NO participa de `ok` a propósito: un core stock —o un fork anterior a
    // `f3fe1e1`— no trae el símbolo, y eso no es un fallo de carga sino el
    // camino legacy (`retro_get_memory_data(0x100-0x10E)`), que sigue vivo.
    // El log deja dicho cuál de los dos mundos quedó activo, porque desde
    // afuera los dos arrancan igual y la diferencia recién se nota cuando algo
    // no responde.
    fn_ayther_get_interface_ =
        loader_.sym<ayther_get_interface_fn>("ayther_get_interface");
    if (fn_ayther_get_interface_) {
        // Se pide 1.0 y se acepta lo que venga con major 1: la ABI es aditiva
        // (1.9 es «seis versiones aditivas» sobre 1.3), así que un minor mayor
        // trae el mismo puntero con más campos, y cada campo nuevo se gatea
        // por capability + struct_size, nunca por `minor ==`.
        ayther_api_ = fn_ayther_get_interface_(AYTHER_ABI_VERSION_1_0);
        if (!ayther_api_) {
            std::fprintf(stderr,
                "[RetroRunner] ayther_get_interface devolvio NULL para v1.0 — "
                "core con ABI pero de otra version\n");
        } else if (AYTHER_ABI_VERSION_MAJOR(ayther_api_->abi_version) != 1) {
            std::fprintf(stderr,
                "[RetroRunner] ABI AYTHER major %u (se entiende la 1) — se "
                "sigue por el camino legacy\n",
                AYTHER_ABI_VERSION_MAJOR(ayther_api_->abi_version));
            ayther_api_ = nullptr;
        } else {
            std::fprintf(stdout,
                "[RetroRunner] ABI AYTHER v%u.%u negociada. build_id=%.*s  "
                "caps=0x%016llX\n",
                AYTHER_ABI_VERSION_MAJOR(ayther_api_->abi_version),
                AYTHER_ABI_VERSION_MINOR(ayther_api_->abi_version),
                static_cast<int>(ayther_api_->build_id_size),
                ayther_api_->build_id,
                static_cast<unsigned long long>(ayther_api_->capabilities));
        }
    } else {
        std::fprintf(stdout,
            "[RetroRunner] sin ayther_get_interface — core stock/legacy\n");
    }

    return ok;
}

// ---------------------------------------------------------------------------
// E-3 (): lecturas por la ABI v1.
//
// Todas comparten la misma forma: sin ABI devuelven UNSUPPORTED (el caller cae
// al camino legacy), y con ABI la lectura pasa por `read_region`, que valida la
// generación contra el snapshot. Esa validación es la diferencia real con el
// puntero crudo: si el frame avanzó entre el snapshot y la lectura, el core lo
// DICE en vez de devolver memoria de otro instante.
// ---------------------------------------------------------------------------
RetroRunner::AytherReadResult
RetroRunner::capture_frame_snapshot(ayther_frame_snapshot_v1& out) const {
    AytherReadResult r;
    if (!ayther_api_) return r;
    out = ayther_frame_snapshot_v1{};
    out.struct_size = sizeof(out);
    r.status = ayther_api_->capture_snapshot(&out, sizeof(out));
    if (r.status == AYTHER_STATUS_OK) r.generation = out.snapshot_generation;
    return r;
}

RetroRunner::AytherReadResult
RetroRunner::read_region_v1(uint32_t region, void* out, uint32_t bytes,
                            uint64_t generation) const {
    AytherReadResult r;
    if (!ayther_api_ || !out || !bytes) return r;
    r.status = ayther_api_->read_region(region, 0, out, bytes,
                                        generation, &r.generation);
    if (r.status == AYTHER_STATUS_OK) r.count = bytes;
    return r;
}

// Las cuatro regiones del VDP comparten cuerpo: el tamaño lo declara el core
// (query_region), así que el Engine no hardcodea 64 KB de VRAM ni 128 B de CRAM
// — si el core cambiara esos tamaños, la lectura sigue siendo correcta.
RetroRunner::AytherReadResult
RetroRunner::read_vdp_region_(uint32_t region, void* out,
                              const ayther_frame_snapshot_v1& s) const {
    AytherReadResult r;
    if (!ayther_api_ || !out) return r;
    ayther_region_info_v1 info{};
    info.struct_size = sizeof(info);
    if (ayther_api_->query_region(region, &info, sizeof(info)) != AYTHER_STATUS_OK)
        return r;
    if (!info.byte_size) { r.status = AYTHER_STATUS_OK; return r; }
    r = read_region_v1(region, out, info.byte_size, s.snapshot_generation);
    return r;
}

size_t RetroRunner::abi_region_bytes(uint32_t region) const {
    if (!ayther_api_ || !ayther_api_->query_region) return 0;
    ayther_region_info_v1 info{};
    info.struct_size = sizeof(info);
    if (ayther_api_->query_region(region, &info, sizeof(info)) != AYTHER_STATUS_OK)
        return 0;
    return static_cast<size_t>(info.byte_size);
}

RetroRunner::AytherReadResult
RetroRunner::read_vram_v1(void* out, const ayther_frame_snapshot_v1& s) const {
    return read_vdp_region_(AYTHER_REGION_VRAM, out, s);
}
RetroRunner::AytherReadResult
RetroRunner::read_cram_v1(void* out, const ayther_frame_snapshot_v1& s) const {
    return read_vdp_region_(AYTHER_REGION_CRAM, out, s);
}
RetroRunner::AytherReadResult
RetroRunner::read_vdp_regs_v1(void* out, const ayther_frame_snapshot_v1& s) const {
    return read_vdp_region_(AYTHER_REGION_VDP_REGS, out, s);
}
RetroRunner::AytherReadResult
RetroRunner::read_vsram_v1(void* out, const ayther_frame_snapshot_v1& s) const {
    return read_vdp_region_(AYTHER_REGION_VSRAM, out, s);
}

RetroRunner::AytherReadResult
RetroRunner::read_parsed_sprites_v1(ayther_sprite_v1* out, uint32_t max,
                                    const ayther_frame_snapshot_v1& s) const {
    AytherReadResult r;
    if (!ayther_api_ || !out) return r;
    // (std::min) entre parentesis: windows.h define min como MACRO y la
    // llamada sin parentesis no compila.
    const uint32_t n = (std::min)(max, s.parsed_sprite_count);
    if (n == 0) { r.status = AYTHER_STATUS_OK; return r; }

    ayther_region_info_v1 info{};
    info.struct_size = sizeof(info);
    if (ayther_api_->query_region(AYTHER_REGION_PARSED_SPRITES, &info,
                                  sizeof(info)) != AYTHER_STATUS_OK)
        return r;

    if (info.element_size > sizeof(ayther_sprite_v1)) {
        // FORWARD-COMPAT: el core tiene campos que este Engine todavía no
        // conoce. Se lee con SU stride y se copian sólo los bytes del struct
        // conocido — leer con el stride propio desalinearía todo el array, que
        // es el modo de falla que el puntero legacy tenía latente.
        std::vector<uint8_t> tmp(static_cast<size_t>(n) * info.element_size);
        r.status = ayther_api_->read_region(
            AYTHER_REGION_PARSED_SPRITES, 0, tmp.data(),
            static_cast<uint32_t>(tmp.size()), s.snapshot_generation,
            &r.generation);
        if (r.status == AYTHER_STATUS_OK) {
            for (uint32_t i = 0; i < n; ++i)
                std::memcpy(&out[i], tmp.data() + size_t(i) * info.element_size,
                            sizeof(ayther_sprite_v1));
            r.count = n;
        }
        return r;
    }
    r.status = ayther_api_->read_region(
        AYTHER_REGION_PARSED_SPRITES, 0, out,
        n * static_cast<uint32_t>(sizeof(ayther_sprite_v1)),
        s.snapshot_generation, &r.generation);
    if (r.status == AYTHER_STATUS_OK) r.count = n;
    return r;
}

RetroRunner::AytherReadResult
RetroRunner::read_audio_writes_v1(ayther_audio_write_v1* out, uint32_t max,
                                  const ayther_frame_snapshot_v1& s) const {
    AytherReadResult r;
    if (!ayther_api_ || !out) return r;
    const uint32_t n = (std::min)(max, s.audio_write_count);
    if (n == 0) { r.status = AYTHER_STATUS_OK; return r; }
    r.status = ayther_api_->read_region(
        AYTHER_REGION_AUDIO_WRITES, 0, out,
        n * static_cast<uint32_t>(sizeof(ayther_audio_write_v1)),
        s.snapshot_generation, &r.generation);
    if (r.status == AYTHER_STATUS_OK) r.count = n;
    return r;
}

uint32_t RetroRunner::read_raster_fallback_v1(
    const ayther_frame_snapshot_v1& s) const {
    return ayther_api_ ? s.fallback_reasons : 0u;
}

RetroRunner::AytherReadResult
RetroRunner::read_system_v1(ayther_system_v1& out) const {
    AytherReadResult r;
    if (!ayther_api_ || !ayther_api_->read_region) return r;
    if (!(ayther_api_->capabilities & AYTHER_CAP_SYSTEM_V1)) return r;
    out = ayther_system_v1{};
    out.struct_size    = sizeof(out);
    out.layout_version = AYTHER_LAYOUT_SYSTEM_V1;
    // Sin suscripción y sin generación: SYSTEM se llena al leer y describe el
    // contenido cargado, no un frame.
    r.status = ayther_api_->read_region(AYTHER_REGION_SYSTEM, 0, &out, sizeof(out),
                                        AYTHER_GENERATION_ANY, &r.generation);
    if (r.status == AYTHER_STATUS_OK &&
        (out.struct_size < sizeof(out) || out.layout_version != AYTHER_LAYOUT_SYSTEM_V1)) {
        // Un layout que este Engine no entiende se descarta en vez de leerse
        // corrido — mismo criterio que poll_frame_delta_.
        r.status = AYTHER_STATUS_UNSUPPORTED;
    }
    if (r.status == AYTHER_STATUS_OK) r.count = 1;
    return r;
}

uint32_t RetroRunner::poll_audio_events_v1(ayther_audio_event_v1* out,
                                           uint32_t max) const {
    if (!ayther_api_ || !out || !max || !ayther_api_->poll_audio_events) return 0;
    if (!(ayther_api_->capabilities & AYTHER_CAP_AUDIO_PROBE_V1)) return 0;
    // El TAMAÑO lo declara el core. Si algún día no coincide con el struct que
    // conoce este Engine, leer con el `sizeof` local devolvería los campos
    // corridos sin que nada falle — y el layout de este evento ya cambió una
    // vez (pasó a ser una unión). Ante la duda no se lee nada.
    if (ayther_api_->get_audio_transport_stats) {
        ayther_audio_transport_stats_v1 st{};
        st.struct_size = sizeof(st);
        if (ayther_api_->get_audio_transport_stats(&st, sizeof(st)) == AYTHER_STATUS_OK &&
            st.event_size && st.event_size != sizeof(ayther_audio_event_v1)) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::fprintf(stderr,
                    "[RetroRunner] el core dice event_size=%u y este Engine "
                    "conoce %zu — no se leen eventos de audio\n",
                    st.event_size, sizeof(ayther_audio_event_v1));
            }
            return 0;
        }
    }
    uint32_t n = 0;
    if (ayther_api_->poll_audio_events(out, max, &n) != AYTHER_STATUS_OK) return 0;
    return n;
}

uint32_t RetroRunner::audio_events_dropped() const {
    if (!ayther_api_ || !ayther_api_->get_audio_transport_stats) return 0;
    ayther_audio_transport_stats_v1 st{};
    st.struct_size = sizeof(st);
    if (ayther_api_->get_audio_transport_stats(&st, sizeof(st)) != AYTHER_STATUS_OK)
        return 0;
    return st.dropped_events;
}

RetroRunner::AytherWriteResult
RetroRunner::write_control_v1(uint32_t region, const void* data, uint32_t bytes,
                              uint64_t expected_generation) const {
    AytherWriteResult r;
    if (!ayther_api_ || !data || !bytes) return r;
    r.status = ayther_api_->write_control(region, 0, data, bytes,
                                          expected_generation,
                                          &r.new_generation);
    return r;
}

// ---------------------------------------------------------------------------
// load_rom — read the ROM file into memory and call retro_load_game
// ---------------------------------------------------------------------------
bool RetroRunner::load_rom(const std::string& rom_path) {
    std::ifstream file(rom_path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::fprintf(stderr, "[RetroRunner] Cannot open ROM: %s\n", rom_path.c_str());
        return false;
    }

    const auto size = static_cast<size_t>(file.tellg());
    file.seekg(0);
    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));

    //  EM-7.4: el parche del usuario, EN RAM. El archivo del disco no se
    // toca — quien prueba un romhack no puede perder su ROM por eso.
    if (!patch_path_.empty()) {
        std::ifstream pf(patch_path_, std::ios::binary | std::ios::ate);
        if (!pf) {
            std::fprintf(stderr, "[RetroRunner] no se pudo abrir el parche: %s\n",
                         patch_path_.c_str());
            return false;
        }
        const auto pn = static_cast<size_t>(pf.tellg());
        pf.seekg(0);
        std::vector<uint8_t> patch(pn);
        pf.read(reinterpret_cast<char*>(patch.data()), (std::streamsize)pn);

        uint32_t necesita = 0;
        // Se pregunta el tamano primero: un parche puede EXTENDER la ROM (hay
        // hacks que agregan contenido), asi que reservar el tamano de la
        // original truncaria el resultado sin avisar.
        ayther_apply_rom_patch(data.data(), (uint32_t)data.size(),
                               patch.data(), (uint32_t)patch.size(),
                               nullptr, 0, &necesita);
        std::vector<uint8_t> parcheada(necesita ? necesita : data.size());
        const int64_t n = ayther_apply_rom_patch(
            data.data(), (uint32_t)data.size(),
            patch.data(), (uint32_t)patch.size(),
            parcheada.data(), (uint32_t)parcheada.size(), &necesita);
        if (n < 0) {
            char err[256] = "";
            ayther_rom_patch_error(err, (uint32_t)sizeof(err));
            // Se FALLA en vez de seguir con la ROM sin parchear: arrancar el
            // juego en ingles cuando el usuario pidio la traduccion parece que
            // el parche no hizo nada, y nadie sabe por que.
            std::fprintf(stderr, "[RetroRunner] el parche no se pudo aplicar: %s\n",
                         err[0] ? err : "motivo desconocido");
            return false;
        }
        parcheada.resize((size_t)n);
        data.swap(parcheada);
        std::fprintf(stdout, "[RetroRunner] parche aplicado: %s  (%zu KB -> %zu KB)\n",
                     patch_path_.c_str(), size / 1024, data.size() / 1024);
    }

    std::fprintf(stdout, "[RetroRunner] ROM: %s  (%zu KB)\n",
                 rom_path.c_str(), size / 1024);

    retro_game_info game_info{};
    game_info.path = rom_path.c_str();
    game_info.data = data.data();
    game_info.size = data.size();   //  EM-7.4: el parche puede extenderla

    if (!fn_retro_load_game(&game_info)) {
        std::fprintf(stderr, "[RetroRunner] retro_load_game failed\n");
        return false;
    }

    retro_system_av_info av{};
    fn_retro_get_system_av_info(&av);
    fps_ = (av.timing.fps > 0.0) ? av.timing.fps : 60.0;
    std::fprintf(stdout, "[RetroRunner] AV: %ux%u @ %.4f fps\n",
                 av.geometry.base_width, av.geometry.base_height, fps_);
    return true;
}

// ---------------------------------------------------------------------------
// Static C trampolines
// ---------------------------------------------------------------------------
bool RetroRunner::s_environment(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *static_cast<bool*>(data) = true;
        return true;

    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        // Store the format so the TileHasher (Phase 2) and Vulkan renderer
        // (Phase 3) can interpret the framebuffer bytes correctly.
        if (s_active_instance_)
            s_active_instance_->pixel_format_ = *static_cast<const unsigned*>(data);
        return true;

    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY:
        // El directorio de la ROM, no "." — el CWD del proceso depende de
        // quién invoque (el Lab, un probe desde build/bin, ctest) y ahí no
        // hay BIOS. Con esto, `bios_CD_U.bin` junto a la ISO alcanza para que
        // un juego de Sega CD arranque, que es lo que el core espera.
        *static_cast<const char**>(data) =
            s_active_instance_ ? s_active_instance_->system_dir_.c_str() : ".";
        return true;

    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        // EM-7.1 (): el valor que el FRONTEND eligió para esta opción.
        //
        // Antes esto devolvía false siempre —«no soportamos opciones»— y el
        // core caía a sus defaults. Con eso, «sin límite de sprites» (el
        // anti-flicker) y el overclock eran inalcanzables desde Play aunque el
        // core los ofreciera.
        //
        // Sin valor elegido se sigue devolviendo false, que NO es un error: es
        // «el frontend no opina», y el core usa su default. Devolver una cadena
        // vacía sería otra cosa —una opción puesta en nada— y algunos cores la
        // interpretan como valor válido.
        auto* var = static_cast<retro_variable*>(data);
        if (!s_active_instance_ || !var || !var->key) return false;
        const auto it = s_active_instance_->core_options_.find(var->key);
        if (it == s_active_instance_->core_options_.end() || it->second.empty())
            return false;
        // El puntero tiene que sobrevivir a la llamada: apunta al string del
        // mapa, que vive lo que vive el runner. Copiarlo a un buffer temporal
        // es el error clásico acá — el core guarda el puntero, no el texto.
        var->value = it->second.c_str();
        return true;
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        // Las opciones NO cambian en runtime. Hay que escribir el bool* a false:
        // devolver true sin setearlo dejaba basura → el core creía que cambiaron
        // y re-aplicaba las opciones cada frame, reinicializando el chip de sonido
        // (audio mudo). Con esto el core lee opciones una sola vez.
        if (data) *static_cast<bool*>(data) = false;
        return true;

    case RETRO_ENVIRONMENT_SET_VARIABLES: {
        // EM-7.1: lo que el core DECLARA ofrecer. Se guarda para que un
        // frontend pueda presentarlas sin hardcodear la lista de ningún core
        // — con BYOC no sabemos cuál van a usar, y una lista nuestra quedaría
        // vieja con cada core nuevo.
        //
        // El formato de `value` es «Descripción; opción1|opción2|...». Se parte
        // en la descripción y se deja el resto: quien las ofrezca necesita las
        // dos partes, y volver a parsear del lado del consumidor sería tener
        // dos parsers del mismo formato.
        if (s_active_instance_ && data) {
            s_active_instance_->declared_options_.clear();
            for (auto* v = static_cast<const retro_variable*>(data);
                 v && v->key; ++v) {
                s_active_instance_->declared_options_.emplace_back(
                    v->key, v->value ? v->value : "");
            }
        }
        return true;
    }
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_ROTATION:
    case RETRO_ENVIRONMENT_GET_OVERSCAN:
        return true;  // silently ignore

    default:
        return false; // unsupported, let the core decide what to do
    }
}

void RetroRunner::s_video_refresh(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (s_active_instance_ && s_active_instance_->video_cb_)
        s_active_instance_->video_cb_(data, w, h, pitch);
    // Phase 1: no rendering — frame is silently dropped
}

void RetroRunner::s_audio_sample(int16_t, int16_t) {}

size_t RetroRunner::s_audio_sample_batch(const int16_t* data, size_t frames) {
    if (s_active_instance_ && s_active_instance_->audio_cb_)
        return s_active_instance_->audio_cb_(data, frames);
    return frames;
}
void   RetroRunner::s_input_poll() {}

// Reads the injected per-port button bitfield (set via set_input).
// Bit i corresponds to RETRO_DEVICE_ID_JOYPAD_* id i. Handles the JOYPAD_MASK
// query (id 256) that some cores use to fetch all buttons at once.
int16_t RetroRunner::s_input_state(unsigned port, unsigned device,
                                   unsigned /*index*/, unsigned id) {
    if (!s_active_instance_)          return 0;
    if (device != RETRO_DEVICE_JOYPAD) return 0;
    if (static_cast<int>(port) >= kPorts) return 0;
    const uint16_t buttons = s_active_instance_->input_[port];
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return static_cast<int16_t>(buttons);
    if (id >= 16)                     return 0;
    return (buttons >> id) & 1;
}
