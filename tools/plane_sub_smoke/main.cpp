// ---------------------------------------------------------------------------
// plane_sub_smoke — valida el RESOLVER scroll-aware de la Fase 2c: que
// AytherSession resuelve las posiciones en pantalla de un tile de plano asignado.
//
// Oráculo de POSICIÓN: la Fase 2b (ocultar un patrón) cambia el framebuffer
// EXACTAMENTE en las celdas donde ese patrón está en pantalla. Así:
//   1. asignar un patrón de Plano A (assign_plane → dummy) → el resolver emite
//      subs en posiciones {(sx,sy)} (plane_tile_subs).
//   2. ocultar ESE patrón (set_plane_tile_hidden) → diff del fb → celdas 8×8
//      cambiadas = posiciones VERDADERAS.
//   3. comparar: las celdas de los subs ⊆ las celdas cambiadas (si el SIGNO del
//      scroll está mal, las posiciones caen en el lado opuesto → no coinciden).
//
//   Build:  -DAYTHER_BUILD_SPIKE=ON  →  target plane_sub_smoke
//   Run:    bin/plane_sub_smoke[.exe]   (toma rutas de tests/test_config.toml)
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_recording.h"
#include "ayther_core_ffi.h"

#include <cstdint>
#include <cstdio>
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

// Copia el framebuffer (BGRA/RGB565 — comparamos bytes crudos por celda).
static std::vector<uint8_t> snap_fb(const FrameView* fv) {
    if (!fv || !fv->fb_pixels || !fv->fb_height || !fv->fb_pitch) return {};
    const uint8_t* p = static_cast<const uint8_t*>(fv->fb_pixels);
    return std::vector<uint8_t>(p, p + (size_t)fv->fb_height * fv->fb_pitch);
}

