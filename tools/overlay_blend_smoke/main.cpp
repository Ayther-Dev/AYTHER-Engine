// ---------------------------------------------------------------------------
// overlay_blend_smoke (#485/#486/#488) — los SIETE modos de mezcla del
// Acetato EJECUTADOS en la GPU real y comparados por píxel contra su
// especificación.
//
// POR QUÉ EXISTE. El papel viejo de la épica #487 pide multiplicar (manchas
// que OSCURECEN) y las luces piden pantalla (aclarar proporcional, sin la
// saturación del aditivo); #488 completa el kit clásico. Las fórmulas son
// triviales y por eso se fijan acá, texel a texel:
//   blend=0  out = mix(d, c, f)                       (alpha clásico)
//   blend=1  out = d + c·f                             (aditivo #352)
//   blend=2  out = d · mix(1, c, f)                    (multiplicar #485)
//   blend=3  out = d + c·f·(1−d)                       (pantalla #486)
//   blend=4  out = d − c·f                             (sustractivo #488)
//   blend=5  out = min(d, mix(1, c, f))                (oscurecer #488)
//   blend=6  out = max(d, c·f)                         (aclarar #488)
// con f = alpha del PNG × opacity, d = lo que hay debajo, y clamp a [0,1].
//
// NO NECESITA ROM NI GRABACIÓN (molde: overlay_vscroll_smoke): DOS capas Custom —
// una BASE opaca de color conocido (blend 0, el «debajo» exacto) y encima la
// lámina de test con TRES bandas de alpha (0 · 128 · 255). factor=0 en ambas
// (lámina fija a la pantalla — de paso fija el truco del marco de #487).
// Canvas 320×240 = escala 1:1 → NEAREST → comparación exacta (±1 UNORM).
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target overlay_blend_smoke (requiere GPU)
//   Args:  ninguno
// ---------------------------------------------------------------------------
#include "ayther_layers.h"
#include "ayther_renderer.h"
#include "ayther_session.h"
#include "vulkan_backend/vk_context.h"
#include <SDL3/SDL.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif
using ayther::FrameView;

