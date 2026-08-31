// ---------------------------------------------------------------------------
// abi_multilayer (E-7, #406) — ¿la extracción multicapa del core sirve para lo
// que E-7 quiere construir encima?
//
// POR QUÉ EXISTE. E-7 propone reemplazar la extracción multipasada por UNA
// llamada a `ayther_recompose_multilayer`, que devuelve las cuatro capas
// nativas del VDP más el composite. Antes de invertir en el cableado —y sobre
// todo antes de que #231 (widescreen) se apoye en esto— hay tres cosas que la
// issue da por ciertas y que este oráculo mide:
//
//   1. Que el composite recompuesto sea el frame REAL. Se compara contra el
//      framebuffer del emulador, y se exige BIT-EXACTO en los frames donde el
//      propio core dice que no hubo actividad raster (`fallback_reasons == 0`).
//      Ahí el contrato de R-1 no admite grises: recomponer desde el estado
//      final tiene que dar exactamente lo mismo. En los frames CON actividad se
//      reporta el % de coincidencia como dato, no como veredicto.
//   2. Que las capas se separen de verdad. Cuatro buffers idénticos entre sí
//      serían una extracción que no extrae — y pasarían cualquier check que
//      sólo mire el composite.
//   3. Que recomponer no altere la emulación. La función corre el MISMO
//      renderer del core sobre el estado vivo del VDP y lo restaura al salir;
//      si la restauración fuera incompleta, la autoría empezaría a divergir del
//      original por el solo hecho de estar mirándolo. Se verifica corriendo la
//      misma ROM los mismos frames con y sin las llamadas.
//
// Y mide dos cosas que la issue asume del contrato y el core NO da:
//   · los `AYTHER_RC_ERR_*` con nombre propio (la issue quiere el motivo en el
//     log, no un «falló»);
//   · el caché por `frame_generation`.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target abi_multilayer
//   Env:   AYTHER_PROBE_ROM (opcional: sin ella corre con la ROM sintética del
//          repo) · AYTHER_ABI_CORE (default: el del repo)
//          AYTHER_ML_FRAMES (default 300) · AYTHER_ML_WARMUP (default 60)
// ---------------------------------------------------------------------------
#include "../common/synth_rom.h"

#include "libretro_host/retro_runner.h"
#include "ayther_session.h"
#include "ayther_env.h"

#include <memory>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_checks = 0, g_fails = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

std::string env_or(const char* k, const std::string& d) {
    if (const char* v = ayther::env_get(k)) if (*v) return v;
    return d;
}
int env_int(const char* k, int d) {
    if (const char* v = ayther::env_get(k)) if (*v) return std::atoi(v);
    return d;
}

using clk = std::chrono::steady_clock;
double ms_since(clk::time_point t) {
    return std::chrono::duration<double, std::milli>(clk::now() - t).count();
}

uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ULL) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

// El export directo del fork. NO vive en el descriptor `ayther_interface_v1`:
// se resuelve con core_sym<>() sobre el módulo que cargó ESTE runner.
using MultilayerFn = int32_t (AYTHER_CALL *)(
    uint16_t*, uint16_t*, uint16_t*, uint16_t*, uint16_t*,
    uint32_t, uint32_t, uint32_t*, uint32_t*);

constexpr size_t kMaxPixels = 320 * 300;   // el tope que el core cachea

/// De dónde salió la multicapa. Se reporta porque el camino CAMBIÓ con la ABI
/// 1.2 y un oráculo que no lo dijera dejaría el cambio invisible.
struct Multilayer { MultilayerFn fn = nullptr; const char* via = "no disponible"; };

/// Descriptor primero (es donde vive desde 1.2), export suelto después (1.0/1.1
/// siguen siendo cores válidos: la ABI es aditiva).
template <typename Runner>
Multilayer resolver_multilayer(Runner& r, const ayther_interface_v1* api) {
    if (api && AYTHER_IFACE_HAS(api, recompose_multilayer) && api->recompose_multilayer)
        return { reinterpret_cast<MultilayerFn>(api->recompose_multilayer),
                 "descriptor (ABI >= 1.2)" };
    if (auto fn = r.template core_sym<MultilayerFn>("ayther_recompose_multilayer"))
        return { fn, "export suelto (ABI 1.0/1.1)" };
    return {};
}

