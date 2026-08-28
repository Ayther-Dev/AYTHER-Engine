// ---------------------------------------------------------------------------
// scene_inventory_smoke (#272) — el inventario de escena REPRODUCE la escena.
//
// Por cada frame de una toma: se pide el inventario (scene_inventory — la
// lista única ordenada de celdas de plano + sprites) y se COMPONE la pantalla
// en CPU sólo desde sus elementos, con la semántica del VDP:
//
//   1. sprites → buffer primero-gana en orden GLOBAL de cadena (menor chain =
//      más al frente; la prioridad NO participa en el duelo sprite-vs-sprite,
//      sólo decide en qué nivel entra el pixel ganador),
//   2. compose back→front: backdrop → B pri0 → A pri0 → W pri0 → sprite px
//      pri0 → B pri1 → A pri1 → W pri1 → sprite px pri1 (índice 0 = deja ver),
//   3. left-column blanking si reg0.5 (igual que el renderer).
//
// El resultado se compara BIT A BIT (RGB565, misma paleta del core) contra
// ayther_recompose_frame(base) — el oráculo de R-1: el mismo renderer del
// emulador recomponiendo desde el mismo estado. Si el inventario cubre todo
// lo dibujable, la diferencia es CERO.
//
// Frames de GA gameplay (sin S/H, sin window, hscroll full, sin overflow de
// sprites — todo medido en R-1): lo que el inventario modela hoy. El raster
// por línea y el límite de sprites quedan anotados en la épica.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target scene_inventory_smoke
//   Args:  <rec.arp> [frame ...]   (default: 300 900 1500)
//   Env:   AYTHER_PROBE_ROM
// ---------------------------------------------------------------------------
#include "ayther_env.h"
#include "ayther_session.h"
#include "ayther_recording.h"
#include <stb_image_write.h>

#include <algorithm>
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
using ayther::FrameView;
using SceneElement = ayther::AytherSession::SceneElement;

using RecomposeFn = int (*)(uint16_t*, int, unsigned, int*, int*);

static std::string toml_quoted(const std::string& l) {
    const auto a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string() : l.substr(a + 1, b - a - 1);
}
static std::string resolve(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\') return p;
    return base + "/" + p;
}

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

