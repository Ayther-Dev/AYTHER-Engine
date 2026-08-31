// ---------------------------------------------------------------------------
// sprite_mask_smoke (Vestuario) — el shader de la máscara de tinte EJECUTADO
// en la GPU real y comparado por píxel contra su especificación.
//
// POR QUÉ EXISTE. El Vestuario define qué zonas del asset HD de una pose
// siguen el tinte de paleta (blanco) y cuáles solo su LUMA (negro — la piel
// acompaña los fundidos pero no cambia de matiz). La fórmula vive en
// sprite_mask.frag:  out = c · mix(vec3(luma(t)), t, m).  Un error acá no
// rompe nada visible en un smoke normal: la ropa se tiñe "parecido" y nadie
// se entera hasta mirar a la cara del enemigo en pleno palette swap. Este
// oráculo la fija por píxel, con tolerancia ±1 por redondeo UNORM (el mismo
// criterio que element_effect_smoke #275).
//
// NO NECESITA ROM NI GRABACIÓN (molde: video_shader_smoke #378): dos PNGs
// sintéticos —asset de color pleno + máscara mitad negra / mitad blanca—, tres
// AytherSpriteSub armados a mano y un stack donde solo SpritesHd es visible.
//
// LO QUE CAE ADENTRO:
//   · La mitad BLANCA = el tinte completo de sprite.frag (mismo resultado que
//     un sub SIN máscara con el mismo tinte — se verifica contra uno al lado).
//   · La mitad NEGRA = solo la luma del tinte (Rec.601, los pesos del
//     peak-hold del motor).
//   · El PRE-FLIP de la máscara: un sub espejado en H con el MISMO asset
//     simétrico invierte solo la máscara — si la carga no espejara, el lado
//     teñido no se daría vuelta.
//   · El fallback: el pipeline de máscara ausente o la máscara ilegible caen
//     al tinte entero (no se cuelga, no queda transparente).
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target sprite_mask_smoke (requiere GPU)
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
#include <filesystem>
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

// Canvas = resolución nativa del emulador (320×240) → escala 1:1, y con el
// asset 64×64 dibujado a 64×64 el sampler por-quad elige NEAREST: cada píxel
// del readback es UN texel, la comparación puede ser exacta (±1 de UNORM).
constexpr uint32_t kW = 320, kH = 240;
constexpr int      kSpr = 64;

// Color pleno del asset (B,G,R) y tinte Q2.6 del sub: R 2.0 (satura), G/B 0.5.
constexpr uint8_t kB = 80, kG = 120, kR = 200;
constexpr uint8_t kTintQ[3] = { 128, 32, 32 };   // R,G,B en Q2.6 (64 = 1.0)

