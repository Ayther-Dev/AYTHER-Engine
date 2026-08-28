// ---------------------------------------------------------------------------
// audio_mute_smoke — valida el MUTE SELECTIVO POR CANAL del fork (C-A3).
//
// El fork expone una máscara de mute por canal (id 0x10D): bits 0-5 = FM 0-5,
// bits 6-9 = PSG 0-3. El canal se pone a 0 en el mixer de SALIDA sin tocar el
// estado del chip → replay-safe. Este harness lo confirma midiendo la energía
// del PCM emitido bajo distintas máscaras, desde el MISMO savestate:
//
//   * mute=0          → energía base (todo suena)         > 0
//   * mute=FM (0x03F) → baja (FM domina la música)        < base
//   * mute=ALL (0x3FF)→ ~silencio                          ≈ 0
//   * re-correr una máscara → PCM bit-idéntico (hash)      = determinismo
//
// El mute a nivel de salida no cambia el estado del chip, así que el PCM bajo una
// misma máscara debe ser reproducible byte a byte (clave para la sustitución).
//
//   audio_mute_smoke <core.dll> <rom> [--warmup N] [--frames N]
// ---------------------------------------------------------------------------

#include "libretro_host/retro_runner.h"
#include "synth_rom.h"   // #415: ROM sintetica, para entrar al ctest

#include <filesystem>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static constexpr uint16_t BTN_START = 1u << 3;
static constexpr uint16_t BTN_RIGHT = 1u << 7;
static constexpr uint16_t BTN_A     = 1u << 8;

static uint16_t scripted_input(int frame) {
    uint16_t b = BTN_RIGHT;
    if (frame % 24 < 4) b |= BTN_A;
    return b;
}

// Acumulador de salida de audio (sumado por el callback de batches).
static uint64_t g_energy = 0;             // Σ|sample| (estéreo)
static uint64_t g_hash   = 1469598103934665603ull;  // FNV-1a del PCM
static uint64_t g_samples = 0;

static void reset_accum() {
    g_energy = 0; g_samples = 0; g_hash = 1469598103934665603ull;
}

struct Measure { uint64_t energy; uint64_t hash; uint64_t samples; };

static Measure measure(RetroRunner& runner, const std::vector<uint8_t>& S,
                       uint16_t mask, int frames) {
    runner.unserialize(S);
    runner.set_audio_mute_v1(mask);
    reset_accum();
    for (int i = 0; i < frames; ++i) {
        runner.set_input(0, scripted_input(i));
        runner.run_frame();
    }
    runner.set_audio_mute_v1(0);   // limpiar para la próxima medición
    return { g_energy, g_hash, g_samples };
}

