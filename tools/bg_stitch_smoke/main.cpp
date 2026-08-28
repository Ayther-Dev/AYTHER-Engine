// ---------------------------------------------------------------------------
// bg_stitch_smoke — valida el BackgroundStitcher (Fondos, Caso B/C). Graba una
// toma con SCROLL (entra al juego con START, luego avanza a la derecha grabando),
// replica la lógica de host_background::stitch_plane_layer recorriendo la toma con
// replay_seek, des-enrolla el scroll a espacio de nivel y reconstruye la tira
// completa de un plano. Escribe PPM (RGB) para Plano A y B + reporta el rango de
// scroll y las dimensiones reconstruidas.
//
// El RIESGO que valida: el signo/des-enrollado del scroll. Si está mal, la tira
// sale del ancho de una pantalla (no acumula) o sale gigante/duplicada.
//
//   Build:  -DAYTHER_BUILD_SPIKE=ON  →  target bg_stitch_smoke
//   Run:    bin/bg_stitch_smoke[.exe] [out_dir]   (rutas de tests/test_config.toml)
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"

#include <array>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif

using ayther::FrameView;
using ayther::PlaneCellHit;
using ayther::PlaneTileOccurrence;

static std::string quoted(const std::string& l) {
    auto a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string() : l.substr(a + 1, b - a - 1);
}
static std::string resolve(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\') return p;
    return base + "/" + p;
}
static long fdiv(long a, long b) { long q = a / b, r = a % b; if (r && ((r < 0) != (b < 0))) --q; return q; }

// Replica stitch_plane_layer (host_background.cpp) → escribe un PPM. Devuelve las
// dimensiones en celdas (cw,ch) o {0,0} si no acumuló nada.
static std::pair<long,long> stitch(ayther::AytherSession* s, const ayther::AytherRecording& rec,
                                   int plane, const std::string& ppm_path,
                                   int& hmin, int& hmax) {
    const uint32_t nframes = rec.frame_count();
    using Tile = std::array<uint8_t, 8*8*4>;
    std::map<std::pair<long,long>, Tile> cells;
    long cam_x = 0, cam_y = 0; int prev_h = 0, prev_v = 0; bool first = true;
    int wrapw = 512, wraph = 512;
    long minx = LONG_MAX, miny = LONG_MAX, maxx = LONG_MIN, maxy = LONG_MIN;
    long cam_min = LONG_MAX, cam_max = LONG_MIN;

    for (uint32_t f = 0; f < nframes; ++f) {
        const FrameView* fv = s->replay_seek(rec, f);
        if (!fv) continue;
        if (fv->plane_wpx) wrapw = fv->plane_wpx;
        if (fv->plane_hpx) wraph = fv->plane_hpx;
        const int H = fv->plane_hscroll[plane], V = fv->plane_vscroll[plane];
        if (H < hmin) hmin = H; if (H > hmax) hmax = H;
        if (first) { prev_h = H; prev_v = V; first = false; }
        int dh = H - prev_h; if (dh >  wrapw/2) dh -= wrapw; else if (dh < -wrapw/2) dh += wrapw;
        int dv = V - prev_v; if (dv >  wraph/2) dv -= wraph; else if (dv < -wraph/2) dv += wraph;
        cam_x += -dh; cam_y += -dv; prev_h = H; prev_v = V;
        if (cam_x < cam_min) cam_min = cam_x; if (cam_x > cam_max) cam_max = cam_x;
        if (fv->plane_cell_count == 0) continue;

        std::unordered_map<uint64_t, const PlaneTileOccurrence*> byhash;
        for (uint32_t i = 0; i < fv->plane_tile_occ_count; ++i)
            byhash[fv->plane_tile_occs[i].hash] = &fv->plane_tile_occs[i];

        for (uint32_t i = 0; i < fv->plane_cell_count; ++i) {
            const PlaneCellHit& pc = fv->plane_cells[i];
            if (pc.plane != plane) continue;
            const std::pair<long,long> key{ fdiv(cam_x + pc.screen_x, 8), fdiv(cam_y + pc.screen_y, 8) };
            if (cells.count(key)) continue;
            auto it = byhash.find(pc.hash);
            if (it == byhash.end()) continue;
            const PlaneTileOccurrence* o = it->second;
            Tile t{};
            s->decode_plane_tile(o->pattern, o->palette, o->hflip != 0, o->vflip != 0, t.data());
            cells.emplace(key, t);
            if (key.first  < minx) minx = key.first;  if (key.first  > maxx) maxx = key.first;
            if (key.second < miny) miny = key.second; if (key.second > maxy) maxy = key.second;
        }
    }
    std::printf("[plane %d] scroll H=[%d..%d] cam_x=[%ld..%ld] tiles=%zu\n",
                plane, hmin, hmax, cam_min == LONG_MAX ? 0 : cam_min,
                cam_max == LONG_MIN ? 0 : cam_max, cells.size());
    if (cells.empty()) return {0, 0};
    const long cw = maxx - minx + 1, ch = maxy - miny + 1;
    const int W = (int)cw * 8, H = (int)ch * 8;
    std::vector<uint8_t> img((size_t)W * H * 3, 0);
    for (const auto& kv : cells) {
        const int ox = (int)(kv.first.first - minx) * 8, oy = (int)(kv.first.second - miny) * 8;
        for (int ty = 0; ty < 8; ++ty)
            for (int tx = 0; tx < 8; ++tx) {
                const uint8_t* src = kv.second.data() + (ty*8+tx)*4;       // BGRA
                uint8_t* dst = img.data() + ((size_t)(oy+ty)*W + (ox+tx))*3;
                dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0];          // BGR→RGB
            }
    }
    std::ofstream o(ppm_path, std::ios::binary);
    o << "P6\n" << W << " " << H << "\n255\n";
    o.write(reinterpret_cast<const char*>(img.data()), (std::streamsize)img.size());
    std::printf("       → %s  (%d x %d px, %ld x %ld celdas)\n", ppm_path.c_str(), W, H, cw, ch);
    return {cw, ch};
}

