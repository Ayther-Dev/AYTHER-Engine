// ---------------------------------------------------------------------------
// cam_track_smoke — valida la cámara en espacio de NIVEL (EM-1 #226): el
// unwrap secuencial de scroll de FrameView.plane_cam_x/y.
//
// Oráculos:
//   1. CONSISTENCIA: recorriendo la toma secuencialmente, la cámara del motor
//      coincide frame a frame con un unwrap independiente hecho acá (el mismo
//      algoritmo del stitcher) desde plane_hscroll/vscroll.
//   2. ESTABILIDAD DE NIVEL (el AC del epic): durante un tramo con SCROLL, el
//      conjunto de posiciones de nivel (cam + screen) de las celdas de Plano A
//      se conserva mayormente entre frames — el hash no cambia y el nivel_x
//      tampoco, aunque el screen_x se corra 1..8 px.
//   3. RE-ANCLAJE: un seek con salto invalida la cámara (plane_cam_valid
//      false, cam re-anclada en 0) y el próximo frame secuencial la revalida.
//
//   Requiere un juego cuyo plano scrollee ENTERO (el H whole-plane se
//   muestrea en la línea 0): Sonic 2 sirve (mismo corpus que el stitcher);
//   Aladdin/GA no (HUD de plano fijo en la línea 0 → señal 0).
//
//   Build:  -DAYTHER_BUILD_SPIKE=ON  →  target cam_track_smoke
//   Run:    bin/cam_track_smoke [core.dll rom]  (default tests/test_config.toml)
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif

using ayther::FrameView;

static std::string quoted(const std::string& l) {
    auto a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string() : l.substr(a + 1, b - a - 1);
}
static std::string resolve(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\') return p;
    return base + "/" + p;
}

