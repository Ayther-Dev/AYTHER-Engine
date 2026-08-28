// ---------------------------------------------------------------------------
// audio_chip_spike — valida el riesgo #1 del workspace Audios (hito C-A1).
//
// PREMISA (la apuesta del enfoque por canal): la identidad de un sonido debe
// derivarse de la SECUENCIA DE COMANDOS a los chips (YM2612 FM + SN76489 PSG),
// NO del PCM de salida. El PCM no es reproducible a través del replay (la fase
// del FM diverge tras unserialize — ver memoria audio-not-reproducible-on-replay),
// pero las escrituras de registro son comandos que dispara la CPU, y la CPU/VDP
// SON byte-deterministas (lo prueba determinism_spike con el framebuffer).
//
// Este harness lo confirma o lo refuta, headless, leyendo el log de escrituras
// que el fork expone por los ids 0x109/0x10A:
//
//   1. cargar core + ROM, warm-up hasta un estado con audio sonando
//   2. savestate S
//   3. Run A: N frames con input scripted → por frame, reset+run+leer el log,
//      hashear la secuencia de comandos (chip,addr,data) y el registro completo
//      (incl. cycle)                                            → A[]
//   4. unserialize(S)
//   5. Run B: N frames idénticos                                → B[]
//   6. A == B frame-a-frame  →  las escrituras son REPLAY-ESTABLES ✓
//
// Si el stream de comandos coincide (aunque el cycle difiera), el enfoque por
// firma de comandos es viable. Si ni los comandos coinciden, hay que replantear.
//
//   audio_chip_spike <core.dll> <rom> [--warmup N] [--frames N]
// ---------------------------------------------------------------------------

#include "libretro_host/retro_runner.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static_assert(sizeof(RetroRunner::AudioWrite) == 8,
              "AudioWrite debe ser 8 bytes (ABI idéntico al AytherAudioWrite del fork)");

