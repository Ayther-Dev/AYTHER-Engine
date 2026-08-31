// ---------------------------------------------------------------------------
// abi_write_control (E-4, #400) — las escrituras de control, por la ABI.
//
// POR QUÉ EXISTE. Las escrituras legacy son `*p = valor` sobre memoria del core:
// no validan bounds, no avisan del cambio de generación —el sistema de snapshots
// queda desincronizado— y zeroes impide hacerlas en pleno `retro_run`. Lo que este
// oráculo fija no es que la escritura «ande», sino que ahora la ABI DICE QUE NO
// cuando corresponde: escribir una región de sólo lectura tiene que fallar, y un
// core sin ABI tiene que seguir andando por el camino viejo.
//
// El efecto se verifica DONDE SE VE: la máscara de capas en 0 tiene que apagar
// el framebuffer, y restaurarla tiene que devolverlo. Un test que sólo mirara el
// status diría OK aunque el control no hiciera zeroes.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target abi_write_control
//   Env:   AYTHER_PROBE_ROM (opcional: sin ella corre con la ROM sintética del
//          repo) · AYTHER_ABI_CORE · AYTHER_STOCK_CORE
// ---------------------------------------------------------------------------
#include "libretro_host/retro_runner.h"
#include "ayther_env.h"

#include "../common/synth_rom.h"