int main(int argc, char** argv) {
    const std::string root = AYTHER_SOURCE_DIR;
    std::ifstream cfg(root + "/tests/test_config.toml");
    if (!cfg) { std::fprintf(stderr, "[skip] sin tests/test_config.toml\n"); return 0; }
    std::string core, rom, line; bool in_rom = false;
    while (std::getline(cfg, line)) {
        if (line.find("[[rom]]") != std::string::npos) in_rom = true;
        else if (line.find("core") != std::string::npos && line.find('=') != std::string::npos && core.empty())
            core = quoted(line);
        else if (in_rom && rom.empty() && line.find("path") != std::string::npos) rom = quoted(line);
    }
    core = resolve(core, root); rom = resolve(rom, root);
    if (argc >= 3) { core = argv[1]; rom = argv[2]; }
    if (core.empty() || rom.empty()) { std::fprintf(stderr, "[FAIL] config incompleta\n"); return 1; }
    std::printf("=== cam_track_smoke ===\ncore: %s\nrom : %s\n\n", core.c_str(), rom.c_str());

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;

    // Boot estilo background_smoke (Sonic 2): START cada 32 frames hasta entrar
    // al nivel, después correr a la DERECHA (bit 7 del RetroPad).
    constexpr uint16_t START = 1u << 3, RIGHT = 1u << 7;
    for (int i = 0; i < 1400; ++i) { s->set_input(0, (i % 32 == 0) ? START : 0); s->step(); }
    for (int i = 0; i < 200; ++i)  { s->set_input(0, RIGHT); s->step(); }
    s->record_start();
    for (int i = 0; i < 360; ++i)  { s->set_input(0, RIGHT); s->step(); }
    s->record_stop();
    ayther::AytherRecording take = s->take_recording();
    std::printf("[ok] toma: %u frames\n", take.frame_count());

    int fail = 0;
    auto check = [&](bool ok, const char* m) { std::printf("[%s] %s\n", ok ? "ok" : "FAIL", m); if (!ok) ++fail; };

    // ── 1. Consistencia con un unwrap independiente, barrido secuencial ─────
    const uint32_t N = take.frame_count();
    int32_t exp_cam[2] = {0, 0};
    int16_t prev_h[2]  = {0, 0};
    bool    first      = true;
    bool    consistent = true;
    bool    ever_valid = false;
    int     scrolled   = 0;   // frames con delta de scroll ≠ 0 (Plano A)
    std::vector<int32_t> camA(N, 0);
    std::vector<bool>    plane_a_valid(N, false);
    int32_t max_jump = 0;   // mayor salto de cámara en un frame secuencial
    int     hmax     = 0;   // mayor H crudo visto (¿el campo pasa de 10 bits?)
    for (uint32_t f = 0; f < N; ++f) {
        const FrameView* fv = s->replay_seek(take, f);
        if (!fv) { consistent = false; break; }
        hmax = std::max(hmax, (int)fv->plane_hscroll[0]);
        if (!first) {
            for (int p = 0; p < 2; ++p) {
                // Des-wrapear en el módulo en que REALMENTE wrapea: el ancho
                // del plano. El campo del VDP es de 10 bits y el juego puede
                // escribir por encima de wpx, así que primero se REDUCE.
                const int m = fv->plane_wpx ? fv->plane_wpx : 512;
                int dh = (fv->plane_hscroll[p] % m) - (prev_h[p] % m);
                if (dh > m / 2) dh -= m; else if (dh < -m / 2) dh += m;
                if (p == 0 && dh != 0) ++scrolled;
                exp_cam[p] += -dh;
            }
            if (fv->plane_cam_valid) {
                ever_valid = true;
                if (fv->plane_cam_x[0] != exp_cam[0] ||
                    fv->plane_cam_x[1] != exp_cam[1])
                    consistent = false;
        if (f > 0 && plane_a_valid[f - 1])
                    max_jump = std::max(max_jump,
                                        std::abs(fv->plane_cam_x[0] - camA[f - 1]));
            }
        }
        prev_h[0] = fv->plane_hscroll[0];
        prev_h[1] = fv->plane_hscroll[1];
        camA[f]   = fv->plane_cam_x[0];
        plane_a_valid[f] = fv->plane_cam_valid;
        first = false;
    }
    check(ever_valid, "el tracking se activa en el barrido secuencial");
    check(consistent, "la camara del motor coincide con el unwrap independiente");
    check(scrolled > 30, "la toma tiene scroll real (control del oraculo)");
    // Una cámara física no salta: el motor mismo considera un corte de escena
    // cualquier delta > 32 px (kBgMaxStepPx). Este chequeo es el que delata un
    // unwrap con el módulo equivocado — con módulo wpx en vez del campo de 10
    // bits, el borde 1023→0 metía un salto de ~511 px.
    check(max_jump <= 64, "la camara no pega saltos (unwrap con el modulo correcto)");
    std::printf("[..] frames con scroll: %d · cam_x[A] final = %d · salto maximo = %d px "
                "· H crudo maximo = %d (el campo del VDP es de 10 bits)\n",
                scrolled, (int)exp_cam[0], (int)max_jump, hmax);

    // ── 2. Estabilidad de nivel bajo scroll (AC del epic) ───────────────────
    // Dos frames con scroll entre medio: el multiset de (hash, nivel_x) de
    // las celdas de Plano A debe conservarse mayormente (los tiles del nivel
    // no se mueven; solo entra/sale el borde).
    {
        uint32_t f0 = 0;
        for (uint32_t f = N / 2; f + 6 < N; ++f)
        if (plane_a_valid[f] && plane_a_valid[f + 5] && camA[f + 5] != camA[f]) { f0 = f; break; }
        check(f0 != 0, "hay un tramo valido con scroll para el AC");
        if (f0 != 0) {
            auto level_set = [&](uint32_t f) {
                std::multiset<std::pair<uint64_t, int32_t>> out;
                const FrameView* fv = s->replay_seek(take, f);
                for (uint32_t i = 0; fv && i < fv->plane_cell_count; ++i) {
                    const auto& pc = fv->plane_cells[i];
                    if (pc.plane != 0) continue;
                    out.insert({ pc.hash, fv->plane_cam_x[0] + pc.screen_x });
                }
                return out;
            };
            // Re-barrer secuencial hasta f0/f0+5 (el seek directo re-ancla).
            for (uint32_t f = 0; f <= f0 + 5; ++f) s->replay_seek(take, f);
            const auto a = level_set(f0 + 0 == 0 ? 0 : f0);      // ya posicionado
            // seguir secuencial hasta f0+5
            for (uint32_t f = f0 + 1; f <= f0 + 5; ++f) s->replay_seek(take, f);
            const auto b = level_set(f0 + 5);
            size_t common = 0;
            for (const auto& e : a) if (b.count(e)) ++common;
            const double frac = a.empty() ? 0.0 : double(common) / double(a.size());
            std::printf("[..] estabilidad de nivel: %zu/%zu celdas conservan "
                        "(hash, nivel_x) = %.0f%%\n", common, a.size(), frac * 100.0);
            check(frac >= 0.7, "el (hash, nivel_x) es estable bajo scroll (>=70%)");
        }
    }

    // ── 3. Re-anclaje en seek con salto ─────────────────────────────────────
    {
        const FrameView* fv = s->replay_seek(take, 40);   // salto atras grande
        check(fv && !fv->plane_cam_valid, "un seek con salto INVALIDA la camara");
        check(fv && fv->plane_cam_x[0] == 0 && fv->plane_cam_x[1] == 0,
              "la camara re-ancla en 0 tras el salto");
        fv = s->replay_seek(take, 41);
        check(fv && fv->plane_cam_valid, "el frame secuencial siguiente revalida");
    }

    std::printf("\n%s (%d fallos)\n", fail ? "FALLÓ" : "OK", fail);
    return fail ? 1 : 0;
}