// Paleta 565 EXACTA del core: canal de 3 bits → nivel normal ×2 (0..14) →
// RGB565 con la expansión de MAKE_PIXEL (la misma tabla que pixel_lut[1]).
static uint16_t genesis565(uint16_t packed) {
    const unsigned r4 = ((packed >> 0) & 7) << 1;
    const unsigned g4 = ((packed >> 3) & 7) << 1;
    const unsigned b4 = ((packed >> 6) & 7) << 1;
    const unsigned r5 = (r4 << 1) | (r4 >> 3);
    const unsigned g6 = (g4 << 2) | (g4 >> 2);
    const unsigned b5 = (b4 << 1) | (b4 >> 3);
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

// Índice de color 0..15 de un pixel de tile (vista de bus: off^1).
static uint8_t tile_px(const uint8_t* vram, size_t vsz,
                       uint32_t pattern, int px, int py) {
    const uint32_t off = (pattern * 32u + (uint32_t)py * 4u + (uint32_t)(px >> 1)) ^ 1u;
    const uint8_t  b   = off < vsz ? vram[off] : 0;
    return (px & 1) ? (b & 0x0F) : (b >> 4);
}

static void dump_png(const char* name, const std::vector<uint16_t>& buf, int w, int h) {
    std::vector<uint8_t> png((size_t)w * h * 3);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        const uint16_t p = buf[i];
        const unsigned r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
        png[i * 3 + 0] = (uint8_t)((r << 3) | (r >> 2));
        png[i * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
        png[i * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
    }
    stbi_write_png(name, w, h, 3, png.data(), w * 3);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: scene_inventory_smoke <rec.arp> [frame ...]\n");
        return 2;
    }
    const std::string rec_path = argv[1];
    std::vector<uint32_t> frames;
    for (int i = 2; i < argc; ++i)
        frames.push_back((uint32_t)std::strtoul(argv[i], nullptr, 10));
    if (frames.empty()) frames = { 300, 900, 1500 };

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
    auto rc = reinterpret_cast<RecomposeFn>(s->core_export("ayther_recompose_frame"));
    if (!rc) { std::fprintf(stderr, "[FAIL] el core no exporta ayther_recompose_frame\n"); return 1; }
    auto rec = ayther::AytherRecording::load(rec_path);
    if (!rec) { std::fprintf(stderr, "[FAIL] no se pudo cargar %s\n", rec_path.c_str()); return 1; }

    std::printf("=== scene_inventory_smoke (#272) ===\n\n");
    std::vector<uint16_t> expected(320 * 240), got(320 * 240);
    std::vector<SceneElement> inv;

    for (uint32_t f : frames) {
        const FrameView* fv = s->replay_seek(*rec, f);
        if (!fv || !fv->fb_width) { std::printf("f%u:\n", f); check(false, "seek"); continue; }
        const int W = (int)fv->fb_width, H = (int)fv->fb_height;
        size_t rsz = 0, vsz = 0, csz = 0;
        const uint8_t* regs = s->vdp_regs(&rsz);
        const uint8_t* vram = s->video_ram(&vsz);
        const uint8_t* cram = s->color_ram(&csz);
        if (!regs || rsz < 0x20 || !vram || !cram) {
            std::printf("f%u:\n", f);
            check(false, "VRAM/CRAM/regs del fork");
            continue;
        }

        // Oráculo: el renderer del core desde el mismo estado.
        int rw = 0, rh = 0;
        if (!rc(expected.data(), (int)expected.size(), 0, &rw, &rh) ||
            rw != W || rh != H) {
            std::printf("f%u:\n", f);
            check(false, "recompose(base) del core");
            continue;
        }

        const size_t n = s->scene_inventory(inv);
        uint32_t nB = 0, nA = 0, nW = 0, nS = 0;
        for (const SceneElement& e : inv)
            (e.layer == 0 ? nB : e.layer == 1 ? nA : e.layer == 2 ? nW : nS)++;
        std::printf("f%u: inventario %zu elementos (B=%u A=%u W=%u S=%u)\n",
                    f, n, nB, nA, nW, nS);

        // Paleta 565 + backdrop.
        uint16_t pal565[64];
        for (int i = 0; i < 64; ++i)
            pal565[i] = genesis565((uint16_t)(cram[i * 2] | (cram[i * 2 + 1] << 8)));
        const uint16_t backdrop = pal565[regs[7] & 0x3F];

        // 1) Sprites → buffer primero-gana por cadena GLOBAL (frente primero).
        std::vector<const SceneElement*> sprites;
        for (const SceneElement& e : inv) if (e.layer == 3) sprites.push_back(&e);
        std::stable_sort(sprites.begin(), sprites.end(),
                         [](const SceneElement* a, const SceneElement* b) {
                             return a->chain < b->chain;
                         });
        std::vector<uint8_t> spr_idx((size_t)W * H, 0), spr_pri((size_t)W * H, 0);
        for (const SceneElement* e : sprites) {
            const int wt = e->w / 8, ht = e->h / 8;
            for (int py = 0; py < e->h; ++py) {
                const int sy = e->y + py;
                if (sy < 0 || sy >= H) continue;
                const int ly = (e->flips & 2) ? e->h - 1 - py : py;
                for (int px = 0; px < e->w; ++px) {
                    const int sx = e->x + px;
                    if (sx < 0 || sx >= W) continue;
                    const size_t at = (size_t)sy * W + sx;
                    if (spr_idx[at]) continue;   // primero-gana (menor chain)
                    const int lx = (e->flips & 1) ? e->w - 1 - px : px;
                    // Tiles de sprite en orden COLUMN-MAJOR: base + col*ht + fila.
                    const uint32_t tile = (uint32_t)(e->pattern & 0x7FF)
                                        + (uint32_t)(lx / 8) * ht + (uint32_t)(ly / 8);
                    const uint8_t idx = tile_px(vram, vsz, tile & 0x7FF, lx & 7, ly & 7);
                    if (!idx) continue;
                    spr_idx[at] = (uint8_t)((e->palette << 4) | idx);
                    spr_pri[at] = e->priority;
                }
            }
            (void)wt;
        }

        // 2) Compose back→front desde el inventario (el ORDEN del vector).
        std::fill(got.begin(), got.begin() + (size_t)W * H, backdrop);
        auto paint_cell = [&](const SceneElement& e) {
            for (int py = 0; py < 8; ++py) {
                const int sy = e.y + py;
                if (sy < 0 || sy >= H) continue;
                const int ly = (e.flips & 2) ? 7 - py : py;
                for (int px = 0; px < 8; ++px) {
                    const int sx = e.x + px;
                    if (sx < 0 || sx >= W) continue;
                    const int lx = (e.flips & 1) ? 7 - px : px;
                    const uint8_t idx = tile_px(vram, vsz, e.pattern, lx, ly);
                    if (idx)
                        got[(size_t)sy * W + sx] = pal565[(e.palette << 4) | idx];
                }
            }
        };
        auto paint_sprites = [&](uint8_t pri) {
            for (size_t i = 0; i < (size_t)W * H; ++i)
                if (spr_idx[i] && spr_pri[i] == pri) got[i] = pal565[spr_idx[i]];
        };
        uint8_t last_pri = 0;
        for (const SceneElement& e : inv) {
            if (e.layer == 3) continue;              // los pinta paint_sprites
            if (e.priority != last_pri) {            // cambio de grupo: sprites pri0
                paint_sprites(0);
                last_pri = e.priority;
            }
            paint_cell(e);
        }
        if (last_pri == 0) paint_sprites(0);         // frame sin celdas pri1
        paint_sprites(1);

        // 3) Left-column blanking (reg 0 bit 5) — igual que el renderer.
        if (regs[0] & 0x20)
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < 8; ++x) got[(size_t)y * W + x] = backdrop;

        // Comparación bit a bit.
        size_t mismatch = 0;
        for (size_t i = 0; i < (size_t)W * H; ++i) mismatch += (got[i] != expected[i]);
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "compose desde el inventario == recompose del core "
                      "(%zu px distintos de %d)", mismatch, W * H);
        check(mismatch == 0, msg);
        if (mismatch) {
            int shown = 0;
            for (int y = 0; y < H && shown < 10; ++y)
                for (int x = 0; x < W && shown < 10; ++x) {
                    const size_t i = (size_t)y * W + x;
                    if (got[i] != expected[i]) {
                        std::printf("        diff (%3d,%3d): got=%04X want=%04X spr=%02X/%u\n",
                                    x, y, got[i], expected[i], spr_idx[i], spr_pri[i]);
                        ++shown;
                    }
                }
            char name[64];
            std::snprintf(name, sizeof(name), "scene_inv_f%u_got.png", f);
            dump_png(name, got, W, H);
            std::snprintf(name, sizeof(name), "scene_inv_f%u_want.png", f);
            dump_png(name, expected, W, H);
            std::printf("        (dump: scene_inv_f%u_{got,want}.png)\n", f);
        }
    }

    std::printf("\n%d checks, %d fails\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
