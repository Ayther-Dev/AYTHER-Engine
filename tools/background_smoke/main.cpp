// ---------------------------------------------------------------------------
// background_smoke — valida el WIRING de Fondos en AytherSession: bg_capture
// acumula la tira del nivel (stitcher + unwrap del scroll del VDP, el camino
// validado por tools/background_spike) y export_backgrounds escribe el PNG por
// capa con el índice en el nombre.
//
// Oráculo: correr ~5s de EHZ scrolleando → el plano A debe acumular una tira
// MÁS ANCHA que una pantalla (el punto del stitcher: ningún frame la contiene
// entera) → exportar → releer el PNG con stb_image → dimensiones = las de la
// tira acumulada, con píxeles opacos (contenido real decodificado de CRAM).
//
//   Build:  -DAYTHER_BUILD_SPIKE=ON  →  target background_smoke
//   Run:    bin/background_smoke <core_vram.dll> <sonic2.md> [out_dir]
// ---------------------------------------------------------------------------
#include "ayther_session.h"

#include <stb_image.h>   // declarations; la impl vive en el engine (tile_tex_cache)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

using ayther::AytherSession;
using ayther::FrameView;

static constexpr uint32_t OFF_SONIC_X = 0xB008;
static constexpr uint32_t OFF_SONIC_Y = 0xB00C;

static constexpr uint16_t BTN_START = 1u << 3;
static constexpr uint16_t BTN_RIGHT = 1u << 7;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "background_smoke — session-level wiring check for background capture+export\n"
            "usage: %s <core.dll> <rom> [out_dir]\n", argv[0]);
        return 2;
    }
    const std::string core = argv[1], rom = argv[2];
    const std::string out_dir = argc > 3 ? argv[3] : ".";

    AytherSession::Config cfg;
    cfg.core_path = core; cfg.rom_path = rom; cfg.enable_audio = false;
    auto r = AytherSession::create(cfg);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<AytherSession>& s = *r;
    std::printf("=== background_smoke ===\ncore: %s\nrom : %s\nout : %s\n\n",
                core.c_str(), rom.c_str(), out_dir.c_str());

    int fail = 0;
    auto check = [&](bool ok, const char* m) {
        std::printf("[%s] %s\n", ok ? "ok" : "FAIL", m);
        if (!ok) ++fail;
    };

    // Warm-up adaptativo hasta gameplay + pre-roll (pasar el arranque casi quieto,
    // donde lo que domina es la animación decorativa, no el scroll).
    int reached = -1;
    for (int i = 0; i < 1200; ++i) {
        s->set_input(0, (i % 32 == 0) ? BTN_START : 0);
        s->step();
        if (s->ram_u16(OFF_SONIC_X) != 0 && s->ram_u16(OFF_SONIC_Y) != 0) { reached = i; break; }
    }
    check(reached >= 0, "gameplay alcanzado");
    if (reached < 0) { std::printf("\nFALLÓ\n"); return 1; }
    for (int i = 0; i < 200; ++i) { s->set_input(0, BTN_RIGHT); s->step(); }

    // Captura: ~5.3s scrolleando a la derecha.
    check(!s->bg_capturing(), "la captura arranca apagada");
    s->bg_capture(true);
    check(s->bg_capturing(), "bg_capture(true) enciende la captura");
    const FrameView* fv = nullptr;
    for (int i = 0; i < 320; ++i) { s->set_input(0, BTN_RIGHT); fv = &s->step(); }
    s->bg_capture(false);
    (void)fv;

    const size_t cellsA = s->bg_cell_count(0), cellsB = s->bg_cell_count(1);
    std::printf("[..] celdas acumuladas: A=%zu B=%zu\n", cellsA, cellsB);
    check(cellsA > 0 && cellsB > 0, "la captura acumuló celdas en A y B");
    // La pantalla es 40×28 celdas: una tira útil supera con holgura una pantalla.
    check(cellsA > 40 * 28, "el plano A acumuló más que una pantalla (stitch real)");

    // Export por capa.
    auto ex = s->export_backgrounds(out_dir);
    check(bool(ex), "export_backgrounds escribe sin error");
    if (!ex) { std::printf("     error: %s\n", ex.error.message.c_str()); return 1; }
    std::printf("[..] PNGs escritos: %zu\n", ex->size());
    for (const auto& p : *ex) std::printf("     %s\n", p.c_str());
    check(ex->size() >= 2, "exportó al menos los planos A y B");

    // Releer el primer PNG (plano A): dimensiones anchas + contenido opaco real.
    bool png_ok = false, wide = false, opaque = false;
    if (!ex->empty()) {
        int w = 0, h = 0, comp = 0;
        unsigned char* px = stbi_load((*ex)[0].c_str(), &w, &h, &comp, 4);
        png_ok = px && w > 0 && h > 0;
        if (png_ok) {
            wide = w > 320;   // la tira supera el ancho de una pantalla H40
            size_t opaques = 0;
            const size_t n = size_t(w) * size_t(h);
            for (size_t i = 0; i < n; ++i) opaques += (px[i * 4 + 3] == 255);
            opaque = opaques > n / 10;   // contenido real, no un PNG vacío
            std::printf("[..] PNG A: %dx%d px, %zu%% opaco\n",
                        w, h, opaques * 100 / n);
        }
        stbi_image_free(px);
    }
    check(png_ok, "el PNG exportado se relee (stb_image)");
    check(wide,   "la tira exportada es más ancha que una pantalla (nivel, no frame)");
    check(opaque, "el PNG tiene contenido decodificado (píxeles opacos de CRAM)");

    // El nombre lleva el índice de re-import: bg_A_t<ox>x<oy>_<hash>.png
    check(!ex->empty() && (*ex)[0].find("bg_A_t") != std::string::npos,
          "el nombre codifica el índice (plano + origen + hash)");

    std::printf("\n%s (%d fallos)\n", fail ? "FALLÓ" : "OK", fail);
    return fail ? 1 : 0;
}
