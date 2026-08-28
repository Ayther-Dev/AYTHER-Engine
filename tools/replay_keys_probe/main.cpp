// ---------------------------------------------------------------------------
// replay_keys_probe (#492) — ¿el PLAYBACK puebla `replay_keys`? Simula la
// reproducción (replay_seek secuencial f, f+1, …) sobre una toma y mide:
//   · replay_key_count() tras N frames (esperado: N / 300 en una toma SIN
//     keyframes horneados; 0 por diseño R7e si los tiene),
//   · el costo de un seek hacia atrás después (ms) — con keys runtime el
//     re-sim arranca del key más cercano, sin ellos del inicio/horneado.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target replay_keys_probe (sin GPU)
//   Args:  <rec> [frames=900]
//   Env:   AYTHER_PROBE_ROM
// ---------------------------------------------------------------------------
#include "ayther_env.h"
#include "ayther_session.h"
#include "ayther_recording.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif

static std::string toml_quoted(const std::string& l) {
    const auto a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string() : l.substr(a + 1, b - a - 1);
}
static std::string resolve(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\') return p;
    return base + "/" + p;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "uso: replay_keys_probe <rec> [frames]\n"); return 2; }
    const std::string rec_path = argv[1];
    const uint32_t N = argc > 2 ? (uint32_t)std::strtoul(argv[2], nullptr, 10) : 900u;

    const std::string root = AYTHER_SOURCE_DIR;
    std::string core, rom, line;
    {
        std::ifstream cfg(root + "/tests/test_config.toml");
        while (std::getline(cfg, line))
            if (line.find("core") != std::string::npos &&
                line.find('=') != std::string::npos && core.empty())
                core = toml_quoted(line);
    }
    core = resolve(core, root);
    if (const char* er = ayther::env_get("AYTHER_PROBE_ROM")) rom = er;
    if (rom.empty()) { std::fprintf(stderr, "[FAIL] falta AYTHER_PROBE_ROM\n"); return 2; }

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;
    auto rec = ayther::AytherRecording::load(rec_path);
    if (!rec) { std::fprintf(stderr, "[FAIL] no se pudo cargar %s\n", rec_path.c_str()); return 1; }
    const uint32_t n = std::min(N, rec->frame_count() - 1);

    std::printf("=== replay_keys_probe (#492) ===  %s  (%u frames, %zu keyframes horneados)\n\n",
                rec_path.c_str(), rec->frame_count(), rec->keyframes.size());

    using clk = std::chrono::steady_clock;
    {   // 0) el PRIMER seek (f0) vs el mismo seek repetido tras invalidar.
        auto ta = clk::now(); s->replay_seek(*rec, 0, true);
        const double a = std::chrono::duration<double, std::milli>(clk::now() - ta).count();
        s->replay_invalidate();
        auto tb = clk::now(); s->replay_seek(*rec, 0, true);
        const double b = std::chrono::duration<double, std::milli>(clk::now() - tb).count();
        s->replay_invalidate();
        auto tc = clk::now(); s->replay_seek(*rec, 1, true);
        const double cc = std::chrono::duration<double, std::milli>(clk::now() - tc).count();
        std::printf("seek f0 primero: %.0f ms · f0 de nuevo (invalidado): %.0f ms · f1 frio: %.0f ms\n", a, b, cc);
        s->replay_invalidate();
    }
    // 1) playback simulado: seeks secuenciales (el camino replay_pos+1 == target).
    auto t0 = clk::now();
    double worst = 0; uint32_t worst_f = 0; double acc = 0; uint32_t slow = 0;
    for (uint32_t f = 0; f <= n; ++f) {
        auto tf = clk::now();
        s->replay_seek(*rec, f, /*quiet=*/false);
        const double ms = std::chrono::duration<double, std::milli>(clk::now() - tf).count();
        if (ms > worst) { worst = ms; worst_f = f; }
        if (ms > 8.0) ++slow;
        acc += ms;
    }
    std::printf("por frame: media %.2f ms · peor %.1f ms en f%u · %u frames > 8 ms\n",
                acc / (n + 1), worst, worst_f, slow);
    const double play_ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    const size_t keys = s->replay_key_count();
    std::printf("playback 0..%u: %.0f ms · replay_keys = %zu (esperado %s)\n", n, play_ms, keys,
                rec->keyframes.empty() ? "n/300" : "0 por diseño R7e: la toma tiene horneados");

    // 2) seek hacia atrás a n-50: ¿arranca del key runtime o re-simula todo?
    t0 = clk::now();
    s->replay_seek(*rec, n >= 50 ? n - 50 : 0, true);
    const double back_ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    std::printf("seek atrás a %u: %.1f ms\n", n >= 50 ? n - 50 : 0, back_ms);

    // 3) seek frío a n/2 (otra vez, tras invalidar): costo con/sin keys.
    s->replay_invalidate();
    t0 = clk::now();
    s->replay_seek(*rec, n / 2, true);
    const double mid_ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    std::printf("seek a %u tras invalidar: %.1f ms\n", n / 2, mid_ms);

    const bool ok = rec->keyframes.empty() ? keys == (size_t)(n / 300) : keys == 0;
    std::printf("\n[%s] replay_keys %s\n", ok ? " OK " : "FAIL",
                ok ? "se comporta como dice el diseño" : "NO coincide con el diseño");
    return ok ? 0 : 1;
}
