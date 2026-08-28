// ---------------------------------------------------------------------------
// determinism_spike — validates risk #1 of the v0.10 plan.
//
// The whole .arp / timeline / QA model assumes Genesis Plus GX re-simulates
// IDENTICALLY from a savestate + the same input. This headless harness proves
// (or disproves) it:
//
//   1. load core + ROM, warm up (tap START to try to reach gameplay)
//   2. savestate S
//   3. run N frames with a scripted input sequence  → hash each framebuffer (H1)
//   4. load_state(S)
//   5. run N frames with the SAME sequence          → hash each framebuffer (H2)
//   6. H1 == H2 frame-by-frame  →  DETERMINISTIC ✓
//
// No SDL / Vulkan: the video callback just hashes the raw framebuffer.
// BYOR: you provide the ROM.
//
//   determinism_spike <core.dll> <rom> [--warmup N] [--frames N]
// ---------------------------------------------------------------------------

#include "libretro_host/retro_runner.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// FNV-1a 64-bit over a byte range (chainable across rows).
static uint64_t fnv1a(const uint8_t* p, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
static constexpr uint64_t kFnvSeed = 1469598103934665603ull;

// Libretro joypad button ids → bit positions.
static constexpr uint16_t BTN_START = 1u << 3;
static constexpr uint16_t BTN_UP    = 1u << 4;
static constexpr uint16_t BTN_RIGHT = 1u << 7;
static constexpr uint16_t BTN_A     = 1u << 8;

// Scripted, deterministic input: run right, jump periodically.
// What it does in-game doesn't matter — only that BOTH runs use the SAME
// sequence, so a deterministic core must produce identical frames.
static uint16_t scripted_input(int frame) {
    uint16_t b = BTN_RIGHT;
    if (frame % 24 < 4) b |= BTN_A;   // hop every ~0.4 s
    return b;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "determinism_spike — validate savestate re-simulation determinism\n"
            "usage: %s <core.dll> <rom> [--warmup N] [--frames N]\n",
            argv[0]);
        return 2;
    }
    const std::string core = argv[1];
    const std::string rom  = argv[2];
    int warmup = 250;   // frames to reach a dynamic state
    int frames = 300;   // frames to compare (~5 s)
    for (int i = 3; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--warmup") == 0) warmup = std::atoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "--frames") == 0) frames = std::atoi(argv[i + 1]);
    }

    RetroRunner runner;

    // The video callback hashes each frame. data==nullptr is a "dupe frame"
    // (repeat previous) → keep the last hash.
    uint64_t frame_hash = 0;
    runner.set_video_callback(
        [&](const void* data, unsigned w, unsigned h, size_t pitch) {
            if (!data || w == 0 || h == 0) return;
            const auto* px = static_cast<const uint8_t*>(data);
            const size_t bpp = (runner.pixel_format() == 1) ? 4 : 2;  // XRGB8888 vs 16-bit
            uint64_t hh = kFnvSeed;
            for (unsigned y = 0; y < h; ++y)
                hh = fnv1a(px + y * pitch, static_cast<size_t>(w) * bpp, hh);
            frame_hash = hh;
        });

    if (!runner.init(core, rom)) {
        std::fprintf(stderr, "[spike] init failed (core/rom path?)\n");
        return 1;
    }
    runner.subscribe_all_supported();   // sin esto el fork no observa nada
    std::fprintf(stdout, "[spike] core fps=%.4f  pixfmt=%u\n",
                 runner.fps(), runner.pixel_format());

    // ---- Warm-up: tap START a few times to try to enter gameplay ----------
    for (int i = 0; i < warmup; ++i) {
        const bool tap = (i == 60 || i == 120 || i == 180);
        runner.set_input(0, tap ? BTN_START : 0);
        runner.run_frame();
    }

    // ---- Savestate ---------------------------------------------------------
    std::vector<uint8_t> S;
    if (!runner.serialize(S)) {
        std::fprintf(stderr,
            "[spike] core has NO savestate support (retro_serialize missing/failed).\n"
            "        The .arp model cannot rely on this core.\n");
        return 3;
    }
    std::fprintf(stdout, "[spike] savestate size: %zu bytes\n", S.size());

    // ---- Run 1 -------------------------------------------------------------
    std::vector<uint64_t> H1(frames);
    for (int i = 0; i < frames; ++i) {
        runner.set_input(0, scripted_input(i));
        runner.run_frame();
        H1[i] = frame_hash;
    }

    // ---- Restore + Run 2 ---------------------------------------------------
    if (!runner.unserialize(S)) {
        std::fprintf(stderr, "[spike] unserialize failed.\n");
        return 4;
    }
    std::vector<uint64_t> H2(frames);
    for (int i = 0; i < frames; ++i) {
        runner.set_input(0, scripted_input(i));
        runner.run_frame();
        H2[i] = frame_hash;
    }

    // ---- Compare -----------------------------------------------------------
    int first_diff = -1;
    for (int i = 0; i < frames; ++i)
        if (H1[i] != H2[i]) { first_diff = i; break; }

    std::fprintf(stdout, "\n");
    if (first_diff < 0) {
        std::fprintf(stdout,
            "============================================================\n"
            " ✓ DETERMINISTA — %d frames idénticos tras load_state.\n"
            "   El modelo .arp / timeline / QA es VIABLE (riesgo #1 OK).\n"
            "============================================================\n",
            frames);
        // Bonus: savestate round-trip stability (not fatal if it differs).
        std::vector<uint8_t> S2;
        if (runner.unserialize(S) && runner.serialize(S2))
            std::fprintf(stdout, " round-trip serialize: %s\n",
                         (S2 == S) ? "estable (byte-exact)" : "difiere (equivalente, no fatal)");
        return 0;
    }

    std::fprintf(stdout,
        "============================================================\n"
        " ✗ NO DETERMINISTA — divergen en el frame %d.\n"
        "   H1=%016llx  H2=%016llx\n"
        "   → Mitigación: keyframes más frecuentes en la occurrence\n"
        "     history, o redefinir §7 del plan antes de invertir en R7.\n"
        "============================================================\n",
        first_diff,
        static_cast<unsigned long long>(H1[first_diff]),
        static_cast<unsigned long long>(H2[first_diff]));
    return 5;
}