namespace {

int g_checks = 0, g_fails = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

constexpr uint32_t kW = 320, kH = 240;
// Base y test con canales bien separados: multiplicar y pantalla tienen que
// moverse en los tres canales sin saturar de entrada.
constexpr uint8_t kBase[3] = { 120,  80, 200 };   // R,G,B del «debajo»
constexpr uint8_t kTest[3] = { 200,  60,  40 };
constexpr uint8_t kAlphaBand[3] = { 0, 128, 255 };   // tres bandas verticales

// La referencia CPU de las cuatro fórmulas, en el MISMO espacio que la GPU
// (UNORM 8 bits → float, clamp, redondeo al escribir).
void expect_px(uint8_t blend, uint8_t band_alpha, float opacity, uint8_t out[3]) {
    const double f = (band_alpha / 255.0) * opacity;
    for (int i = 0; i < 3; ++i) {
        const double d = kBase[i] / 255.0, c = kTest[i] / 255.0;
        double o = 0.0;
        switch (blend) {
            case 0: o = d + (c - d) * f;       break;   // mix(d, c, f)
            case 1: o = d + c * f;             break;   // aditivo
            case 2: o = d * (1.0 + (c - 1.0) * f); break;   // d · mix(1, c, f)
            case 3: o = d + c * f * (1.0 - d); break;   // pantalla
            case 4: o = d - c * f;             break;   // sustractivo (#488)
            case 5: o = std::min(d, 1.0 + (c - 1.0) * f); break;   // oscurecer
            case 6: o = std::max(d, c * f);    break;   // aclarar
        }
        o = std::clamp(o, 0.0, 1.0);
        out[i] = (uint8_t)std::lround(o * 255.0);
    }
}

bool near3(const uint8_t* bgra, const uint8_t rgb[3], int tol = 1) {
    return std::abs((int)bgra[2] - (int)rgb[0]) <= tol &&
           std::abs((int)bgra[1] - (int)rgb[1]) <= tol &&
           std::abs((int)bgra[0] - (int)rgb[2]) <= tol;
}

}  // namespace

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "[FAIL] SDL_Init\n"); return 1; }
    SDL_Window* win = SDL_CreateWindow("overlay_blend_smoke", 64, 64,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!win) { std::fprintf(stderr, "[FAIL] SDL_CreateWindow\n"); return 1; }
    VkContext ctx;
    if (!ctx.init(win)) { std::fprintf(stderr, "[FAIL] VkContext::init\n"); return 1; }

    ayther::AytherRenderer renderer;
    const std::string sh = std::string(AYTHER_SOURCE_DIR) + "/shaders/";
    if (!renderer.init(ctx, kW, kH, sh.c_str()) || !renderer.readback_init(ctx)) {
        std::fprintf(stderr, "[FAIL] renderer (shaders en %s)\n", sh.c_str());
        return 1;
    }

    std::printf("=== overlay_blend_smoke (#485/#486) — 4 modos contra su formula ===\n\n");

    // -- Las dos láminas sintéticas, al build dir.
    const std::string root = AYTHER_SOURCE_DIR;
    const std::string base_png = root + "/build/acetato_blend_base.png";
    const std::string test_png = root + "/build/acetato_blend_test.png";
    {
        std::vector<uint8_t> img((size_t)kW * kH * 4);
        for (size_t i = 0; i < img.size(); i += 4) {
            img[i] = kBase[0]; img[i + 1] = kBase[1]; img[i + 2] = kBase[2];
            img[i + 3] = 255;
        }
        if (!stbi_write_png(base_png.c_str(), kW, kH, 4, img.data(), kW * 4)) {
            std::fprintf(stderr, "[FAIL] no se pudo escribir %s\n", base_png.c_str());
            return 1;
        }
        // Test: color pleno, alpha en 3 bandas verticales de ~107 px.
        for (uint32_t y = 0; y < kH; ++y)
            for (uint32_t x = 0; x < kW; ++x) {
                uint8_t* p = &img[((size_t)y * kW + x) * 4];
                p[0] = kTest[0]; p[1] = kTest[1]; p[2] = kTest[2];
                p[3] = kAlphaBand[x < 107 ? 0 : x < 214 ? 1 : 2];
            }
        if (!stbi_write_png(test_png.c_str(), kW, kH, 4, img.data(), kW * 4)) {
            std::fprintf(stderr, "[FAIL] no se pudo escribir %s\n", test_png.c_str());
            return 1;
        }
    }

    // -- Stack: base (abajo) + test (arriba), las dos Custom, factor=0 = fijas.
    AytherLayerStack stack;
    const uint32_t base_id = stack.insert_custom("base",  stack.layers().size());
    const uint32_t test_id = stack.insert_custom("test",  stack.layers().size());
    for (const AytherLayer& l : stack.layers())
        stack.set_visible(l.id, l.id == base_id || l.id == test_id);
    auto set_layer = [&](uint32_t id, const std::string& png, uint8_t blend,
                         float opacity) {
        AytherLayerContent cc{};
        std::snprintf(cc.asset, sizeof(cc.asset), "%s", png.c_str());
        cc.img_w = (uint16_t)kW; cc.img_h = (uint16_t)kH;
        cc.y = 0; cc.anchor = 0; cc.factor = 0.0f;   // #487: fija a la pantalla
        cc.tile_mode = 0; cc.opacity = opacity; cc.blend = blend;
        stack.set_content(id, cc);
    };
    set_layer(base_id, base_png, 0, 1.0f);

    FrameView fv{};
    fv.fps_timing = 60.0;

    const uint8_t* got = nullptr;
    auto px = [&](int x, int y) { return got + ((size_t)y * kW + x) * 4; };
    const int bx[3] = { 50, 160, 270 };   // un punto por banda de alpha

    // -- Espera del decode async (gpu-oracle-texstate-ready): listo cuando la
    //    banda a=255 con blend 0 REEMPLAZA y la banda a=0 muestra la base.
    set_layer(test_id, test_png, 0, 1.0f);
    uint8_t want[3];
    bool ready = false;
    for (int it = 0; it < 240 && !ready; ++it) {
        got = renderer.export_frame(ctx, fv, nullptr, /*hd_on=*/true, &stack);
        if (!got) break;
        uint8_t w0[3], w2[3];
        expect_px(0, 0, 1.0f, w0); expect_px(0, 255, 1.0f, w2);
        ready = near3(px(bx[0], 120), w0) && near3(px(bx[2], 120), w2);
    }
    check(got != nullptr, "el readback devuelve pixeles");
    if (!got) return 1;
    check(ready, "las dos laminas cargaron (a=0 muestra la base, a=1 reemplaza)");

    // -- Los 7 modos × 3 bandas, contra la fórmula.
    static const char* kNames[7] = { "alpha", "aditivo", "multiplicar",
                                     "pantalla", "sustractivo", "oscurecer",
                                     "aclarar" };
    for (uint8_t blend = 0; blend < 7; ++blend) {
        set_layer(test_id, test_png, blend, 1.0f);
        got = renderer.export_frame(ctx, fv, nullptr, true, &stack);
        if (!got) return 1;
        bool ok = true;
        for (int b = 0; b < 3 && ok; ++b) {
            expect_px(blend, kAlphaBand[b], 1.0f, want);
            ok = near3(px(bx[b], 120), want);
        }
        char msg[96];
        std::snprintf(msg, sizeof(msg),
                      "blend=%u (%s): las 3 bandas de alpha dan la formula",
                      blend, kNames[blend]);
        check(ok, msg);
    }

    // -- La fuerza: opacity=0.5 escala el efecto en multiplicar y pantalla.
    for (uint8_t blend = 2; blend <= 3; ++blend) {
        set_layer(test_id, test_png, blend, 0.5f);
        got = renderer.export_frame(ctx, fv, nullptr, true, &stack);
        if (!got) return 1;
        expect_px(blend, 255, 0.5f, want);
        char msg[96];
        std::snprintf(msg, sizeof(msg),
                      "blend=%u: opacity=0.5 es la FUERZA (a=1 da el mix a medias)",
                      blend);
        check(near3(px(bx[2], 120), want), msg);
    }

    // -- Neutros: blanco no multiplica · negro no aclara (byte-exacto con la base).
    {
        const std::string white_png = root + "/build/acetato_blend_white.png";
        const std::string black_png = root + "/build/acetato_blend_black.png";
        std::vector<uint8_t> img((size_t)kW * kH * 4, 255);
        stbi_write_png(white_png.c_str(), kW, kH, 4, img.data(), kW * 4);
        for (size_t i = 0; i < img.size(); i += 4) { img[i] = img[i+1] = img[i+2] = 0; }
        stbi_write_png(black_png.c_str(), kW, kH, 4, img.data(), kW * 4);

        set_layer(test_id, test_png, 0, 0.0f);   // apagar el test
        uint8_t base_want[3];
        expect_px(0, 0, 1.0f, base_want);

        set_layer(test_id, white_png, 2, 1.0f);
        bool ok = false;
        for (int it = 0; it < 240 && !ok; ++it) {   // decode de la lámina nueva
            got = renderer.export_frame(ctx, fv, nullptr, true, &stack);
            if (!got) return 1;
            ok = near3(px(160, 120), base_want, 0);
        }
        check(ok, "multiplicar: lamina BLANCA = identidad byte-exacta");

        set_layer(test_id, black_png, 3, 1.0f);
        ok = false;
        for (int it = 0; it < 240 && !ok; ++it) {
            got = renderer.export_frame(ctx, fv, nullptr, true, &stack);
            if (!got) return 1;
            ok = near3(px(160, 120), base_want, 0);
        }
        check(ok, "pantalla: lamina NEGRA = identidad byte-exacta");

        // #488: los neutros de los tres nuevos.
        set_layer(test_id, black_png, 4, 1.0f);
        got = renderer.export_frame(ctx, fv, nullptr, true, &stack);
        check(got && near3(px(160, 120), base_want, 0),
              "sustractivo: lamina NEGRA = identidad byte-exacta");
        set_layer(test_id, white_png, 5, 1.0f);
        got = renderer.export_frame(ctx, fv, nullptr, true, &stack);
        check(got && near3(px(160, 120), base_want, 0),
              "oscurecer: lamina BLANCA = identidad byte-exacta");
        set_layer(test_id, black_png, 6, 1.0f);
        got = renderer.export_frame(ctx, fv, nullptr, true, &stack);
        check(got && near3(px(160, 120), base_want, 0),
              "aclarar: lamina NEGRA = identidad byte-exacta");
    }

    // -- #488, el caso CLAVE de min/max: lámina GRIS sobre base mitad clara /
    //    mitad oscura — oscurecer solo toca la mitad clara, aclarar solo la
    //    oscura (multiply/screen tocarían LAS DOS).
    {
        const std::string split_png = root + "/build/acetato_blend_split.png";
        const std::string gray_png  = root + "/build/acetato_blend_gray.png";
        std::vector<uint8_t> img((size_t)kW * kH * 4);
        for (uint32_t y = 0; y < kH; ++y)
            for (uint32_t x = 0; x < kW; ++x) {
                uint8_t* p = &img[((size_t)y * kW + x) * 4];
                const uint8_t v = x < kW / 2 ? 30 : 220;   // oscura | clara
                p[0] = p[1] = p[2] = v; p[3] = 255;
            }
        stbi_write_png(split_png.c_str(), kW, kH, 4, img.data(), kW * 4);
        for (size_t i = 0; i < img.size(); i += 4)
            img[i] = img[i+1] = img[i+2] = 128;
        stbi_write_png(gray_png.c_str(), kW, kH, 4, img.data(), kW * 4);

        set_layer(test_id, test_png, 0, 0.0f);         // apagar el test
        set_layer(base_id, split_png, 0, 1.0f);        // base partida
        // esperar el decode de la base nueva (a=0 en test ⇒ se ve la base)
        bool okb = false;
        uint8_t d30[3] = { 30, 30, 30 }, d220[3] = { 220, 220, 220 };
        for (int it = 0; it < 240 && !okb; ++it) {
            got = renderer.export_frame(ctx, fv, nullptr, true, &stack);
            if (!got) return 1;
            okb = near3(px(80, 120), d30) && near3(px(240, 120), d220);
        }
        check(okb, "base partida cargada (30 | 220)");

        set_layer(test_id, gray_png, 5, 1.0f);         // oscurecer con gris 128
        bool okg = false;
        uint8_t g128[3] = { 128, 128, 128 };
        for (int it = 0; it < 240 && !okg; ++it) {
            got = renderer.export_frame(ctx, fv, nullptr, true, &stack);
            if (!got) return 1;
            okg = near3(px(240, 120), g128);           // decode del gris listo
        }
        check(okg && near3(px(80, 120), d30, 0),
              "oscurecer con gris: la mitad OSCURA queda intacta, la clara baja a 128");
        set_layer(test_id, gray_png, 6, 1.0f);         // aclarar con gris 128
        got = renderer.export_frame(ctx, fv, nullptr, true, &stack);
        check(got && near3(px(80, 120), g128) && near3(px(240, 120), d220, 0),
              "aclarar con gris: la mitad CLARA queda intacta, la oscura sube a 128");

        set_layer(base_id, base_png, 0, 1.0f);         // restaurar la base
        bool okr = false;
        for (int it = 0; it < 240 && !okr; ++it) {
            set_layer(test_id, test_png, 0, 0.0f);
            got = renderer.export_frame(ctx, fv, nullptr, true, &stack);
            if (!got) return 1;
            uint8_t bw[3]; expect_px(0, 0, 1.0f, bw);
            okr = near3(px(160, 120), bw);
        }
        check(okr, "base restaurada");
    }

    // -- Determinismo.
    set_layer(test_id, test_png, 2, 1.0f);
    got = renderer.export_frame(ctx, fv, nullptr, true, &stack);
    if (!got) return 1;
    std::vector<uint8_t> a(got, got + (size_t)kW * kH * 4);
    got = renderer.export_frame(ctx, fv, nullptr, true, &stack);
    check(got && std::memcmp(a.data(), got, a.size()) == 0,
          "determinismo: dos renders byte-identicos");

    renderer.readback_shutdown(ctx);
    renderer.shutdown(ctx);
    ctx.shutdown();
    SDL_DestroyWindow(win);
    SDL_Quit();

    std::printf("\n%d checks, %d fallas\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