#include <cstdint>
#include <cstdio>
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

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const std::string root = AYTHER_SOURCE_DIR;
    const std::string rom  = ayther::synth::probe_rom_path();   // #415: sintética si no hay otra
    const std::string core = env_or("AYTHER_ABI_CORE",
        root + "/third_party/cores/genesis_plus_gx_libretro_vram.dll");
    const std::string stock = env_or("AYTHER_STOCK_CORE",
        root + "/third_party/cores/genesis_plus_gx_libretro.dll");

    std::printf("=== abi_write_control (E-4 #400) ===\n");
    if (rom.empty() || !fs::exists(rom) || !fs::exists(core)) {
        std::printf("[skip] falta ROM o core\n"); return 77;
    }

    RetroRunner r;
    if (!r.init(core, rom)) { std::fprintf(stderr, "[FAIL] init\n"); return 1; }
    if (!r.has_ayther_v1()) { std::printf("[skip] core sin ABI\n"); return 77; }

    const ayther_interface_v1* api = r.ayther_api();
    ayther_subscription_state_v1 subs{};
    subs.struct_size = sizeof(subs);
    api->get_subscriptions(&subs, sizeof(subs));
    api->set_subscriptions(AYTHER_SUB_ALL & subs.supported_mask);
    for (int i = 0; i < 400; ++i) r.run_frame();   // hasta una pantalla con algo

    // ---- Lo que la ABI puede decir y el puntero crudo no --------------------
    {
        // VRAM es de sólo LECTURA: escribirla tiene que fallar explícitamente.
        // Con el camino legacy esto no existía — el puntero era mutable y una
        // escritura equivocada corrompía el core sin una palabra.
        const uint8_t basura[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        const auto ro = r.write_control_v1(AYTHER_REGION_VRAM, basura, 4);
        check(ro.status == AYTHER_STATUS_READ_ONLY,
              "escribir una region de solo lectura da READ_ONLY");
        std::printf("       status=%d (READ_ONLY=%d)\n", ro.status,
                    AYTHER_STATUS_READ_ONLY);
    }

    // ---- El control TIENE EFECTO donde se ve -------------------------------
    // Sumar el framebuffer es suficiente: con todas las capas apagadas la imagen
    // tiene que cambiar, y al restaurarlas tiene que volver.
    uint64_t normal_sum = 0, disabled_sum = 0, restored_sum = 0;
    {
        auto framebuffer_sum = [&]() -> uint64_t {
            uint64_t acc = 0; int n = 0;
            // The row sum walks `pitch` bytes per line, so the frame width is
            // not read here; naming it would be a parameter nobody uses.
            r.set_video_callback([&](const void* px, unsigned /*width*/,
                                     unsigned h, size_t pitch) {
                if (!px || n++) return;   // sólo el primer frame del tick
                const uint8_t* p = static_cast<const uint8_t*>(px);
                for (unsigned y = 0; y < h; ++y)
                    for (size_t x = 0; x < pitch; ++x) acc += p[y * pitch + x];
            });
            r.run_frame();
            r.set_video_callback(nullptr);
            return acc;
        };
        normal_sum = framebuffer_sum();

        const auto w0 = r.set_layer_mask_v1(0x00);
        check(w0.ok(), "set_layer_mask_v1(0x00) acepta");
        disabled_sum = framebuffer_sum();
        check(disabled_sum != normal_sum,
              "…y el frame CAMBIA (el control tiene efecto real)");

        const auto w1 = r.set_layer_mask_v1(0x0F);
        check(w1.ok(), "set_layer_mask_v1(0x0F) acepta");
        restored_sum = framebuffer_sum();
        check(restored_sum != disabled_sum,
              "…y restaurar las capas vuelve a cambiar el frame");
        std::printf("       fb: normal=%llu  apagado=%llu  restaurado=%llu\n",
                    (unsigned long long)normal_sum,
                    (unsigned long long)disabled_sum,
                    (unsigned long long)restored_sum);
    }

    // ---- La supresion de TILES DE PLANO tiene efecto? ----------------------
    // El fork tenia un flag aparte (0x106, PLANE_SUPPRESS_ACTIVE) sin el cual
    // escribir la mascara de planos era un no-op SILENCIOSO. Hoy nadie lo
    // enciende — ni por el id legacy ni por su region v1. O el core ya no lo
    // necesita, o la supresion de planos no hace zeroes y nadie se entero. Se
    // mide donde se ve: suprimir TODOS los tiles de los 3 planos tiene que
    // cambiar el frame.
    {
        auto framebuffer_sum = [&]() -> uint64_t {
            uint64_t acc = 0; int n = 0;
            // The row sum walks `pitch` bytes per line, so the frame width is
            // not read here; naming it would be a parameter nobody uses.
            r.set_video_callback([&](const void* px, unsigned /*width*/,
                                     unsigned h, size_t pitch) {
                if (!px || n++) return;
                const uint8_t* p = static_cast<const uint8_t*>(px);
                for (unsigned y = 0; y < h; ++y)
                    for (size_t x = 0; x < pitch; ++x) acc += p[y * pitch + x];
            });
            r.run_frame();
            r.set_video_callback(nullptr);
            return acc;
        };
        std::vector<uint8_t> suppression_mask(3 * 1024, 0xFF);   // los 3 planos, todo suprimido
        const uint64_t before = framebuffer_sum();
        const auto w = r.set_plane_tile_suppress_v1(suppression_mask.data(),
                                                    (uint32_t)suppression_mask.size());
        check(w.ok(), "set_plane_tile_suppress_v1(todo) acepta");
        const uint64_t suppressed = framebuffer_sum();
        check(suppressed != before,
              "suprimir TODOS los tiles de plano CAMBIA el frame (no es no-op)");
        std::vector<uint8_t> zeroes(3 * 1024, 0x00);
        r.set_plane_tile_suppress_v1(zeroes.data(), (uint32_t)zeroes.size());
        const uint64_t restored = framebuffer_sum();
        check(restored != suppressed, "…y soltarla lo devuelve");
        std::printf("       fb planos: normal=%llu suprimido=%llu restaurado=%llu\n",
                    (unsigned long long)before, (unsigned long long)suppressed,
                    (unsigned long long)restored);
    }

    // ---- La escritura mueve la generacion ----------------------------------
    {
        const auto a = r.set_layer_dim_v1(1);
        const auto b = r.set_layer_dim_v1(0);
        check(a.ok() && b.ok(), "set_layer_dim_v1 acepta encender y apagar");
        check(a.new_generation != 0 && b.new_generation >= a.new_generation,
              "cada escritura devuelve una generacion (el snapshot no queda desfasado)");
        std::printf("       gen: %llu → %llu\n",
                    (unsigned long long)a.new_generation,
                    (unsigned long long)b.new_generation);
    }

    // ---- Mute de canales: acepta y no rompe la emulacion -------------------
    {
        const auto m = r.set_audio_mute_v1(0x0001);   // FM ch0
        check(m.ok(), "set_audio_mute_v1(FM0) acepta");

        // REPLAY-SAFE (#327): silenciar un canal NO puede apagar el chip — el
        // hasher tiene que seguir viendo las escrituras, porque la identidad
        // del audio se construye sobre la secuencia de comandos y no sobre el
        // PCM. Si el mute se comiera las escrituras, la autoría hecha con un
        // canal en silencio dejaría de reconocer ese sonido al reproducir.
        auto writes_during = [&](int frames) {
            uint32_t total = 0;
            for (int i = 0; i < frames; ++i) {
                r.run_frame();
                ayther_frame_snapshot_v1 s{};
                if (r.capture_frame_snapshot(s).ok()) total += s.audio_write_count;
            }
            return total;
        };
        const uint32_t with_mute = writes_during(120);
        const auto m2 = r.set_audio_mute_v1(0x0000);
        check(m2.ok(), "…y se puede desmutear");
        // CONTROL: el mismo tramo SIN mute. Sin esto, un juego que no toca los
        // chips en ese momento se lee como «el mute se comió las escrituras» —
        // el mismo error de leer una métrica sin su línea de base.
        const uint32_t without_mute = writes_during(120);
        std::printf("       escrituras: con mute=%u  sin mute=%u\n",
                    with_mute, without_mute);
        if (without_mute == 0)
            std::printf("       [aviso] el juego no escribe a los chips en este "
                        "tramo — replay-safe SIN ejercitar\n");
        else
        check(with_mute > 0,
                  "con FM0 muteado el chip SIGUE registrando escrituras (replay-safe)");
    }
    r.shutdown();

    // ---- Con core STOCK todo esto es no-op, sin crash ----------------------
    if (fs::exists(stock)) {
        RetroRunner s;
        if (s.init(stock, rom)) {
            check(!s.has_ayther_v1(), "el core stock no tiene ABI");
            // E-5 (#401): ya no hay «camino legacy» que probar — los setters
            // mutables se eliminaron y estos son los ÚNICOS. Lo que se fija
            // acá es que con un core sin ABI digan UNSUPPORTED en vez de
            // fallar en silencio (que era exactamente el vicio del viejo).
            const uint8_t bits16[16] = {0};
            const auto w  = s.set_layer_mask_v1(0x0F);
            const auto w2 = s.set_layer_dim_v1(0);
            const auto w3 = s.set_sprite_suppress_v1(bits16);
            const auto w4 = s.set_audio_mute_v1(0);
            check(w.status == AYTHER_STATUS_UNSUPPORTED,
                  "los set_*_v1 con core stock son no-op (UNSUPPORTED, sin crash)");
            check(w2.status == AYTHER_STATUS_UNSUPPORTED &&
                  w3.status == AYTHER_STATUS_UNSUPPORTED &&
                  w4.status == AYTHER_STATUS_UNSUPPORTED,
                  "…los cuatro, no sólo el primero");
            s.run_frame();
            check(true, "…y el core sigue corriendo");
            s.shutdown();
        }
    }

    std::printf("\n%s — %d checks, %d fallos\n",
                g_fails ? "=== FAIL ===" : "=== OK ===", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
