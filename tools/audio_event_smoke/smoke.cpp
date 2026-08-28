// ---------------------------------------------------------------------------
// audio_event_smoke — valida el AudioEventDetector (C-A2) sobre datos REALES.
//
// audio_chip_spike probó que el LOG de escrituras es replay-estable. Esto cierra
// el siguiente eslabón: que el DETECTOR, alimentado con ese log frame a frame,
// produzca eventos por canal (key-on→key-off) con firmas estables, y que el
// CONJUNTO de eventos sea idéntico a través del replay (no sólo el log crudo).
//
//   1. cargar core + ROM, warm-up hasta un estado con audio
//   2. savestate S
//   3. Run A: N frames → por frame, reset+run+leer el log, alimentar el detector;
//      finish → snapshot del set de eventos A
//   4. unserialize(S) + reset detector
//   5. Run B idéntico → set de eventos B
//   6. A == B  →  los eventos son replay-consistentes ✓
//   + telemetría: total, FM/PSG, firmas únicas (agrupación), top recurrentes.
//
//   audio_event_smoke <core.dll> <rom> [--warmup N] [--frames N]
// ---------------------------------------------------------------------------

#include "libretro_host/retro_runner.h"
#include "ayther_core_ffi.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static_assert(sizeof(RetroRunner::AudioWrite) == sizeof(AytherAudioWrite),
              "RetroRunner::AudioWrite y AytherAudioWrite deben tener el mismo ABI");

static constexpr uint16_t BTN_START = 1u << 3;
static constexpr uint16_t BTN_RIGHT = 1u << 7;
static constexpr uint16_t BTN_A     = 1u << 8;

static uint16_t scripted_input(int frame) {
    uint16_t b = BTN_RIGHT;
    if (frame % 24 < 4) b |= BTN_A;
    return b;
}

// Corre `frames` frames desde el estado actual, alimentando el detector, y
// devuelve el set de eventos detectados (tras finish).
static std::vector<AytherAudioEvent> run_and_detect(RetroRunner& runner, int frames) {
    AytherAudioEventDetector* det = ayther_audio_event_new();
    for (int i = 0; i < frames; ++i) {
        runner.set_input(0, scripted_input(i));
        runner.run_frame();
        const auto* aw = runner.audio_writes();
        const uint32_t n = runner.audio_write_count();
        ayther_audio_event_process_frame(
            det, static_cast<uint32_t>(i),
            reinterpret_cast<const AytherAudioWrite*>(aw), n);
    }
    ayther_audio_event_finish(det);

    const uint32_t k = ayther_audio_event_count(det);
    std::vector<AytherAudioEvent> out(k);
    if (k) ayther_audio_event_get(det, out.data(), k);
    ayther_audio_event_free(det);
    return out;
}

