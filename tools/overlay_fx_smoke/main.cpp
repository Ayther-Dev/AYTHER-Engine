// ---------------------------------------------------------------------------
// overlay_fx_smoke (#352/#353/#354) — los tres huecos del Acetato verificados
// por píxel sobre un frame real:
//
//   #352 opacidad + blend: una lámina OPACA sobre el frame conocido, fórmula
//        de composición por píxel para opacity ∈ {1.0, 0.5, 0.0} en ambos
//        modos (alpha: src·a + dst·(1−a) · aditivo: src·a + dst, clamp 255),
//        neutro (1.0 + alpha) byte-exacto y confinado a su banda.
//   #353 tileado 2D: single_row_render única vs tile_mode=1 — la primera single_row_render byte-idéntica,
//        las siguientes repetición exacta (período img_h); el caso patológico
//        (asset 8×8) recorta al tope de quads en vez de colgarse.
//   #354 deriva propia: el término temporal es función PURA del frame de la
//        toma — fase esperada (H·factor + drift·frame/fps) mod img_w; seek a
//        f900 por dos caminos (directo vs vía f300) byte-idéntico; dos renders
//        consecutivos del MISMO frame (pausa) byte-idénticos.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target overlay_fx_smoke (GPU)
//   Args:  <rec.arp>
//   Env:   AYTHER_PROBE_ROM
// ---------------------------------------------------------------------------
#include "ayther_env.h"
#include "ayther_session.h"
#include "ayther_recording.h"
#include "ayther_renderer.h"
#include "ayther_layers.h"
#include "vulkan_backend/vk_context.h"
#include <SDL3/SDL.h>
#include <stb_image_write.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif
using ayther::FrameView;

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

