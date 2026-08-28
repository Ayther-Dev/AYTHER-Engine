// ---------------------------------------------------------------------------
// hd_audio_probe — sonda del issue #146 (degradación de sonido con Assets HD):
// reproduce un rango de una grabación DOS veces — sin y con pose-overrides
// (compose activo) — y compara:
//   · el ESTADO serializado del core al final (byte a byte): si difiere, el
//     produce con compose FILTRÓ estado al chain de replay (post-B / roundtrip
//     FM) → la continuidad de audio no puede ser la misma.
//   · el tiempo por frame (el compose re-emula el frame → 2× costo).
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target hd_audio_probe
//   Args:  <poses.toml> <recording.arp> <f0> <n_frames>
//   Env:   AYTHER_PROBE_ROM (el core sale de tests/test_config.toml)
// ---------------------------------------------------------------------------
#include "ayther_env.h"
#include "ayther_session.h"
#include "ayther_recording.h"

#include <SDL3/SDL.h>   // SDL_InitSubSystem(AUDIO) — el device lo abre AudioPlayer

#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

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

// Parser mínimo de poses.toml (subset del de pose_replay_scan): hashes/rel/
// dims/asset — suficiente para alimentar set_pose_preview como el Lab.
struct PoseDef {
    std::string asset;
    std::vector<uint64_t> hashes;
    std::vector<std::pair<int16_t, int16_t>> rel, dims;
};
static std::vector<PoseDef> load_poses(const std::string& path) {
    std::vector<PoseDef> out;
    std::ifstream f(path);
    std::string line;
    PoseDef cur;
    auto flush = [&]() { if (!cur.hashes.empty()) out.push_back(cur); cur = PoseDef{}; };
    while (std::getline(f, line)) {
        if (line.find("[[pose]]") != std::string::npos) { flush(); continue; }
        auto starts = [&](const char* k) { return line.rfind(k, 0) == 0; };
        if (starts("default_asset")) cur.asset = toml_quoted(line);
        else if (starts("hashes")) {
            const std::string v = toml_quoted(line);
            for (size_t p = 0; p < v.size(); ) {
                size_t e = v.find('|', p);
                if (e == std::string::npos) e = v.size();
                cur.hashes.push_back(std::strtoull(v.substr(p, e - p).c_str(), nullptr, 16));
                p = e + 1;
            }
        } else if (starts("rel") || starts("dims")) {
            auto& dst = starts("rel") ? cur.rel : cur.dims;
            const std::string v = toml_quoted(line);
            for (size_t p = 0; p < v.size(); ) {
                size_t e = v.find('|', p);
                if (e == std::string::npos) e = v.size();
                const std::string it = v.substr(p, e - p);
                const size_t c = it.find(',');
                if (c != std::string::npos)
                    dst.push_back({ (int16_t)std::atoi(it.substr(0, c).c_str()),
                                    (int16_t)std::atoi(it.substr(c + 1).c_str()) });
                p = e + 1;
            }
        }
    }
    flush();
    return out;
}

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "uso: hd_audio_probe <poses.toml> <rec.arp> <f0> <n>\n");
        return 2;
    }
    const std::string root = AYTHER_SOURCE_DIR;
    std::ifstream cfg(root + "/tests/test_config.toml");
    std::string core, line;
    while (std::getline(cfg, line))
        if (line.find("core") != std::string::npos && line.find('=') != std::string::npos && core.empty())
            core = toml_quoted(line);
    core = resolve(core, root);
    const char* rom = ayther::env_get("AYTHER_PROBE_ROM");
    if (core.empty() || !rom) { std::fprintf(stderr, "[FAIL] falta core/AYTHER_PROBE_ROM\n"); return 2; }

    // AYTHER_PROBE_REALTIME=1: reproduce como el Lab — device de audio REAL +
    // pacing al fps del juego. La degradación de ENTREGA se mide sola: el
    // AudioPlayer loguea "[AudioPlayer] ... starving" cuando el backlog raspa
    // el fondo, y acá se reportan los peores frame-times (picos > presupuesto).
    const bool realtime = ayther::env_get("AYTHER_PROBE_REALTIME") != nullptr;

    // Gotcha conocido (lab-audio-mudo): AudioPlayer abre el device pero el
    // SUBSISTEMA lo inicializa el host — sin esto el device falla y no hay
    // starving que medir.
    if (realtime && !SDL_InitSubSystem(SDL_INIT_AUDIO))
        std::fprintf(stderr, "[WARN] SDL_INIT_AUDIO fallo: %s\n", SDL_GetError());

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = realtime;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create\n"); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;

    auto rec = ayther::AytherRecording::load(argv[2]);
    if (!rec) { std::fprintf(stderr, "[FAIL] arp\n"); return 1; }
    const uint32_t f0 = (uint32_t)std::atoi(argv[3]);
    const uint32_t n  = (uint32_t)std::atoi(argv[4]);

    const auto poses = load_poses(argv[1]);
    std::vector<ayther::AytherSession::PosePreview> pvs;
    for (const auto& p : poses) {
        if (p.asset.empty() || p.rel.size() != p.hashes.size()) continue;
        ayther::AytherSession::PosePreview pv;
        pv.hashes = p.hashes;
        pv.asset  = p.asset;
        pv.hd     = true;
        for (auto& rl : p.rel) { pv.rel_x.push_back(rl.first); pv.rel_y.push_back(rl.second); }
        if (p.dims.size() == p.hashes.size())
            for (auto& d : p.dims) { pv.dim_w.push_back(d.first); pv.dim_h.push_back(d.second); }
        pvs.push_back(std::move(pv));
    }
    std::printf("=== hd_audio_probe ===\nposes con HD: %zu · rango f%u..f%u\n\n",
                pvs.size(), f0, f0 + n);

    // Un pase de PLAYBACK secuencial (como el Lab): seek(f0), luego f0+1.. — el
    // fast-path avanza bare desde el estado actual. Devuelve el estado final +
    // ms/frame. En realtime, cada frame se PACEA al período del juego y se
    // registran los peores frame-times (un pico > período = el device pierde).
    const double period_ms = 1000.0 / (s->timing_fps() > 1.0 ? s->timing_fps() : 60.0);
    auto playback = [&](bool with_hd, std::vector<uint8_t>& out_state) -> double {
        s->replay_invalidate();
        s->set_pose_preview(with_hd ? pvs
                                    : std::vector<ayther::AytherSession::PosePreview>{});
        std::vector<double> worst;
        const auto t0 = std::chrono::steady_clock::now();
        for (uint32_t f = f0; f < f0 + n; ++f) {
            const auto fs = std::chrono::steady_clock::now();
            if (!s->replay_seek(*rec, f)) { std::fprintf(stderr, "[FAIL] seek f%u\n", f); return -1; }
            const auto fe = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(fe - fs).count();
            worst.push_back(ms);
            if (realtime) {
                // Pacing por reloj real al período del juego. HÍBRIDO: sleep
                // grueso + spin fino — el sleep_for pelado tiene granularidad
                // ~15.6 ms en Windows y el jitter tapaba la medición.
                const auto deadline = fs + std::chrono::duration_cast<
                    std::chrono::steady_clock::duration>(
                        std::chrono::duration<double, std::milli>(period_ms));
                auto now2 = std::chrono::steady_clock::now();
                if (deadline - now2 > std::chrono::milliseconds(3))
                    std::this_thread::sleep_for(
                        (deadline - now2) - std::chrono::milliseconds(2));
                while (std::chrono::steady_clock::now() < deadline) { /* spin */ }
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        s->set_pose_preview({});
        if (realtime && !worst.empty()) {
            std::sort(worst.begin(), worst.end(), std::greater<double>());
            std::printf("  peores frame-times (%s HD): %.1f %.1f %.1f %.1f %.1f ms  (presupuesto %.1f)\n",
                        with_hd ? "con" : "sin",
                        worst[0], worst.size() > 1 ? worst[1] : 0.0,
                        worst.size() > 2 ? worst[2] : 0.0,
                        worst.size() > 3 ? worst[3] : 0.0,
                        worst.size() > 4 ? worst[4] : 0.0, period_ms);
        }
        out_state.clear();
        (void)s->serialize(out_state);
        return std::chrono::duration<double, std::milli>(t1 - t0).count() / n
        ;
    };

    std::vector<uint8_t> st_off, st_on;
    const double ms_off = playback(false, st_off);
    const double ms_on  = playback(true,  st_on);
    if (ms_off < 0 || ms_on < 0) return 1;

    std::printf("ms/frame  sin HD: %.3f   con HD: %.3f   (x%.2f)\n",
                ms_off, ms_on, ms_off > 0 ? ms_on / ms_off : 0.0);

    // #153: desglose del sobrecosto del compose — cuánto de la diferencia es
    // el COPIADO de estado (serialize A → unserialize B, por frame) vs la
    // RE-EMULACIÓN del frame en el shadow (inherente al diseño de dos pasadas).
    {
        std::vector<uint8_t> st;
        constexpr int kIters = 200;
        const auto s0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kIters; ++i) { st.clear(); (void)s->serialize(st); }
        const auto s1 = std::chrono::steady_clock::now();
        for (int i = 0; i < kIters; ++i) (void)s->unserialize(st);
        const auto s2 = std::chrono::steady_clock::now();
        const double ser_ms = std::chrono::duration<double, std::milli>(s1 - s0).count() / kIters;
        const double uns_ms = std::chrono::duration<double, std::milli>(s2 - s1).count() / kIters;
        std::printf("estado: %zu KB  ·  serialize %.3f ms  ·  unserialize %.3f ms  "
                    "(copiado/frame ~%.3f ms de %.3f de sobrecosto HD)\n",
                    st.size() / 1024, ser_ms, uns_ms, ser_ms + uns_ms,
                    ms_on - ms_off);
    }

    if (st_off.size() != st_on.size()) {
        std::printf("[DIVERGE] tamaño de estado: %zu vs %zu\n", st_off.size(), st_on.size());
        return 1;
    }
    size_t first = SIZE_MAX, count = 0;
    for (size_t i = 0; i < st_off.size(); ++i)
        if (st_off[i] != st_on[i]) { if (first == SIZE_MAX) first = i; ++count; }
    if (count == 0) {
        std::printf("[OK] estado del core IDENTICO tras el playback con y sin HD\n");
        return 0;
    }
    std::printf("[DIVERGE] %zu/%zu bytes difieren (primero en offset %zu de %zu)\n"
                "→ el produce con compose FILTRA estado al chain de replay:\n"
                "  la continuidad (audio incluido) con HD activo NO es la del original.\n",
                count, st_off.size(), first, st_off.size());
    return 1;
}