/// El framebuffer del frame, compactado (sin pitch) desde el video callback.
struct Fb {
    std::vector<uint16_t> px;
    unsigned w = 0, h = 0;
    bool     rgb565 = true;
    void set(const void* data, unsigned ww, unsigned hh, size_t pitch) {
        if (!data || !ww || !hh) { w = h = 0; return; }
        w = ww; h = hh;
        px.resize(size_t(w) * h);
        const uint8_t* src = static_cast<const uint8_t*>(data);
        for (unsigned y = 0; y < h; ++y)
            std::memcpy(&px[size_t(y) * w], src + size_t(y) * pitch, size_t(w) * 2);
    }
};

/// Corre la ROM N frames y devuelve el hash de RAM + framebuffer acumulado.
/// `ml` != nullptr → además llama a la recomposición en cada frame, que es
/// justamente lo que se quiere probar que NO cambia nada.
uint64_t run_session(const std::string& core, const std::string& rom, int warmup,
                     int frames, bool with_recompose, uint64_t* fb_hash_out) {
    RetroRunner r;
    if (!r.init(core, rom)) return 0;
    Fb fb;
    r.set_video_callback([&](const void* d, unsigned w, unsigned h, size_t p) {
        fb.set(d, w, h, p);
    });
    if (r.has_ayther_v1()) {
        const ayther_interface_v1* api = r.ayther_api();
        ayther_subscription_state_v1 st{};
        st.struct_size = sizeof(st);
        api->get_subscriptions(&st, sizeof(st));
        api->set_subscriptions(AYTHER_SUB_ALL & st.supported_mask);
    }
    auto ml = resolver_multilayer(r, r.ayther_api()).fn;
    std::vector<uint16_t> a(kMaxPixels), b(kMaxPixels), win(kMaxPixels),
                          spr(kMaxPixels), comp(kMaxPixels);

    uint64_t fb_hash = 1469598103934665603ULL;
    for (int i = 0; i < warmup + frames; ++i) {
        r.set_input(0, (i % 64 == 0) ? 0x0008 : 0x0000);
        r.run_frame();
        if (with_recompose && ml && i >= warmup) {
            uint32_t w = 0, h = 0;
            ml(a.data(), b.data(), win.data(), spr.data(), comp.data(),
               uint32_t(kMaxPixels), 0, &w, &h);
        }
        if (i >= warmup && !fb.px.empty())
            fb_hash = fnv(fb.px.data(), fb.px.size() * 2, fb_hash);
    }
    if (fb_hash_out) *fb_hash_out = fb_hash;
    const uint64_t ram = r.work_ram()
        ? fnv(r.work_ram(), r.work_ram_size()) : 0;
    r.shutdown();
    return ram;
}

}  // namespace