// La referencia CPU de sprite_mask.frag (m = 0 o 1; la máscara del oráculo es
// binaria a propósito — el mix intermedio es aritmética trivial de GPU).
void expect_px(bool masked_white, uint8_t out[3]) {
    const double t[3] = { kTintQ[0] / 64.0, kTintQ[1] / 64.0, kTintQ[2] / 64.0 };
    const double l = 0.299 * t[0] + 0.587 * t[1] + 0.114 * t[2];
    const double c[3] = { kR / 255.0, kG / 255.0, kB / 255.0 };   // R,G,B
    for (int i = 0; i < 3; ++i) {
        const double f = masked_white ? t[i] : l;
        out[i] = (uint8_t)std::lround(std::clamp(c[i] * f, 0.0, 1.0) * 255.0);
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
    SDL_Window* win = SDL_CreateWindow("sprite_mask_smoke", 64, 64,
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

    std::printf("=== sprite_mask_smoke (Vestuario) — la mascara contra su formula ===\n\n");

    // -- Los dos PNGs sintéticos, al build dir (mismo criterio que acetato_fx).
    const std::string root = AYTHER_SOURCE_DIR;
    const std::string asset_png = root + "/build/sprite_mask_asset.png";
    const std::string mask_png  = root + "/build/sprite_mask_mask.png";
    {
        std::vector<uint8_t> img((size_t)kSpr * kSpr * 4);
        for (size_t i = 0; i < img.size(); i += 4) {
            img[i] = kR; img[i + 1] = kG; img[i + 2] = kB; img[i + 3] = 255;
        }   // stb escribe RGBA
        if (!stbi_write_png(asset_png.c_str(), kSpr, kSpr, 4, img.data(), kSpr * 4)) {
            std::fprintf(stderr, "[FAIL] no se pudo escribir %s\n", asset_png.c_str());
            return 1;
        }
        // Máscara: mitad IZQUIERDA negra (solo luma) · DERECHA blanca (tinte).
        std::vector<uint8_t> m((size_t)kSpr * kSpr);
        for (int y = 0; y < kSpr; ++y)
            for (int x = 0; x < kSpr; ++x)
                m[(size_t)y * kSpr + x] = x < kSpr / 2 ? 0 : 255;
        if (!stbi_write_png(mask_png.c_str(), kSpr, kSpr, 1, m.data(), kSpr)) {
            std::fprintf(stderr, "[FAIL] no se pudo escribir %s\n", mask_png.c_str());
            return 1;
        }
    }

    // -- Tres subs: con máscara · sin máscara (control) · con máscara ESPEJADA.
    AytherSpriteSub subs[3];
    std::memset(subs, 0, sizeof(subs));
    auto fill = [&](AytherSpriteSub& s, int x, int y, bool masked) {
        std::snprintf(s.asset_path, sizeof(s.asset_path), "%s", asset_png.c_str());
        if (masked)
            std::snprintf(s.mask_path, sizeof(s.mask_path), "%s", mask_png.c_str());
        s.screen_x = (int16_t)x; s.screen_y = (int16_t)y;
        s.w_tiles = kSpr / 8; s.h_tiles = kSpr / 8;
        s.w_px = kSpr; s.h_px = kSpr;
        s.palette = 0xFF;
        s.u0 = 0.0f; s.v0 = 0.0f; s.uw = 1.0f; s.vh = 1.0f;
    };
    fill(subs[0],  16, 16, true);    // con Vestuario
    fill(subs[1], 112, 16, false);   // control: tinte entero clásico
    fill(subs[2], 208, 16, true);    // con Vestuario, sub ESPEJADO en H
    const uint8_t flips[3] = { 0, 0, 1 };
    uint8_t tint[9];
    for (int i = 0; i < 3; ++i)
        for (int c2 = 0; c2 < 3; ++c2) tint[i * 3 + c2] = kTintQ[c2];

    AytherLayerStack stack;
    bool stack_configured = true;
    for (const AytherLayer& l : stack.layers())
        stack_configured &= stack.set_visible(l.id, l.kind == AytherLayerKind::SpritesHd);
    check(stack_configured,
          "el stack de capas quedo configurado (si no, se compone vacio)");

    FrameView fv{};
    fv.sprite_subs      = subs;
    fv.sprite_sub_count = 3;
    fv.sprite_sub_flips = flips;
    fv.sprite_sub_tint  = tint;

    // -- Render en loop hasta que el decode asíncrono cargue asset y máscara
    //    (gpu-oracle-texstate-ready: sin espera, el oráculo pasa VACUO). El
    //    criterio de listo es el PIXEL: la mitad blanca del sub enmascarado
    //    tiene que coincidir con el control — hasta entonces el quad cae al
    //    pipeline clásico (fallback) o no se dibuja.
    uint8_t want_white[3], want_luma[3];
    expect_px(true,  want_white);
    expect_px(false, want_luma);
    const uint8_t* got = nullptr;
    auto px = [&](int x, int y) { return got + ((size_t)y * kW + x) * 4; };
    bool ready = false;
    for (int it = 0; it < 240 && !ready; ++it) {
        got = renderer.export_frame(ctx, fv, nullptr, /*hd_on=*/true, &stack);
        if (!got) break;
        // Punto de la mitad negra (izq) y de la blanca (der) del sub 0.
        ready = near3(px(16 + 16, 48), want_luma) &&
                near3(px(16 + 48, 48), want_white);
    }
    check(got != nullptr, "el readback devuelve pixeles");
    if (!got) return 1;

    // NO-VACUIDAD: el control tiene que estar dibujado y tintado (≠ color del
    // PNG crudo) — un frame sin sprites pasaría cualquier comparación relativa.
    {
        const uint8_t raw[3] = { kR, kG, kB };
        check(!near3(px(112 + 32, 48), raw),
              "NO vacuo: el control esta dibujado y TINTADO (no el PNG crudo)");
    }

    // 1. Mitad BLANCA = tinte completo, y ADEMÁS igual al control clásico.
    {
        char msg[160];
        uint8_t const* p = px(16 + 48, 48);
        std::snprintf(msg, sizeof(msg),
                      "mitad blanca = tinte completo (got %d,%d,%d · want %d,%d,%d)",
                      p[2], p[1], p[0], want_white[0], want_white[1], want_white[2]);
        check(near3(p, want_white), msg);
        const uint8_t* q = px(112 + 48, 48);
        check(std::abs((int)p[0] - (int)q[0]) <= 1 &&
              std::abs((int)p[1] - (int)q[1]) <= 1 &&
              std::abs((int)p[2] - (int)q[2]) <= 1,
              "…y coincide con el sub SIN mascara (blanco == comportamiento clasico)");
    }
    // 2. Mitad NEGRA = solo la luma del tinte.
    {
        char msg[160];
        const uint8_t* p = px(16 + 16, 48);
        std::snprintf(msg, sizeof(msg),
                      "mitad negra = solo luma (got %d,%d,%d · want %d,%d,%d)",
                      p[2], p[1], p[0], want_luma[0], want_luma[1], want_luma[2]);
        check(near3(p, want_luma), msg);
    }
    // 3. Pre-flip: en el sub espejado la mitad TEÑIDA se da vuelta (el asset es
    //    simétrico — solo la máscara puede mover el lado).
    {
        check(near3(px(208 + 16, 48), want_white),
              "sub espejado H: la izquierda ahora es la mitad BLANCA");
        check(near3(px(208 + 48, 48), want_luma),
              "sub espejado H: la derecha ahora es la mitad NEGRA");
    }
    // 4. Barrido por área: TODA la mitad de cada lado cumple su fórmula (no
    //    solo el punto sonda) — un borde corrido de 1 px aparecería acá.
    {
        size_t bad = 0, total = 0;
        for (int y = 18; y < 16 + kSpr - 2; ++y)
            for (int x = 18; x < 16 + kSpr - 2; ++x) {
                if (x == 16 + kSpr / 2 || x == 16 + kSpr / 2 - 1) continue;  // frontera (filtro)
                const bool white = x >= 16 + kSpr / 2;
                ++total;
                if (!near3(px(x, y), white ? want_white : want_luma)) ++bad;
            }
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "barrido del sub enmascarado: %zu/%zu px fuera de formula",
                      bad, total);
        check(bad == 0, msg);
    }
    // 5. Fallback: una máscara ILEGIBLE cae al tinte entero (no transparente,
    //    no colgado). Se re-apunta el sub 0 a un path inexistente.
    {
        std::snprintf(subs[0].mask_path, sizeof(subs[0].mask_path), "%s",
                      (root + "/build/no_existe_vestuario.png").c_str());
        const uint8_t* g2 = nullptr;
        for (int it = 0; it < 60; ++it) {
            g2 = renderer.export_frame(ctx, fv, nullptr, true, &stack);
            if (g2 && near3(g2 + ((size_t)48 * kW + 16 + 16) * 4, want_white))
                break;   // el lado negro pasó a tinte entero = fallback activo
        }
        check(g2 && near3(g2 + ((size_t)48 * kW + 16 + 16) * 4, want_white),
              "mascara ilegible → fallback al tinte entero (negative-cache)");
    }

    vkDeviceWaitIdle(ctx.device());
    renderer.readback_shutdown(ctx);
    renderer.shutdown(ctx);
    ctx.shutdown();
    SDL_DestroyWindow(win);
    SDL_Quit();

    std::error_code ec;
    std::filesystem::remove(asset_png, ec);
    std::filesystem::remove(mask_png, ec);

    std::printf("\n%d checks, %d fails\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