int main(int argc, char* argv[]) {
    const std::string root = AYTHER_SOURCE_DIR;
    const std::string outd = argc > 1 ? argv[1] : ".";
    const std::string want = argc > 2 ? argv[2] : "aladdin";   // nombre del rom (test_config)
    std::ifstream cfg(root + "/tests/test_config.toml");
    if (!cfg) { std::fprintf(stderr, "[skip] sin tests/test_config.toml\n"); return 0; }
    std::string core, line;
    std::vector<std::pair<std::string,std::string>> roms;   // (name, path)
    std::string cur_name;
    bool in_rom = false;
    while (std::getline(cfg, line)) {
        if (line.find("[[rom]]") != std::string::npos) { in_rom = true; cur_name.clear(); continue; }
        if (!in_rom && line.find("core") != std::string::npos && line.find('=') != std::string::npos && core.empty())
            core = quoted(line);
        if (in_rom && line.find("name") != std::string::npos && line.find('=') != std::string::npos)
            cur_name = quoted(line);
        if (in_rom && line.find("path") != std::string::npos && line.find('=') != std::string::npos)
            roms.emplace_back(cur_name, quoted(line));
    }
    std::string rom;
    for (const auto& [n, p] : roms) if (n == want) { rom = p; break; }
    if (rom.empty() && !roms.empty()) rom = roms.front().second;   // fallback al 1º
    core = resolve(core, root); rom = resolve(rom, root);
    if (core.empty() || rom.empty()) { std::fprintf(stderr, "[FAIL] config incompleta\n"); return 1; }
    std::printf("=== bg_stitch_smoke ===\ncore: %s\nrom : %s\nout : %s\n\n", core.c_str(), rom.c_str(), outd.c_str());

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;

    // Entrar al juego (START), luego avanzar a la derecha GRABANDO → toma con scroll.
    // Bits libretro JOYPAD (retro_runner.cpp:208): START=3, RIGHT=7, B(salto)=0.
    constexpr uint16_t START = 1u << 3, RIGHT = 1u << 7, A_BTN = 1u << 0;
    for (int i = 0; i < 900; ++i) { s->set_input(0, (i % 24 < 6) ? START : 0); s->step(); }
    s->record_start();
    for (int i = 0; i < 600; ++i) { s->set_input(0, RIGHT | ((i % 48 < 4) ? A_BTN : 0u)); s->step(); }
    s->record_stop();
    ayther::AytherRecording take = s->take_recording();
    std::printf("[ok] toma: %u frames\n\n", take.frame_count());

    int hminA = INT_MAX, hmaxA = INT_MIN, hminB = INT_MAX, hmaxB = INT_MIN;
    auto a = stitch(s.get(), take, 0, outd + "/bg_stitch_A.ppm", hminA, hmaxA);
    auto b = stitch(s.get(), take, 1, outd + "/bg_stitch_B.ppm", hminB, hmaxB);

    // Veredicto: al menos un plano scrolleó (rango de scroll grande) y reconstruyó
    // una tira MÁS ANCHA que una pantalla (≈40 celdas = 320px) → el des-enrollado
    // acumuló en vez de pisar sobre la misma pantalla.
    const bool scrolledA = (hmaxA - hminA) > 64, scrolledB = (hmaxB - hminB) > 64;
    const bool wideA = a.first > 48, wideB = b.first > 48;
    std::printf("\nscroll A=%d B=%d  ·  tira ancha A=%d(%ldcel) B=%d(%ldcel)\n",
                hmaxA - hminA, hmaxB - hminB, wideA, a.first, wideB, b.first);
    const bool ok = (scrolledA && wideA) || (scrolledB && wideB);
    std::printf("\n%s — %s\n", ok ? "OK" : "REVISAR",
                ok ? "un plano scrolleó y la tira reconstruida es más ancha que la pantalla"
                   : "ningún plano produjo una tira ancha (¿no entró al nivel? ¿signo del scroll?)");
    return ok ? 0 : 2;
}