int main() {
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
    if (core.empty() || rom.empty()) { std::fprintf(stderr, "[FAIL] config incompleta\n"); return 1; }
    std::printf("=== plane_sub_smoke ===\ncore: %s\nrom : %s\n\n", core.c_str(), rom.c_str());

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;

    constexpr uint16_t START = 1u << 3, RIGHT = 1u << 6;
    for (int i = 0; i < 900; ++i) { s->set_input(0, (i % 24 < 6) ? START : 0); s->step(); }
    s->record_start();
    for (int i = 0; i < 360; ++i) { s->set_input(0, RIGHT | ((i % 48 < 4) ? 1u : 0u)); s->step(); }
    s->record_stop();
    ayther::AytherRecording take = s->take_recording();
    std::printf("[ok] toma: %u frames\n", take.frame_count());

    int fail = 0;
    auto check = [&](bool ok, const char* m) { std::printf("[%s] %s\n", ok ? "ok" : "FAIL", m); if (!ok) ++fail; };

    // Frame con Plano A visible — control por el INVENTARIO (R-5: la máscara
    // de capas ya no toca el fb, así que el sabido-bueno viejo no discrimina).
    // Se elige uno TARDÍO (el personaje ya scrolleó mucho) para que el scroll
    // sea grande → sensible al SIGNO.
    uint32_t F = UINT32_MAX;
    for (uint32_t f = take.frame_count() > 60 ? take.frame_count() - 60 : 0; f >= 30; f -= 10) {
        s->replay_invalidate();
        const FrameView* p = s->replay_seek(take, f);
        uint32_t cellsA = 0;
        for (uint32_t i = 0; p && i < p->plane_cell_count; ++i)
            if (p->plane_cells[i].plane == 0) ++cellsA;
        if (cellsA > 16) { F = f; break; }
        if (f < 40) break;
    }
    check(F != UINT32_MAX, "hay un frame con Plano A visible (control)");
    if (F == UINT32_MAX) { std::printf("\nFALLÓ\n"); return 1; }
    std::printf("[..] frame F=%u\n", F);

    const int W = (int)s->replay_seek(take, F)->fb_width;
    const int H = (int)s->replay_seek(take, F)->fb_height;
    const int pitch = (int)s->replay_seek(take, F)->fb_pitch;
    const int bpp = (W > 0) ? pitch / W : 2;       // bytes por pixel
    const int gcols = W / 8, grows = H / 8;

    // Probar patrones de Plano A hasta hallar uno que el resolver ubique en pantalla.
    uint64_t chosen = 0; std::vector<std::pair<int,int>> subcells;  // celdas 8×8 de los subs
    {
        const FrameView* fv = s->replay_seek(take, F);
        std::set<uint64_t> tried;
        for (uint32_t i = 0; i < fv->plane_tile_occ_count && !chosen; ++i) {
            const auto& o = fv->plane_tile_occs[i];
            if (o.plane != 0 || !tried.insert(o.hash).second) continue;
            s->assign_plane(o.hash, "dummy.png");
            const FrameView* fv2 = nullptr;
            for (int k = 0; k < 4; ++k) { s->replay_invalidate(); fv2 = s->replay_seek(take, F); }
            if (fv2 && fv2->plane_tile_sub_count > 0) {
                chosen = o.hash;
                for (uint32_t j = 0; j < fv2->plane_tile_sub_count; ++j)
                    subcells.emplace_back(fv2->plane_tile_subs[j].screen_x / 8,
                                          fv2->plane_tile_subs[j].screen_y / 8);
            } else {
                s->unassign_plane(o.hash);
            }
        }
    }
    check(chosen != 0, "el resolver ubicó en pantalla algún tile de Plano A asignado");
    if (!chosen) { std::printf("\nFALLÓ (%d)\n", fail); return 1; }
    std::printf("[..] patrón elegido 0x%016llx → %zu subs\n",
                (unsigned long long)chosen, subcells.size());

    // Oráculo R-5 (#274): la supresión 0x105 murió (el fb ya no es lienzo).
    // Las posiciones VERDADERAS del patrón salen del INVENTARIO de escena —
    // que el compose GPU probó byte-idéntico al fb (scene_compose_smoke) —
    // más una guardia: ocultar el patrón por elemento NO toca el fb.
    s->replay_invalidate(); auto fb_vis = snap_fb(s->replay_seek(take, F));
    ayther::HiddenElement he{ chosen, 1 };   // capa 1 = Plano A
    s->set_hidden_elements(&he, 1);
    const FrameView* fvh = nullptr;
    for (int k = 0; k < 4; ++k) { s->replay_invalidate(); fvh = s->replay_seek(take, F); }
    check(snap_fb(fvh) == fb_vis,
          "ocultar por elemento NO toca el fb (canal de supresión muerto)");
    s->set_hidden_elements(nullptr, 0);

    std::set<std::pair<int,int>> changed;   // celdas del patrón según el inventario
    {
        std::vector<ayther::SceneElement> inv;
        s->scene_inventory(inv);
        for (const auto& e : inv)
            if (e.layer == 1 && e.hash == chosen && e.x >= 0 && e.y >= 0)
                changed.insert({ e.x / 8, e.y / 8 });
    }
    std::printf("[..] celdas del patrón en el inventario=%zu\n", changed.size());
    (void)pitch; (void)bpp; (void)grows; (void)gcols;

    // Métrica robusta a oclusión: TODA celda realmente cambiada (= el patrón se veía
    // ahí) debe tener un sub a ≤1 celda. La oclusión agrega subs de más (no faltantes),
    // así que no baja esta cobertura; un SIGNO de scroll errado SÍ la hunde (los subs
    // caen lejos). Se usa un set de celdas de sub para la búsqueda por vecindad.
    std::set<std::pair<int,int>> subset(subcells.begin(), subcells.end());
    auto sub_near = [&](int gx, int gy) {
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                if (subset.count({gx + dx, gy + dy})) return true;
        return false;
    };
    int covered = 0;
    for (auto [gx, gy] : changed) if (sub_near(gx, gy)) ++covered;
    const double frac = changed.empty() ? 0.0 : (double)covered / changed.size();
    std::printf("[..] celdas cambiadas con un sub cerca: %d/%zu (%.0f%%)\n",
                covered, changed.size(), frac * 100.0);
    check(frac >= 0.7, "las posiciones del resolver cubren el oráculo (scroll/signo OK)");

    std::printf("\n%s (%d fallos)\n", fail ? "FALLÓ" : "OK", fail);
    return fail ? 1 : 0;
}
