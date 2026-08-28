// ---------------------------------------------------------------------------
// scene_fallback_probe (#505) — ¿en qué frames el compose indexado cae al
// BLIT del emulador (scene_dirty, R-5) y hay una Identidad MEJORADA en
// pantalla? El filtro de #493 trabaja sobre índices (inmune a los fundidos de
// CRAM), pero los frames de fallback no lo muestran: si pasa durante un fade,
// el suavizado parpadea. Medir, no adivinar.
//
// Por frame del tramo: modo (compose · fallback bit0 mid-frame / bit1 dim /
// bit2 hscroll por línea), celdas de escena y cuántas celdas pertenecen a los
// hashes mejorados pasados por argumento. Resume por tramos y cuenta los
// frames «malos» (fallback ∧ mejorada en pantalla).
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target scene_fallback_probe (sin GPU)
//   Args:  <rec.ayr> <from> <to> [hash ...]   (hash = 0x… de 16 hex)
//   Env:   AYTHER_PROBE_ROM
// ---------------------------------------------------------------------------
#include "ayther_env.h"
#include "ayther_session.h"
#include "ayther_recording.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif
using ayther::FrameView;
using SceneElement = ayther::AytherSession::SceneElement;

static std::string toml_quoted(const std::string& l) {
    const auto a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string() : l.substr(a + 1, b - a - 1);
}
static std::string resolve(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\') return p;
    return base + "/" + p;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "uso: scene_fallback_probe <rec> <from> <to> [hash ...]\n");
        return 2;
    }
    const std::string rec_path = argv[1];
    const uint32_t from = (uint32_t)std::strtoul(argv[2], nullptr, 10);
    const uint32_t to   = (uint32_t)std::strtoul(argv[3], nullptr, 10);
    std::unordered_set<uint64_t> enhanced;
    for (int i = 4; i < argc; ++i) enhanced.insert(std::strtoull(argv[i], nullptr, 16));

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

    std::printf("=== scene_fallback_probe (#505) ===  %s  frames %u..%u  (%zu hashes mejorados)\n\n",
                rec_path.c_str(), from, to, enhanced.size());
    std::printf("frame | modo      | dirty | celdas | mejoradas | nota\n");

    uint32_t n_frames = 0, n_fallback = 0, n_bad = 0, n_enh_frames = 0;
    uint32_t run_start = from; int run_mode = -1; uint32_t run_enh = 0;
    auto flush_run = [&](uint32_t end) {
        if (run_mode < 0) return;
        std::printf("  tramo %5u..%5u  %s%s\n", run_start, end,
                    run_mode ? "FALLBACK" : "compose ",
                    run_enh ? "  (con Identidad mejorada en pantalla)" : "");
    };
    std::vector<SceneElement> inv;
    for (uint32_t f = from; f <= to; ++f) {
        const FrameView* fv = s->replay_seek(*rec, f);
        if (!fv || !fv->fb_width) { std::printf("%5u | (sin frame)\n", f); continue; }
        ++n_frames;
        inv.clear();
        s->scene_inventory(inv);
        uint32_t enh_cells = 0;
        for (const SceneElement& e : inv)
            if (enhanced.count(e.hash)) ++enh_cells;
        const bool fallback = fv->scene_dirty != 0 || !fv->scene_count;
        const int  mode = fallback ? 1 : 0;
        if (fallback) ++n_fallback;
        if (enh_cells) ++n_enh_frames;
        if (fallback && enh_cells) ++n_bad;
        char note[96] = "";
        if (fv->scene_dirty) {
            std::snprintf(note, sizeof(note), "%s%s%s",
                          (fv->scene_dirty & 1) ? "mid-frame " : "",
                          (fv->scene_dirty & 2) ? "dim " : "",
                          (fv->scene_dirty & 4) ? "hscroll/línea " : "");
        } else if (!fv->scene_count) {
            std::snprintf(note, sizeof(note), "sin escena");
        }
        std::printf("%5u | %-9s | 0x%02x  | %6u | %9u | %s%s\n", f,
                    fallback ? "FALLBACK" : "compose", fv->scene_dirty,
                    (unsigned)fv->scene_count, enh_cells, note,
                    (fallback && enh_cells) ? " <== mejora invisible" : "");
        if (mode != run_mode || (enh_cells != 0) != (run_enh != 0)) {
            flush_run(f ? f - 1 : 0);
            run_start = f; run_mode = mode; run_enh = enh_cells;
        }
    }
    flush_run(to);
    std::printf("\nframes %u · fallback %u (%.1f%%) · con mejorada en pantalla %u · "
                "fallback CON mejorada %u\n",
                n_frames, n_fallback, n_frames ? 100.0 * n_fallback / n_frames : 0.0,
                n_enh_frames, n_bad);
    std::printf("%s\n", n_bad ? "VEREDICTO: el suavizado PARPADEA en este tramo (abrir issue de fondo)"
                              : "VEREDICTO: nada que hacer en este tramo");
    return 0;
}