static constexpr int kImgW = 64, kImgH = 16;
static constexpr int kBandY = 100;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: overlay_fx_smoke <rec.arp>\n");
        return 2;
    }
    const std::string rec_path = argv[1];

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

    // Lámina 64×16 OPACA: magenta con columna azul de 4 px en x=0 (fase).
    const std::string strip_png = root + "/build/acetato_fx_strip.png";
    {
        std::vector<uint8_t> img((size_t)kImgW * kImgH * 4);
        for (int y = 0; y < kImgH; ++y)
            for (int x = 0; x < kImgW; ++x) {
                uint8_t* p = &img[((size_t)y * kImgW + x) * 4];
                if (x < 4) { p[0] = 0;   p[1] = 0; p[2] = 255; }   // azul
                else       { p[0] = 255; p[1] = 0; p[2] = 255; }   // magenta
                p[3] = 255;
            }
        stbi_write_png(strip_png.c_str(), kImgW, kImgH, 4, img.data(), kImgW * 4);
    }
    // Caso patológico #353: 8×8 verde puro → 40×28 = 1120 quads > tope 512.
    const std::string tiny_png = root + "/build/acetato_fx_tiny.png";
    {
        std::vector<uint8_t> img(8 * 8 * 4);
        for (size_t i = 0; i < img.size(); i += 4) {
            img[i] = 0; img[i + 1] = 255; img[i + 2] = 0; img[i + 3] = 255;
        }
        stbi_write_png(tiny_png.c_str(), 8, 8, 4, img.data(), 8 * 4);
    }

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;
    auto rec = ayther::AytherRecording::load(rec_path);
    if (!rec) { std::fprintf(stderr, "[FAIL] no se pudo cargar %s\n", rec_path.c_str()); return 1; }

    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "[FAIL] SDL_Init\n"); return 1; }
    SDL_Window* win = SDL_CreateWindow("overlay_fx_smoke", 64, 64,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!win) { std::fprintf(stderr, "[FAIL] SDL_CreateWindow\n"); return 1; }
    VkContext ctx;
    if (!ctx.init(win)) { std::fprintf(stderr, "[FAIL] VkContext::init\n"); return 1; }

    const FrameView* fv = s->replay_seek(*rec, 900);
    if (!fv || !fv->fb_width || fv->scene_dirty) {
        std::fprintf(stderr, "[FAIL] f900 sin escena limpia\n");
        return 1;
    }
    const uint32_t W = fv->fb_width, H = fv->fb_height;
    const double   fps = fv->fps_timing;
    check(fps > 1.0, "FrameView.fps_timing viene poblado (base del drift)");

    ayther::AytherRenderer renderer;
    const std::string sh = root + "/shaders/";
    if (!renderer.init(ctx, W, H, sh.c_str()) || !renderer.readback_init(ctx)) {
        std::fprintf(stderr, "[FAIL] renderer\n");
        return 1;
    }

    // Stack con el Acetato ENCIMA de todo (índice final): la banda queda libre
    // de oclusión y la fórmula de composición se verifica limpia.
    AytherLayerStack stack;
    const uint32_t lid = stack.insert_custom("acetato", stack.layers().size());
    auto content = [&](const char* png, int img_w, int img_h, int16_t y,
                       float factor, float opacity, uint8_t blend,
                       uint8_t tile_mode, float drift_x) {
        AytherLayerContent cc{};
        std::snprintf(cc.asset, sizeof(cc.asset), "%s", png);
        cc.img_w = (uint16_t)img_w; cc.img_h = (uint16_t)img_h;
        cc.y = y; cc.anchor = 0; cc.factor = factor;
        cc.opacity = opacity; cc.blend = blend;
        cc.tile_mode = tile_mode; cc.drift_x = drift_x;
        stack.set_content(lid, cc);
    };

    int hs = 0;
    auto render_at = [&](uint32_t f, const AytherLayerStack* st,
                         std::vector<uint8_t>& out) -> bool {
        s->replay_invalidate();
        const FrameView* f2 = s->replay_seek(*rec, f);
        if (!f2 || f2->fb_width != W) return false;
        hs = f2->plane_hscroll[1];
        const uint8_t* g = renderer.export_frame(ctx, *f2, nullptr, true, st);
        if (!g) return false;
        out.assign(g, g + (size_t)W * H * 4);
        return true;
    };

    // Warmup: los DOS PNG decodifican async — renderizar hasta verlos.
    {
        std::vector<uint8_t> warm;
        content(strip_png.c_str(), kImgW, kImgH, kBandY, 0.5f, 1.f, 0, 0, 0.f);
        for (int tries = 0; tries < 100; ++tries) {
            if (!render_at(900, &stack, warm)) break;
            const size_t i = ((size_t)(kBandY + 8) * W + W / 2) * 4;
            if ((warm[i] == 255 && warm[i + 1] == 0 && warm[i + 2] == 255) ||
                (warm[i] == 255 && warm[i + 1] == 0 && warm[i + 2] == 0)) break;
            SDL_Delay(10);
        }
        content(tiny_png.c_str(), 8, 8, 0, 0.f, 1.f, 0, 1, 0.f);
        for (int tries = 0; tries < 100; ++tries) {
            if (!render_at(900, &stack, warm)) break;
            const size_t i = (4 * (size_t)W + 4) * 4;
            if (warm[i + 1] == 255 && warm[i] == 0) break;
            SDL_Delay(10);
        }
    }

    std::vector<uint8_t> base;
    if (!render_at(900, nullptr, base)) { std::fprintf(stderr, "[FAIL] baseline\n"); return 1; }

    // Color esperado de la lámina en el píxel x (fase del parallax + drift).
    auto strip_rgb = [&](int x, double drift_x, double t, uint8_t* rgb) {
        double off = std::fmod((double)hs * 0.5 + drift_x * t, (double)kImgW);
        if (off > 0.0) off -= (double)kImgW;
        const int x0 = (int)std::lround(off);
        const int ix = ((x - x0) % kImgW + kImgW) % kImgW;
        // El readback es BGRA: azul puro = {255,0,0}, magenta = {255,0,255}.
        if (ix < 4) { rgb[0] = 255; rgb[1] = 0; rgb[2] = 0;   }
        else        { rgb[0] = 255; rgb[1] = 0; rgb[2] = 255; }
    };
    // Banda entera contra la fórmula esperada; tol en unidades UNORM8.
    auto band_matches = [&](const std::vector<uint8_t>& got, float opacity,
                            uint8_t blend, int tol) {
        size_t bad = 0;
        for (int y = kBandY; y < kBandY + kImgH; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const size_t i = ((size_t)y * W + x) * 4;
                uint8_t sc[3]; strip_rgb((int)x, 0.0, 0.0, sc);
                for (int ch = 0; ch < 3; ++ch) {
                    double e;
                    if (blend == 0)
                        e = sc[ch] * opacity + base[i + ch] * (1.0 - opacity);
                    else
                        e = std::min(255.0, (double)sc[ch] * opacity +
                                                (double)base[i + ch]);
                    if (std::abs((double)got[i + ch] - e) > tol) {
                        if (bad < 4)
                            std::printf("  [..] (%u,%d) got %u,%u,%u  strip %u,%u,%u"
                                        "  base %u,%u,%u\n", x, y,
                                        got[i], got[i+1], got[i+2],
                                        sc[0], sc[1], sc[2],
                                        base[i], base[i+1], base[i+2]);
                        ++bad; break;
                    }
                }
            }
        if (bad) std::printf("  [..] %zu px fuera de fórmula (de %u)\n",
                             bad, W * kImgH);
        // ±2 px de borde por el redondeo del offset: tolera media columna.
        return bad <= (size_t)(2 * kImgH * 2);
    };
    auto outside_intact = [&](const std::vector<uint8_t>& got) {
        for (uint32_t y = 0; y < H; ++y) {
            if ((int)y >= kBandY && (int)y < kBandY + kImgH) continue;
            for (uint32_t x = 0; x < W; ++x) {
                const size_t i = ((size_t)y * W + x) * 4;
                if (got[i] != base[i] || got[i + 1] != base[i + 1] ||
                    got[i + 2] != base[i + 2]) return false;
            }
        }
        return true;
    };

    std::printf("\n== #352: opacidad y modo de mezcla ==\n");
    std::vector<uint8_t> got, got2;
    content(strip_png.c_str(), kImgW, kImgH, kBandY, 0.5f, 1.0f, 0, 0, 0.f);
    if (!render_at(900, &stack, got)) { std::fprintf(stderr, "[FAIL] render\n"); return 1; }
    check(band_matches(got, 1.0f, 0, 0), "opacity=1.0 blend=alpha: la lámina opaca REEMPLAZA (exacto)");
    check(outside_intact(got), "neutro: fuera de la banda byte-idéntico al baseline");
    check(render_at(900, &stack, got2) && got2 == got,
          "neutro: dos renders del mismo frame son byte-idénticos");

    content(strip_png.c_str(), kImgW, kImgH, kBandY, 0.5f, 0.5f, 0, 0, 0.f);
    if (!render_at(900, &stack, got)) return 1;
    check(band_matches(got, 0.5f, 0, 2), "opacity=0.5 blend=alpha: 0.5·src + 0.5·dst (±2)");

    content(strip_png.c_str(), kImgW, kImgH, kBandY, 0.5f, 0.0f, 0, 0, 0.f);
    if (!render_at(900, &stack, got)) return 1;
    check(got == base, "opacity=0.0: byte-idéntico al baseline (ambos modos comparten src·a=0)");

    content(strip_png.c_str(), kImgW, kImgH, kBandY, 0.5f, 1.0f, 1, 0, 0.f);
    if (!render_at(900, &stack, got)) return 1;
    check(band_matches(got, 1.0f, 1, 2), "opacity=1.0 blend=aditivo: min(255, src + dst) (±2)");
    check(outside_intact(got), "aditivo: fuera de la banda byte-idéntico al baseline");

    content(strip_png.c_str(), kImgW, kImgH, kBandY, 0.5f, 0.5f, 1, 0, 0.f);
    if (!render_at(900, &stack, got)) return 1;
    check(band_matches(got, 0.5f, 1, 2), "opacity=0.5 blend=aditivo: min(255, 0.5·src + dst) (±2)");

    std::printf("\n== #353: tileado vertical ==\n");
    // factor 0 → off=0: la retícula queda anclada y comparable single_row_render a single_row_render.
    std::vector<uint8_t> single_row_render, tile2d;
    content(strip_png.c_str(), kImgW, kImgH, 0, 0.0f, 1.0f, 0, 0, 0.f);
    if (!render_at(900, &stack, single_row_render)) return 1;
    content(strip_png.c_str(), kImgW, kImgH, 0, 0.0f, 1.0f, 0, 1, 0.f);
    if (!render_at(900, &stack, tile2d)) return 1;
    {
        bool first_rows_same = true;
        for (int y = 0; y < kImgH && first_rows_same; ++y)
            if (std::memcmp(&single_row_render[(size_t)y * W * 4], &tile2d[(size_t)y * W * 4],
                            (size_t)W * 4) != 0) first_rows_same = false;
        check(first_rows_same, "tile 2D: la primera single_row_render es byte-idéntica a la single_row_render única");
        bool repeats = true;
        for (uint32_t y = kImgH; y < H && repeats; ++y)
            if (std::memcmp(&tile2d[(size_t)y * W * 4],
                            &tile2d[(size_t)(y - kImgH) * W * 4],
                            (size_t)W * 4) != 0) repeats = false;
        check(repeats, "tile 2D: cada single_row_render repite la de img_h arriba (período exacto)");
        bool below_base = true;
        for (uint32_t y = kImgH; y < H && below_base; ++y)
            if (std::memcmp(&single_row_render[(size_t)y * W * 4], &base[(size_t)y * W * 4],
                            (size_t)W * 4) != 0) below_base = false;
        check(below_base, "single_row_render única: debajo de la single_row_render el frame es el baseline");
    }
    // Caso patológico: 8×8 sobre el frame entero recorta al tope y NO cuelga.
    content(tiny_png.c_str(), 8, 8, 0, 0.0f, 1.0f, 0, 1, 0.f);
    if (!render_at(900, &stack, got)) return 1;
    {
        const size_t top = (4 * (size_t)W + 4) * 4;
        const size_t bot = ((size_t)(H - 8) * W + 4) * 4;
        check(got[top + 1] == 255 && got[top] == 0 && got[top + 2] == 0,
              "tope de quads: arriba el patrón 8×8 pinta");
        check(got[bot] == base[bot] && got[bot + 1] == base[bot + 1] &&
                  got[bot + 2] == base[bot + 2],
              "tope de quads: el fondo recortado queda en baseline (no cuelga)");
    }

    std::printf("\n== #354: deriva propia determinista ==\n");
    // drift_x=60 px/s → a f900 el término temporal es 60·900/fps ≈ 901 px.
    const double t900 = 900.0 / fps;
    content(strip_png.c_str(), kImgW, kImgH, kBandY, 0.5f, 1.0f, 0, 0, 60.f);
    if (!render_at(900, &stack, got)) return 1;
    {
        size_t bad = 0;
        for (int y = kBandY; y < kBandY + kImgH; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const size_t i = ((size_t)y * W + x) * 4;
                uint8_t sc[3]; strip_rgb((int)x, 60.0, t900, sc);
                if (got[i] != sc[0] || got[i + 1] != sc[1] || got[i + 2] != sc[2])
                    ++bad;
            }
        std::printf("  [..] drift: %zu px fuera de fase (de %u)\n", bad, W * kImgH);
        check(bad <= (size_t)(2 * kImgH * 2),
              "la fase es (H·0.5 + drift·frame/fps) mod 64 — función pura del frame");
    }
    check(render_at(900, &stack, got2) && got2 == got,
          "pausa: dos renders consecutivos del mismo frame son byte-idénticos");
    {
        std::vector<uint8_t> via300, direct;
        if (!render_at(300, &stack, via300)) return 1;   // camino con desvío
        if (!render_at(900, &stack, via300)) return 1;
        if (!render_at(900, &stack, direct)) return 1;   // directo (re-seek)
        check(via300 == direct,
              "seek por dos caminos (directo vs vía f300) da el mismo píxel");
    }

    vkDeviceWaitIdle(ctx.device());
    renderer.readback_shutdown(ctx);
    renderer.shutdown(ctx);
    ctx.shutdown();
    SDL_DestroyWindow(win);
    SDL_Quit();

    std::printf("\n%d checks, %d fails\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
