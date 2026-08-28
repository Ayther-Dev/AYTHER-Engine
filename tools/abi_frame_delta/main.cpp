// ---------------------------------------------------------------------------
// abi_frame_delta (E-6, #405) — ¿el Frame Delta Stream alcanza para dejar de
// leer VRAM entera cada frame?
//
// POR QUÉ EXISTE. E-6 propone reemplazar la lectura de los 64 KiB de VRAM por
// frame (`refresh_abi_mirror`) por una invalidación selectiva guiada por
// `ayther_frame_delta_v1::dirty_patterns` — el array `bg_name_dirty[0x800]` del
// VDP, un byte por *pattern name* (32 bytes de VRAM cada uno). Eso sólo es
// válido si el bitmask es un SUPERCONJUNTO de lo que realmente cambió: un solo
// pattern que cambie sin estar marcado deja el espejo con bytes de un frame
// anterior, y el síntoma no es un error sino un tile viejo dibujándose — lejos
// de acá y mucho después.
//
// Este oráculo no le cree al contrato: compara, frame a frame, la VRAM leída
// entera contra la del frame previo, y confronta los patterns que REALMENTE
// cambiaron con los que el delta marcó.
//
//   · falso NEGATIVO (cambió y no está marcado) → el delta no sirve de
//     invalidación. Es el check que decide la issue.
//   · falso POSITIVO (marcado y no cambió) → benigno: se lee de más.
//
// Y mide lo que la issue pide medir: el costo de leer VRAM entera vs leer sólo
// los rangos marcados, sobre los mismos frames.
//
// NO-VACUIDAD: si en toda la corrida no cambió ni un pattern, no se comparó
// nada y el verde no significa nada — eso se reporta como FAIL, no como OK.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target abi_frame_delta
//   Env:   AYTHER_PROBE_ROM (opcional: sin ella corre con la ROM sintética del
//          repo) · AYTHER_ABI_CORE (default: el del repo)
//          AYTHER_DELTA_FRAMES (default 240) · AYTHER_DELTA_WARMUP (default 600)
// ---------------------------------------------------------------------------
#include "libretro_host/retro_runner.h"
#include "ayther_env.h"

#include "../common/synth_rom.h"

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