int main() {
    // Sin buffer: este oraculo entra al renderer del core por un export directo,
    // y si algo revienta ahi la salida bufferada se pierde entera — que fue
    // exactamente lo que paso la primera vez que corrio.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    namespace fs = std::filesystem;
    const std::string root = AYTHER_SOURCE_DIR;
    const std::string rom  = ayther::synth::probe_rom_path();   // #415: sintética si no hay otra
    const std::string core = env_or("AYTHER_ABI_CORE",
        root + "/third_party/cores/genesis_plus_gx_libretro_vram.dll");
    const int n_frames = env_int("AYTHER_ML_FRAMES", 300);
    const int n_warmup = env_int("AYTHER_ML_WARMUP", 60);

    std::printf("=== abi_multilayer (E-7 #406) — extraccion multicapa del core ===\n");
    if (rom.empty() || !fs::exists(rom) || !fs::exists(core)) {
        std::printf("[skip] falta ROM o core\n"); return 77;
    }

    RetroRunner r;
    if (!r.init(core, rom)) { std::fprintf(stderr, "[FAIL] init\n"); return 1; }
    if (!r.has_ayther_v1()) {
        std::printf("[skip] el core no trae ABI v1\n"); return 77;
    }
    const ayther_interface_v1* api = r.ayther_api();

    // La multicapa se resuelve por LOS DOS CAMINOS, y el oráculo dice cuál usó.
    // Hasta la ABI 1.1 era un export suelto; desde 1.2 vive en el descriptor y
    // el símbolo dejó de exportarse en el perfil standard. Exigir el export
    // directo —como hacía este check— convierte un core nuevo y sano en un
    // rojo, y peor: esconde que el camino que la sesión usa cambió.
    const Multilayer ml_res = resolver_multilayer(r, api);
    auto ml = ml_res.fn;
    check(ml != nullptr,
          "la multicapa se resuelve (descriptor desde ABI 1.2, export suelto en 1.0/1.1)");
    std::printf("       via: %s\n", ml_res.via);
    if (!ml) { std::printf("\n=== FAIL ===\n"); return 1; }

    Fb fb;
    r.set_video_callback([&](const void* d, unsigned w, unsigned h, size_t p) {
        fb.set(d, w, h, p);
    });
    check(r.pixel_format() == 2,
          "el core entrega RGB565 — el mismo layout que devuelve la recomposicion");

    std::vector<uint16_t> a(kMaxPixels), b(kMaxPixels), win(kMaxPixels),
                          spr(kMaxPixels), comp(kMaxPixels);

    // ---- Sin suscripcion: la recomposicion es opt-in -----------------------
    {
        r.run_frame();
        uint32_t w = 0, h = 0;
        const int32_t rc = ml(a.data(), b.data(), win.data(), spr.data(),
                              comp.data(), uint32_t(kMaxPixels), 0, &w, &h);
        check(rc == AYTHER_STATUS_NOT_SUBSCRIBED,
              "sin suscripcion dice NOT_SUBSCRIBED (no devuelve un frame mudo)");
        std::printf("       status=%d (NOT_SUBSCRIBED=%d)\n",
                    rc, AYTHER_STATUS_NOT_SUBSCRIBED);
    }

    ayther_subscription_state_v1 subs{};
    subs.struct_size = sizeof(subs);
    api->get_subscriptions(&subs, sizeof(subs));
    api->set_subscriptions(AYTHER_SUB_ALL & subs.supported_mask);

    for (int i = 0; i < n_warmup; ++i) {
        r.set_input(0, (i % 64 == 0) ? 0x0008 : 0x0000);
        r.run_frame();
    }
    r.set_input(0, 0);

    // ---- Biseccion: que combinacion de buffers sobrevive -------------------
    // La primera version de este oraculo pedia los cinco de una y el proceso se
    // caia con ACCESS_VIOLATION sin decir donde. Esto lo acota antes de entrar
    // al bucle, y de paso queda como diagnostico para el proximo que toque el
    // renderer del core.
    {
        struct Case { const char* name; bool a, b, w, s, c; };
        const Case cases[] = {
            { "solo composite",        false, false, false, false, true  },
            { "composite + plano A",   true,  false, false, false, true  },
            { "composite + sprites",   false, false, false, true,  true  },
            { "los cinco",             true,  true,  true,  true,  true  },
        };
        for (const Case& k : cases) {
            uint32_t w = 0, h = 0;
            std::printf("  [bisec] %-22s ... ", k.name);
            const int32_t rc = ml(k.a ? a.data() : nullptr,
                                  k.b ? b.data() : nullptr,
                                  k.w ? win.data() : nullptr,
                                  k.s ? spr.data() : nullptr,
                                  k.c ? comp.data() : nullptr,
                                  uint32_t(kMaxPixels), 0, &w, &h);
            std::printf("rc=%d  %ux%u\n", rc, w, h);
        }
    }

    // ---- La corrida --------------------------------------------------------
    uint64_t clean_pixels = 0, matching_clean_frames = 0;      // frames sin actividad raster
    uint64_t dirty_pixels  = 0, matching_dirty_frames  = 0;      // frames CON actividad raster
    uint32_t clean_frame_count = 0, dirty_frame_count = 0, exact_frame_count = 0, call_count = 0;
    uint32_t worst_frame = 0; uint64_t worst_mismatch = 0;
    uint32_t distinct_layer_count = 0, non_empty_layer_count = 0, layer_example_count = 0;
    uint32_t scene_frame_count = 0;
    double   ms_ml = 0.0;
    bool     dims_ok = true, status_ok = true;

    for (int f = 0; f < n_frames; ++f) {
        r.run_frame();
        ayther_frame_snapshot_v1 s{};
        if (!r.capture_frame_snapshot(s).ok()) continue;
        if (fb.px.empty()) continue;

        uint32_t w = 0, h = 0;
        const clk::time_point t0 = clk::now();
        const int32_t rc = ml(a.data(), b.data(), win.data(), spr.data(),
                              comp.data(), uint32_t(kMaxPixels), 0, &w, &h);
        ms_ml += ms_since(t0);
        if (rc != AYTHER_STATUS_OK) { status_ok = false; continue; }
        ++call_count;
        if (w != fb.w || h != fb.h) dims_ok = false;

        const size_t n = size_t(fb.w) * fb.h;
        uint64_t matching_pixels = 0;
        for (size_t i = 0; i < n; ++i) if (comp[i] == fb.px[i]) ++matching_pixels;
        const uint64_t mismatch = n - matching_pixels;

        if (s.fallback_reasons == 0) {
            ++clean_frame_count; clean_pixels += n; matching_clean_frames += matching_pixels;
            if (!mismatch) ++exact_frame_count;
            if (mismatch > worst_mismatch) { worst_mismatch = mismatch; worst_frame = uint32_t(f); }
        } else {
            ++dirty_frame_count; dirty_pixels += n; matching_dirty_frames += matching_pixels;
        }

        // Las capas tienen que ser CAPAS: si los cuatro buffers salieran
        // iguales, la extraccion no extrajo nada y el composite solo no lo
        // delata.
        // Un frame UNIFORME (pantalla de un solo color: fundidos, el logo con el
        // display apagado, los negros entre pantallas) tiene legitimamente las
        // cuatro capas iguales, y exigirle separacion es exigirle al core que
        // invente. La pregunta con sentido es sobre los frames CON has_scene_content.
        bool has_scene_content = false;
        for (size_t i = 1; i < n && !has_scene_content; ++i) if (comp[i] != comp[0]) has_scene_content = true;
        if (!has_scene_content) continue;
        ++scene_frame_count;

        const bool a_vs_b    = std::memcmp(a.data(), b.data(),    n * 2) != 0;
        const bool a_vs_spr  = std::memcmp(a.data(), spr.data(),  n * 2) != 0;
        // Y el que de verdad prueba que esto EXTRAE: si el composite fuera
        // identico a una capa suelta, no habria separacion ninguna.
        const bool comp_vs_a = std::memcmp(comp.data(), a.data(), n * 2) != 0;
        if (a_vs_b && a_vs_spr && comp_vs_a) ++distinct_layer_count;
        else if (layer_example_count < 4) {
            std::printf("       frame %d CON has_scene_content y capas pegadas: "
                        "A!=B? %s  A!=sprites? %s  composite!=A? %s\n", f,
                        a_vs_b ? "si" : "NO", a_vs_spr ? "si" : "NO",
                        comp_vs_a ? "si" : "NO");
            ++layer_example_count;
        }
        bool plane_a_has_pixels = false, sprite_layer_has_pixels = false;
        for (size_t i = 0; i < n && !(plane_a_has_pixels && sprite_layer_has_pixels); ++i) {
            if (a[i]   != a[0])   plane_a_has_pixels   = true;
            if (spr[i] != spr[0]) sprite_layer_has_pixels = true;
        }
        if (plane_a_has_pixels && sprite_layer_has_pixels) ++non_empty_layer_count;
    }

    std::printf("\n  --- %d frames tras %d de warmup ---\n", n_frames, n_warmup);
    std::printf("  llamadas OK: %u   %.3f ms por llamada (5 buffers)\n",
                call_count, ms_ml / (call_count ? call_count : 1));
    std::printf("  frames SIN actividad raster: %u  (exact_matches: %u)\n", clean_frame_count, exact_frame_count);
    if (clean_pixels)
        std::printf("     match %.4f%%   peor frame: %u con %llu px distintos\n",
                    100.0 * double(matching_clean_frames) / double(clean_pixels),
                    worst_frame, (unsigned long long)worst_mismatch);
    std::printf("  frames CON actividad raster: %u\n", dirty_frame_count);
    if (dirty_pixels)
        std::printf("     match %.4f%%\n", 100.0 * double(matching_dirty_frames) / double(dirty_pixels));

    check(status_ok && call_count > 0, "la recomposicion responde OK con suscripcion");
    check(dims_ok, "devuelve las dimensiones del frame real");
    check(clean_frame_count > 0,
          "NO-VACUIDAD: hubo frames sin actividad raster que comparar bit a bit");
    check(clean_frame_count > 0 && exact_frame_count == clean_frame_count,
          "sin actividad raster, el composite es BIT-EXACTO al frame del emulador");
    std::printf("  frames CON has_scene_content: %u (de %u) · capas separadas en %u · "
                "con contenido propio en %u\n",
                scene_frame_count, call_count, distinct_layer_count, non_empty_layer_count);
    check(scene_frame_count > 0,
          "NO-VACUIDAD: hubo frames con has_scene_content (no una pantalla de un solo color)");
    // No se exige separacion en TODOS los frames con has_scene_content: una pantalla de
    // titulo con un solo plano activo tiene legitimamente composite == plano A,
    // y una sin sprites tiene la capa de sprites pegada al fondo. Exigirlo seria
    // pedirle al core que invente contenido. Lo que si tiene que existir —y es
    // lo que prueba que esto EXTRAE— son frames donde los cuatro planos se
    // separan de verdad.
    check(distinct_layer_count > 0,
          "hay frames donde las capas se separan (A != B != sprites y composite != A)");
    check(non_empty_layer_count > 0,
          "las capas extraidas traen contenido propio, no solo backdrop");

    // ---- Los AYTHER_RC_ERR_* que la issue quiere loguear -------------------
    {
        uint32_t w = 0, h = 0;
        const int32_t rc = ml(a.data(), b.data(), win.data(), spr.data(),
                              comp.data(), /*pixel_capacity=*/1, 0, &w, &h);
        std::printf("\n  capacidad insuficiente → status=%d "
                    "(RC_INVALID_PARAMS=%d · el viejo UNSUPPORTED para todo=%d)\n",
                    rc, AYTHER_STATUS_RC_INVALID_PARAMS, AYTHER_STATUS_UNSUPPORTED);
        check(rc == AYTHER_STATUS_RC_INVALID_PARAMS,
              "el motivo del rechazo llega TIPIFICADO al frontend, no aplastado");
    }

    // ---- El cache por frame_generation --------------------------------------
    {
        const bool has_stats = AYTHER_IFACE_HAS(api, get_recompose_stats) &&
                               api->get_recompose_stats != nullptr &&
                               api->recompose_stats_size >= sizeof(ayther_recompose_stats_v1);
        check(has_stats,
              "el core expone estadisticas para verificar el cache sin medir tiempos");
        if (has_stats) {
            ayther_recompose_stats_v1 before{}, after_first{}, after_second{};
            uint32_t w = 0, h = 0;
            const int32_t before_rc =
                api->get_recompose_stats(&before, sizeof(before));
            r.run_frame();
            const int32_t first_rc = ml(a.data(), b.data(), win.data(), spr.data(),
                                        comp.data(), uint32_t(kMaxPixels), 0, &w, &h);
            const int32_t after_first_rc =
                api->get_recompose_stats(&after_first, sizeof(after_first));
            const int32_t second_rc = ml(a.data(), b.data(), win.data(), spr.data(),
                                         comp.data(), uint32_t(kMaxPixels), 0, &w, &h);
            const int32_t after_second_rc =
                api->get_recompose_stats(&after_second, sizeof(after_second));

            const bool calls_ok = before_rc == AYTHER_STATUS_OK &&
                                  first_rc == AYTHER_STATUS_OK &&
                                  after_first_rc == AYTHER_STATUS_OK &&
                                  second_rc == AYTHER_STATUS_OK &&
                                  after_second_rc == AYTHER_STATUS_OK;
            check(calls_ok, "las estadisticas y ambas recomposiciones responden OK");
            std::printf("\n  cache multilayer: calls %llu → %llu → %llu; "
                        "hits %llu → %llu → %llu\n",
                        static_cast<unsigned long long>(before.multilayer_calls),
                        static_cast<unsigned long long>(after_first.multilayer_calls),
                        static_cast<unsigned long long>(after_second.multilayer_calls),
                        static_cast<unsigned long long>(before.multilayer_hits),
                        static_cast<unsigned long long>(after_first.multilayer_hits),
                        static_cast<unsigned long long>(after_second.multilayer_hits));
            check(calls_ok &&
                      after_first.multilayer_calls == before.multilayer_calls + 1 &&
                      after_second.multilayer_calls == after_first.multilayer_calls + 1 &&
                      after_first.multilayer_hits == before.multilayer_hits &&
                      after_second.multilayer_hits == after_first.multilayer_hits + 1,
                  "el MISMO frame no se vuelve a renderizar (hay cache por generacion)");
        }
    }

    r.shutdown();

    // ---- Recomponer no puede alterar la emulacion ---------------------------
    {
        uint64_t framebuffer_with = 0, framebuffer_without = 0;
        const uint64_t ram_with = run_session(core, rom, n_warmup, n_frames, true,  &framebuffer_with);
        const uint64_t ram_without = run_session(core, rom, n_warmup, n_frames, false, &framebuffer_without);
        std::printf("\n  con recompose: ram=%016llX fb=%016llX\n"
                    "  sin recompose: ram=%016llX fb=%016llX\n",
                    (unsigned long long)ram_with, (unsigned long long)framebuffer_with,
                    (unsigned long long)ram_without, (unsigned long long)framebuffer_without);
        check(ram_with && ram_with == ram_without,
              "llamar a la recomposicion NO altera la emulacion (RAM del 68k)");
        check(framebuffer_with && framebuffer_with == framebuffer_without,
              "…ni el video que el emulador produce (framebuffer de todos los frames)");
    }

    // ---- Y ahora por la API que el Engine expone (E-7) ---------------------
    // Todo lo de arriba entra al core por el export directo. Esto ejercita el
    // camino que van a usar los consumidores reales: `AytherSession::
    // recompose_layers()`, con sus buffers, su gate y su aborto transaccional.
    // Verificarlo por el runner y NO por la sesion dejaria el cableado sin
    // cubrir, que es donde se rompen estas cosas.
    {
        std::printf("\n  --- por AytherSession::recompose_layers() ---\n");
        ayther::AytherSession::Config c;
        c.core_path = core;
        c.rom_path  = rom;
        c.enable_audio = false;
        auto sr = ayther::AytherSession::create(c);
        if (!sr) {
            std::fprintf(stderr, "[FAIL] no se pudo crear la sesion: %s\n",
                         sr.error.message.c_str());
            ++g_checks; ++g_fails;
        } else {
            std::unique_ptr<ayther::AytherSession>& s = *sr;

            // ANTES de correr un frame no hay nada que recomponer: el gate
            // tiene que decirlo y no devolver medio resultado.
            {
                const auto empty_result = s->recompose_layers();
                check(!empty_result.ok() && empty_result.composite == nullptr &&
                          empty_result.bg_a == nullptr && empty_result.width == 0,
                      "sin frame producido no devuelve NADA (transaccional, no medio frame)");
                std::printf("       motivo: \"%s\"\n", s->layers_error());
                check(s->layers_error()[0] != '\0',
                      "…y deja un motivo legible, no un codigo mudo");
            }

            for (int i = 0; i < n_warmup; ++i) {
                s->set_input(0, (i % 64 == 0) ? 0x0008 : 0x0000);
                s->step();
            }
            s->set_input(0, 0);

            uint32_t exact_matches = 0, frames_run = 0;
            const uint16_t* prev_comp = nullptr;
            bool same_buffers = true;
            for (int f = 0; f < 60; ++f) {
                const ayther::FrameView& fv = s->step();
                const auto L = s->recompose_layers();
                if (!L.ok()) continue;
                ++frames_run;
                if (prev_comp && L.composite != prev_comp) same_buffers = false;
                prev_comp = L.composite;
                if (L.width != fv.fb_width || L.height != fv.fb_height) continue;
                if (!fv.fb_pixels) continue;
                const auto* src = static_cast<const uint8_t*>(fv.fb_pixels);
                bool identical = true;
                for (uint32_t y = 0; y < L.height && identical; ++y)
                    identical = std::memcmp(L.composite + size_t(y) * L.width,
                                        src + size_t(y) * fv.fb_pitch,
                                        size_t(L.width) * 2) == 0;
                if (identical) ++exact_matches;
            }
            std::printf("       %u llamadas · %u con composite identico al frame\n",
                        frames_run, exact_matches);
            check(frames_run > 0, "la sesion entrega capas tras step()");
            check(exact_matches > 0,
                  "el composite de la sesion coincide EXACTO con su propio frame");
            check(same_buffers,
                  "los buffers se REUSAN entre frames (no hay un malloc por capa a 60 Hz)");
            check(s->layers_error()[0] == '\0',
                  "tras una llamada buena, el motivo queda limpio");
        }
    }

    std::printf("\n%s — %d checks, %d fallos\n",
                g_fails ? "=== FAIL ===" : "=== OK ===", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