// Igualdad del set de eventos (mismo orden esperado: el detector los emite en
// orden de cierre, determinista si el stream de comandos lo es).
static bool same_events(const std::vector<AytherAudioEvent>& a,
                        const std::vector<AytherAudioEvent>& b, int& first_diff) {
    first_diff = -1;
    if (a.size() != b.size()) { first_diff = static_cast<int>((std::min)(a.size(), b.size())); return false; }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].signature != b[i].signature || a[i].chip != b[i].chip ||
            a[i].channel != b[i].channel || a[i].start_frame != b[i].start_frame ||
            a[i].end_frame != b[i].end_frame) { first_diff = static_cast<int>(i); return false; }
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "audio_event_smoke — valida el AudioEventDetector (C-A2) sobre datos reales\n"
            "usage: %s <core.dll> <rom> [--warmup N] [--frames N]\n", argv[0]);
        return 2;
    }
    const std::string core = argv[1];
    const std::string rom  = argv[2];
    int warmup = 400, frames = 600;
    for (int i = 3; i + 1 < argc; i += 2) {
        if      (std::strcmp(argv[i], "--warmup") == 0) warmup = std::atoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "--frames") == 0) frames = std::atoi(argv[i + 1]);
    }

    RetroRunner runner;
    if (!runner.init(core, rom)) { std::fprintf(stderr, "[smoke] init falló\n"); return 1; }
    // El fork no instrumenta hasta que se lo piden: sin esto el log de
    // escrituras viene VACÍO y el detector no ve un solo evento.
    runner.subscribe_all_supported();

    runner.run_frame();
    if (runner.audio_writes() == nullptr) {
        std::fprintf(stderr, "[smoke] el core no expone el log de audio (id 0x109) — DLL sin instrumentar.\n");
        return 3;
    }

    for (int i = 0; i < warmup; ++i) {
        runner.set_input(0, (i % 64 < 3) ? BTN_START : 0);
        runner.run_frame();
    }

    std::vector<uint8_t> S;
    if (!runner.serialize(S)) { std::fprintf(stderr, "[smoke] sin savestate\n"); return 3; }
    std::printf("[smoke] core=%s\n[smoke] warmup=%d frames=%d savestate=%zu B\n",
                runner.fps() > 0 ? "ok" : "?", warmup, frames, S.size());

    // ---- Run A + Run B (cold replay) ---------------------------------------
    std::vector<AytherAudioEvent> A = run_and_detect(runner, frames);
    runner.unserialize(S);
    std::vector<AytherAudioEvent> B = run_and_detect(runner, frames);

    // ---- Telemetría sobre A -------------------------------------------------
    // El DAC va POR SEPARADO aunque viaje como FM6: es el único que se segmenta
    // por silencio en vez de por key-on, y sumado al FM su ausencia no se ve —
    // que es exactamente lo que pasó cuando el registro 0x2A dejó de llegar.
    uint32_t fm = 0, psg = 0, dac = 0; std::map<uint64_t, uint32_t> by_sig;
    for (const auto& e : A) {
        if (e.chip != 0)                              ++psg;
        else if (e.channel == 5 && e.pitch == 255)    ++dac;
        else                                          ++fm;
        ++by_sig[e.signature];
    }
    std::printf("\n--- eventos detectados (Run A) ---\n"
                "total=%zu  (FM=%u  PSG=%u  DAC=%u)   firmas únicas=%zu\n",
                A.size(), fm, psg, dac, by_sig.size());

    // Top firmas recurrentes (= mismo SFX que reaparece → candidato a evento).
    std::vector<std::pair<uint64_t,uint32_t>> top(by_sig.begin(), by_sig.end());
    std::sort(top.begin(), top.end(), [](auto& a, auto& b){ return a.second > b.second; });
    std::printf("top firmas recurrentes (apariciones):\n");
    for (size_t i = 0; i < top.size() && i < 5; ++i)
        std::printf("  %016llx  ×%u\n", (unsigned long long)top[i].first, top[i].second);

    // Primeros eventos como muestra.
    std::printf("muestra (primeros 6):\n");
    for (size_t i = 0; i < A.size() && i < 6; ++i)
        std::printf("  [%s ch%u]  frames %u..%u  sig=%016llx\n",
                    A[i].chip == 0 ? "FM " : "PSG", A[i].channel,
                    A[i].start_frame, A[i].end_frame, (unsigned long long)A[i].signature);

    // ---- Determinismo del set de eventos a través del replay ----------------
    int first_diff = -1;
    const bool det_ok = same_events(A, B, first_diff);
    const bool live   = !A.empty();

    std::printf("\n");
    if (det_ok && live) {
        std::printf("============================================================\n"
                    " \xE2\x9C\x93 EVENTOS REPLAY-CONSISTENTES — %zu eventos idénticos A==B.\n"
                    "   El AudioEventDetector (C-A2) es robusto sobre datos reales.\n"
                    "============================================================\n", A.size());
        return 0;
    }
    if (!live) {
        std::printf("[FAIL] no se detectó ningún evento (subí --warmup o --frames).\n");
        return 6;
    }
    std::printf("============================================================\n"
                " \xE2\x9C\x97 LOS EVENTOS DIVERGEN entre A y B (índice %d).\n"
                "   A=%zu eventos, B=%zu eventos.\n"
                "============================================================\n",
                first_diff, A.size(), B.size());
    return 5;
}