// FNV-1a 64-bit, encadenable.
static uint64_t fnv1a(const uint8_t* p, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
static constexpr uint64_t kFnvSeed = 1469598103934665603ull;

// Libretro joypad button ids → bit positions.
static constexpr uint16_t BTN_START = 1u << 3;
static constexpr uint16_t BTN_RIGHT = 1u << 7;
static constexpr uint16_t BTN_A     = 1u << 8;

// Input scripted y determinista: correr a la derecha, saltar periódicamente.
// Lo que haga en el juego no importa — sólo que AMBAS corridas usen la MISMA
// secuencia, así un core determinista produce el mismo stream de comandos.
static uint16_t scripted_input(int frame) {
    uint16_t b = BTN_RIGHT;
    if (frame % 24 < 4) b |= BTN_A;   // salto cada ~0.4 s
    return b;
}

// Resultado de medir un frame: hashes + telemetría.
struct FrameAudio {
    uint64_t cmd_hash  = kFnvSeed;  // hash de (chip,addr,data) — la firma de comandos
    uint64_t full_hash = kFnvSeed;  // hash del registro completo (incl. cycle)
    uint32_t n         = 0;         // escrituras este frame
    uint32_t fm        = 0;         // de las cuales FM
    uint32_t psg       = 0;         // de las cuales PSG
    uint32_t fm_keyon  = 0;         // escrituras al registro 0x28 del FM (key on/off)
    bool     saturated = false;     // tocó el tope del buffer
};

// Mide el log de audio del frame que se acaba de correr.
// E-5 (#401): este spike es del camino LEGACY a propósito — mide el log crudo
// del fork tal como lo veía el engine antes de la ABI. La deprecación protege a
// los callers nuevos, no a este.
#define AYTHER_LEGACY_READ_BEGIN     _Pragma("clang diagnostic push")     _Pragma("clang diagnostic ignored \"-Wdeprecated-declarations\"")
#define AYTHER_LEGACY_READ_END _Pragma("clang diagnostic pop")

AYTHER_LEGACY_READ_BEGIN
static FrameAudio measure(const RetroRunner& runner) {
    FrameAudio fa;
    const uint32_t cnt = runner.audio_write_count();
    const RetroRunner::AudioWrite* w = runner.audio_writes();
    if (!w || cnt == 0) return fa;
    fa.n = cnt;
    fa.saturated = (cnt >= 8192);   // == AYTHER_AUDIO_WRITE_CAP del fork

    // `addr` es el REGISTRO ya latcheado (0x000-0x1FF) desde el fork 3fc6ee89
    // (2026-08-11) — ver ayther_core_ffi.h. Contar key-ons reconstruyendo el
    // protocolo de puertos daba SIEMPRE 0 y el spike lo mostraba junto a miles
    // de escrituras sin que la contradicción saltara.
    for (uint32_t i = 0; i < cnt; ++i) {
        const RetroRunner::AudioWrite& e = w[i];
        // Firma de comandos: chip, addr, data (sin el timing).
        const uint8_t cmd[4] = { e.chip, static_cast<uint8_t>(e.addr & 0xFF),
                                 static_cast<uint8_t>(e.addr >> 8), e.data };
        fa.cmd_hash = fnv1a(cmd, sizeof(cmd), fa.cmd_hash);
        // Registro completo: + cycle.
        fa.full_hash = fnv1a(reinterpret_cast<const uint8_t*>(&e), sizeof(e), fa.full_hash);

        if (e.chip == RetroRunner::kAudioChipFM) {
            ++fa.fm;
            if (e.addr == 0x28) ++fa.fm_keyon;   // key on/off de un canal
        } else {
            ++fa.psg;
        }
    }
    return fa;
}
AYTHER_LEGACY_READ_END

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "audio_chip_spike — valida que las escrituras a los chips de sonido\n"
            "son replay-estables (base del workspace Audios, hito C-A1)\n"
            "usage: %s <core.dll> <rom> [--warmup N] [--frames N]\n",
            argv[0]);
        return 2;
    }
    const std::string core = argv[1];
    const std::string rom  = argv[2];
    int warmup = 400;   // frames para alcanzar un estado con música/SFX
    int frames = 300;   // frames a comparar (~5 s)
    for (int i = 3; i + 1 < argc; i += 2) {
        if      (std::strcmp(argv[i], "--warmup") == 0) warmup = std::atoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "--frames") == 0) frames = std::atoi(argv[i + 1]);
    }

    RetroRunner runner;
    if (!runner.init(core, rom)) {
        std::fprintf(stderr, "[spike] init falló (¿path de core/rom?)\n");
        return 1;
    }
    std::fprintf(stdout, "[spike] core fps=%.4f  pixfmt=%u\n",
                 runner.fps(), runner.pixel_format());
    // Sin esto el fork no instrumenta nada y el log viene VACÍO — este spike
    // reportaba «no se capturó NINGUNA escritura» y sugería subir el warmup.
    std::fprintf(stdout, "[spike] suscripciones: 0x%08X\n",
                 runner.subscribe_all_supported());

    // ¿El core expone el log? (id 0x10A). Si no, es un core stock.
    // E-5 (#401): ya no hay reset manual — el core cierra el log en SU frame
    // boundary, que es lo que este spike necesita (escrituras POR frame).
    runner.run_frame();
    AYTHER_LEGACY_READ_BEGIN
    const bool missing_log = (runner.audio_writes() == nullptr);
    AYTHER_LEGACY_READ_END
    if (missing_log) {
        std::fprintf(stderr,
            "[spike] el core NO expone el log de escrituras de audio (id 0x109).\n"
            "        ¿DLL sin instrumentar? Recompilá el fork con la instrumentación C-A1.\n");
        return 3;
    }

    // ---- Warm-up: tocar START para entrar a un nivel (audio sonando) -------
    for (int i = 0; i < warmup; ++i) {
        const bool tap = (i % 64 < 3);   // pulsos de START repetidos
        runner.set_input(0, tap ? BTN_START : 0);
        runner.run_frame();
    }

    // ---- Savestate ---------------------------------------------------------
    std::vector<uint8_t> S;
    if (!runner.serialize(S)) {
        std::fprintf(stderr, "[spike] el core no soporta savestate.\n");
        return 3;
    }
    std::fprintf(stdout, "[spike] savestate: %zu bytes\n", S.size());

    // ---- Run A -------------------------------------------------------------
    std::vector<FrameAudio> A(frames);
    uint64_t totalA = 0, fmA = 0, psgA = 0, keyonA = 0; bool satA = false;
    for (int i = 0; i < frames; ++i) {
        runner.set_input(0, scripted_input(i));
        runner.run_frame();
        A[i] = measure(runner);
        totalA += A[i].n; fmA += A[i].fm; psgA += A[i].psg; keyonA += A[i].fm_keyon;
        satA |= A[i].saturated;
    }

    // ---- Restore + Run B ---------------------------------------------------
    if (!runner.unserialize(S)) {
        std::fprintf(stderr, "[spike] unserialize falló.\n");
        return 4;
    }
    std::vector<FrameAudio> B(frames);
    uint64_t totalB = 0;
    for (int i = 0; i < frames; ++i) {
        runner.set_input(0, scripted_input(i));
        runner.run_frame();
        B[i] = measure(runner);
        totalB += B[i].n;
    }

    // ---- Comparar ----------------------------------------------------------
    int first_cmd_diff  = -1;   // divergencia en la firma de comandos (lo que importa)
    int first_full_diff = -1;   // divergencia incluyendo el cycle (más estricto)
    int first_n_diff    = -1;   // divergencia en el conteo de escrituras
    for (int i = 0; i < frames; ++i) {
        if (first_n_diff   < 0 && A[i].n         != B[i].n)         first_n_diff   = i;
        if (first_cmd_diff < 0 && A[i].cmd_hash  != B[i].cmd_hash)  first_cmd_diff = i;
        if (first_full_diff< 0 && A[i].full_hash != B[i].full_hash) first_full_diff= i;
    }

    std::fprintf(stdout,
        "\n--- telemetría (Run A) ---\n"
        "escrituras totales: %llu  (FM=%llu  PSG=%llu)   key-on FM (reg 0x28): %llu\n"
        "promedio/frame    : %.1f escrituras   saturó el buffer: %s\n"
        "Run B totales     : %llu\n",
        (unsigned long long)totalA, (unsigned long long)fmA, (unsigned long long)psgA,
        (unsigned long long)keyonA, frames ? (double)totalA / frames : 0.0,
        satA ? "SÍ (subir AYTHER_AUDIO_WRITE_CAP)" : "no",
        (unsigned long long)totalB);

    if (totalA == 0) {
        std::fprintf(stdout,
            "\n[FAIL] no se capturó NINGUNA escritura — el test es vacío.\n"
            "       Subí --warmup o verificá que el juego esté reproduciendo audio.\n");
        return 6;
    }

    std::fprintf(stdout, "\n");
    const bool cmd_det  = (first_cmd_diff  < 0);
    const bool full_det = (first_full_diff < 0);

    if (cmd_det) {
        std::fprintf(stdout,
            "============================================================\n"
            " %s FIRMA DE COMANDOS REPLAY-ESTABLE — %d frames idénticos.\n"
            "   La identidad de audio por secuencia de comandos al chip\n"
            "   es VIABLE (riesgo #1 / C-A1 SUPERADO).\n"
            "   timing (cycle) %s.\n"
            "============================================================\n",
            "\xE2\x9C\x93", frames,
            full_det ? "TAMBIÉN bit-exacto (incl. cycle)"
                     : "difiere (no fatal: la firma usa chip/addr/data, no el cycle)");
        return 0;
    }

    std::fprintf(stdout,
        "============================================================\n"
        " %s LAS ESCRITURAS DIVERGEN — firma de comandos en el frame %d.\n"
        "   (conteo difiere en frame %d · registro completo en %d)\n"
        "   A.cmd=%016llx  B.cmd=%016llx\n"
        "   → El enfoque por comandos necesita revisión antes de C-A2.\n"
        "============================================================\n",
        "\xE2\x9C\x97", first_cmd_diff, first_n_diff, first_full_diff,
        (unsigned long long)A[first_cmd_diff].cmd_hash,
        (unsigned long long)B[first_cmd_diff].cmd_hash);
    return 5;
}