constexpr uint32_t kPatterns     = 2048;   // = sizeof(dirty_patterns)
constexpr uint32_t kPatternBytes = 32;     // 8 líneas × 4 bytes (4bpp)

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const std::string root = AYTHER_SOURCE_DIR;
    const std::string rom  = ayther::synth::probe_rom_path();   // #415: sintética si no hay otra
    const std::string core = env_or("AYTHER_ABI_CORE",
        root + "/third_party/cores/genesis_plus_gx_libretro_ayther_x64.dll");   // el del core.lock
    // El warmup por defecto es CORTO a propósito: la ventana tiene que caer
    // donde hay actividad de las dos cosas que se miden. Con 1800 frames Sonic 2
    // queda en una pantalla quieta y la corrida entera pasa con 0 patterns
    // cambiados — verde que no prueba nada, y por eso el check de no-vacuidad
    // está abajo. Con 60 hay carga de tiles Y eventos raster.
    const int n_frames = env_int("AYTHER_DELTA_FRAMES", 300);
    const int n_warmup = env_int("AYTHER_DELTA_WARMUP", 60);

    std::printf("=== abi_frame_delta (E-6 #405) — fidelidad del dirty_patterns ===\n");
    if (rom.empty() || !fs::exists(rom) || !fs::exists(core)) {
        std::printf("[skip] falta ROM o core\n"); return 77;
    }

    RetroRunner r;
    if (!r.init(core, rom)) { std::fprintf(stderr, "[FAIL] init\n"); return 1; }
    if (!r.has_ayther_v1()) {
        std::printf("[skip] el core no trae ABI v1\n"); return 77;
    }
    const ayther_interface_v1* api = r.ayther_api();

    // Capability + puntero: las dos cosas, porque un core puede declarar el bit
    // y traer el descriptor viejo (más corto) sin el campo.
    const bool cap = (api->capabilities & AYTHER_CAP_FRAME_DELTA_V1) != 0;
    const bool ptr_ok = api->struct_size >= sizeof(ayther_interface_v1) &&
                        api->poll_frame_delta != nullptr;
    std::printf("  capability FRAME_DELTA_V1=%d  poll_frame_delta=%p  "
                "frame_delta_size=%u (local %zu)\n",
                (int)cap, (const void*)api->poll_frame_delta,
                ptr_ok ? api->frame_delta_size : 0u,
                sizeof(ayther_frame_delta_v1));
    if (!cap || !ptr_ok) {
        std::printf("[skip] el core no publica Frame Delta — nada que medir\n");
        return 77;
    }
    check(api->frame_delta_size == sizeof(ayther_frame_delta_v1),
          "el frame_delta_size del core == el struct que conoce este Engine");

    ayther_subscription_state_v1 subs{};
    subs.struct_size = sizeof(subs);
    api->get_subscriptions(&subs, sizeof(subs));
    api->set_subscriptions(AYTHER_SUB_ALL & subs.supported_mask);

    const size_t vram_bytes = r.abi_region_bytes(AYTHER_REGION_VRAM);
    if (vram_bytes != size_t(kPatterns) * kPatternBytes) {
        std::printf("[aviso] VRAM = %zu bytes; el delta cubre %u — se compara el solape\n",
                    vram_bytes, kPatterns * kPatternBytes);
    }
    if (!vram_bytes) { std::fprintf(stderr, "[FAIL] VRAM de 0 bytes\n"); return 1; }

    // Warmup: comparar la pantalla negra del boot haría pasar cualquier cosa.
    for (int i = 0; i < n_warmup; ++i) {
        r.set_input(0, (i % 64 == 0) ? 0x0008 : 0x0000);   // START
        r.run_frame();
    }
    r.set_input(0, 0);

    std::vector<uint8_t> prev(vram_bytes, 0), cur(vram_bytes, 0), inc(vram_bytes, 0);
    ayther_frame_snapshot_v1 snap{};
    if (!r.capture_frame_snapshot(snap).ok() || !r.read_vram_v1(prev.data(), snap).ok()) {
        std::fprintf(stderr, "[FAIL] no se pudo leer la VRAM inicial\n"); return 1;
    }
    inc = prev;   // el espejo incremental arranca de una lectura completa

    // ---- La corrida --------------------------------------------------------
    uint64_t total_changed = 0, total_marked = 0, total_false_negatives = 0, total_false_positives = 0;
    uint64_t frames_with_false_negatives = 0, incremental_bytes_read = 0;
    double   ms_full = 0.0, ms_inc = 0.0;
    int      example_count = 0;
    bool     poll_ok = true, gen_ok = true, espejo_ok = true;
    // El otro dato que el delta trae y el snapshot no: el tamaño del raster
    // journal, que es lo que E-6 tiene que propagar como insumo de #406. Un
    // contador que no baja nunca sería un journal que no se vacía — y el replay
    // de #12C estaría recomponiendo con eventos de frames viejos.
    uint32_t raster_min = UINT32_MAX, raster_max = 0, raster_first = 0, raster_last = 0;
    uint64_t raster_decrease_frames = 0;
    bool     counts_coinciden = true;

    for (int f = 0; f < n_frames; ++f) {
        r.run_frame();

        // Por el camino del ENGINE, no llamando al core de vuelta: `run_frame()`
        // ya polleó, y el poll del core es CONSUME-ON-POLL. Un segundo poll acá
        // devolvería un delta vacío y este oráculo reportaría falsos negativos
        // que no existen — o peor, al revés: si algún día alguien agrega otro
        // poll en el Engine, es acá donde se va a ver.
        if (!r.has_frame_delta()) {
            poll_ok = false;
            std::printf("  [aviso] el runner no tiene delta en el frame %d\n", f);
            break;
        }
        const ayther_frame_delta_v1& d = r.frame_delta();
        const clk::time_point t_inc0 = clk::now();

        // Camino INCREMENTAL: sólo los rangos marcados, coalesciendo runs
        // contiguos (una llamada por run, no una por pattern).
        uint32_t p = 0;
        while (p < kPatterns) {
            if (!d.dirty_patterns[p]) { ++p; continue; }
            const uint32_t ini = p;
            while (p < kPatterns && d.dirty_patterns[p]) ++p;
            const uint32_t off = ini * kPatternBytes;
            const uint32_t len = (p - ini) * kPatternBytes;
            if (off + len <= vram_bytes) {
                uint64_t gen = 0;
                api->read_region(AYTHER_REGION_VRAM, off, inc.data() + off, len,
                                 AYTHER_GENERATION_ANY, &gen);
                incremental_bytes_read += len;
            }
        }
        ms_inc += ms_since(t_inc0);

        // Camino ACTUAL: la VRAM entera, por snapshot validado.
        const clk::time_point t_full0 = clk::now();
        ayther_frame_snapshot_v1 s{};
        if (!r.capture_frame_snapshot(s).ok()) continue;
        if (!r.read_vram_v1(cur.data(), s).ok()) continue;
        ms_full += ms_since(t_full0);

        if (d.frame_generation != s.frame_generation) gen_ok = false;
        if (d.parsed_sprite_count != s.parsed_sprite_count ||
            d.audio_write_count   != s.audio_write_count) counts_coinciden = false;
        if (f == 0) raster_first = d.raster_event_count;
        if (f > 0 && d.raster_event_count < raster_last) ++raster_decrease_frames;
        raster_last = d.raster_event_count;
        if (d.raster_event_count < raster_min) raster_min = d.raster_event_count;
        if (d.raster_event_count > raster_max) raster_max = d.raster_event_count;

        // ---- El veredicto: lo que cambió vs lo que el delta marcó ----------
        uint32_t frame_false_negatives = 0;
        const uint32_t n_pat = uint32_t(vram_bytes / kPatternBytes) < kPatterns
                             ? uint32_t(vram_bytes / kPatternBytes) : kPatterns;
        for (uint32_t i = 0; i < n_pat; ++i) {
            const size_t off = size_t(i) * kPatternBytes;
            const bool changed = std::memcmp(&prev[off], &cur[off], kPatternBytes) != 0;
            const bool marked = d.dirty_patterns[i] != 0;
            if (changed) ++total_changed;
            if (marked) ++total_marked;
            if (changed && !marked) {
                ++total_false_negatives; ++frame_false_negatives;
                if (example_count < 8) {
                    std::printf("       falso negativo: frame %d pattern %u "
                                "(VRAM 0x%04zX)\n", f, i, off);
                    ++example_count;
                }
            }
            if (marked && !changed) ++total_false_positives;
        }
        if (frame_false_negatives) ++frames_with_false_negatives;

        // El espejo incremental, comparado con la verdad del frame. Es el mismo
        // dato que los falsos negativos, pero visto como lo vería el Engine:
        // bytes que un espejo guiado por el delta estaría publicando de más.
        if (std::memcmp(inc.data(), cur.data(), vram_bytes) != 0) espejo_ok = false;

        prev.swap(cur);
    }

    // ---- Resultados --------------------------------------------------------
    std::printf("\n  --- %d frames tras %d de warmup ---\n", n_frames, n_warmup);
    std::printf("  patterns que CAMBIARON de verdad : %llu\n", (unsigned long long)total_changed);
    std::printf("  patterns MARCADOS por el delta   : %llu\n", (unsigned long long)total_marked);
    std::printf("  falsos NEGATIVOS (cambio, sin marca): %llu  en %llu frames\n",
                (unsigned long long)total_false_negatives, (unsigned long long)frames_with_false_negatives);
    std::printf("  falsos POSITIVOS (marca, sin cambio): %llu\n",
                (unsigned long long)total_false_positives);
    std::printf("  lectura VRAM entera : %.3f ms/frame (%zu bytes)\n",
                ms_full / (n_frames ? n_frames : 1), vram_bytes);
    std::printf("  lectura incremental : %.3f ms/frame (%llu bytes/frame promedio)\n",
                ms_inc / (n_frames ? n_frames : 1),
                (unsigned long long)(incremental_bytes_read / (n_frames ? n_frames : 1)));

    std::printf("  raster_event_count: primero=%u min=%u max=%u ultimo=%u  "
                "(frames en que BAJO: %llu)\n",
                raster_first, raster_min == UINT32_MAX ? 0 : raster_min,
                raster_max, raster_last, (unsigned long long)raster_decrease_frames);

    check(poll_ok, "poll_frame_delta responde OK y con el layout declarado");
    check(counts_coinciden,
          "los counts de sprites/audio del delta == los del snapshot");
    // Las DOS condiciones, y la primera es la que evita el verde vacuo: un
    // journal que nunca se llena "pasa" cualquier check sobre si se vacía.
    check(raster_max > 0,
          "NO-VACUIDAD: hubo frames con eventos raster que observar");
    check(raster_max > 0 && raster_decrease_frames > 0,
          "el raster journal se VACIA entre frames (su count baja alguna vez)");
    check(gen_ok, "la frame_generation del delta == la del snapshot del mismo frame");
    check(total_changed > 0,
          "NO-VACUIDAD: hubo patterns cambiados que comparar");
    check(total_false_negatives == 0,
          "dirty_patterns es SUPERCONJUNTO de lo que cambio (sin falsos negativos)");
    check(espejo_ok,
          "un espejo guiado SOLO por el delta queda byte-identico a la VRAM real");

    r.shutdown();
    std::printf("\n%s — %d checks, %d fallos\n",
                g_fails ? "=== FAIL ===" : "=== OK ===", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