int main(int argc, char** argv) {
    // #415: sin argumentos usa el core del fork y la ROM SINTÉTICA — el mismo
    // camino que audio_output_smoke. Eso es lo que lo mete al ctest de todos
    // los días: hasta ahora esto medía el mute por canal del fork y se corría a
    // mano, cuando alguien se acordaba.
    //
    // Con argumentos sigue aceptando una ROM comercial, que es como se usa para
    // mirar un juego concreto.
    std::string core, rom;
    if (argc >= 3) {
        core = argv[1];
        rom  = argv[2];
    } else {
        core = std::string(AYTHER_SOURCE_DIR)
             + "/third_party/cores/genesis_plus_gx_libretro_vram.dll";
        if (!std::filesystem::exists(core)) {
            std::printf("[skip] no está el core del fork: %s\n", core.c_str());
            return 77;   // SALTEADO, no aprobado: ver #415
        }
        rom = ayther::synth::canonical_rom_path();
        if (rom.empty()) {
            std::fprintf(stderr, "[FAIL] no pude escribir la ROM sintética\n");
            return 1;
        }
    }
    // La ROM sintética arranca a sonar enseguida: el warm-up de una comercial
    // (450 frames de menús) mediría el final de su bucle en vez del principio.
    int warmup = (argc >= 3) ? 450 : 20;
    int frames = 200;
    for (int i = 3; i + 1 < argc; i += 2) {
        if      (std::strcmp(argv[i], "--warmup") == 0) warmup = std::atoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "--frames") == 0) frames = std::atoi(argv[i + 1]);
    }

    RetroRunner runner;
    runner.set_audio_callback([](const int16_t* data, size_t frames) -> size_t {
        const size_t n = frames * 2;  // estéreo
        const auto* b = reinterpret_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n * sizeof(int16_t); ++i) { g_hash ^= b[i]; g_hash *= 1099511628211ull; }
        for (size_t i = 0; i < n; ++i) { int s = data[i]; g_energy += (s < 0 ? -s : s); }
        g_samples += n;
        return frames;
    });

    if (!runner.init(core, rom)) { std::fprintf(stderr, "[smoke] init falló\n"); return 1; }
    // Sin AYTHER_SUB_RENDER_CONTROLS el core IGNORA la máscara de mute EN
    // SILENCIO, y este smoke termina comparando dos corridas idénticas y
    // llamándolas «con mute» y «sin mute».
    runner.subscribe_all_supported();

    // ¿El core soporta el mute? (id 0x10D). Con core stock, set es no-op.
    runner.run_frame();
    // (no hay lectura directa; lo inferimos de que ALL baje la energía)

    for (int i = 0; i < warmup; ++i) {
        runner.set_input(0, (i % 64 < 3) ? BTN_START : 0);
        runner.run_frame();
    }
    std::vector<uint8_t> S;
    if (!runner.serialize(S)) { std::fprintf(stderr, "[smoke] sin savestate\n"); return 3; }

    // Máscaras: FM = bits 0-5 (0x03F); PSG = bits 6-9 (0x3C0); ALL = 0x3FF.
    const Measure none  = measure(runner, S, 0x000, frames);
    const Measure fm    = measure(runner, S, 0x03F, frames);
    const Measure psg   = measure(runner, S, 0x3C0, frames);
    const Measure all   = measure(runner, S, 0x3FF, frames);
    const Measure none2 = measure(runner, S, 0x000, frames);  // determinismo (sin mute)
    const Measure fm2   = measure(runner, S, 0x03F, frames);  // determinismo (con mute)
    const Measure ch0   = measure(runner, S, 0x001, frames);  // sólo FM canal 0

    auto pct = [&](const Measure& m){ return none.energy ? 100.0 * m.energy / none.energy : 0.0; };
    std::printf("\n--- energía del PCM por máscara (%llu samples/medición) ---\n",
                (unsigned long long)none.samples);
    std::printf("  mute=0x000 (nada)     energía=%-14llu 100.0%%  (base)\n", (unsigned long long)none.energy);
    std::printf("  mute=0x001 (FM ch0)   energía=%-14llu %5.1f%%\n", (unsigned long long)ch0.energy, pct(ch0));
    std::printf("  mute=0x03F (FM all)   energía=%-14llu %5.1f%%\n", (unsigned long long)fm.energy,  pct(fm));
    std::printf("  mute=0x3C0 (PSG all)  energía=%-14llu %5.1f%%\n", (unsigned long long)psg.energy, pct(psg));
    std::printf("  mute=0x3FF (todo)     energía=%-14llu %5.1f%%\n", (unsigned long long)all.energy, pct(all));

    // -- Asertos --------------------------------------------------------------
    const bool has_audio   = none.energy > 0;
    const bool fm_drops     = fm.energy < none.energy;          // mutear FM baja la energía
    const bool ch0_partial  = ch0.energy < none.energy;         // mutear 1 canal baja algo
    const bool all_silences = all.energy * 1000 < none.energy;  // mute total → <0.1% (≈silencio)
    const bool det_none     = none2.hash == none.hash;          // mismo PCM sin mute (determinista)
    const bool det_fm       = fm2.hash   == fm.hash;            // mismo PCM con mute (replay-safe)
    const bool mute_works    = all.energy < none.energy;        // el id 0x10D existe y actúa

    std::printf("\nchecks: audio=%d fm<base=%d ch0<base=%d all≈silencio=%d det(none)=%d det(fm)=%d\n",
                has_audio, fm_drops, ch0_partial, all_silences, det_none, det_fm);

    if (!mute_works) {
        std::printf("\n[FAIL] el mute no tuvo efecto — ¿core sin instrumentar (id 0x10D)?\n");
        return 3;
    }

    const bool ok = has_audio && fm_drops && all_silences && det_none && det_fm;
    std::printf("\n============================================================\n");
    if (ok)
        std::printf(" \xE2\x9C\x93 MUTE SELECTIVO OK — baja por canal, mute total ≈ silencio,\n"
                    "   PCM replay-determinista con y sin mute (C-A3 primitivo listo).\n");
    else
        std::printf(" \xE2\x9C\x97 FALLO en algún check (ver arriba).\n");
    std::printf("============================================================\n");
    return ok ? 0 : 5;
}
