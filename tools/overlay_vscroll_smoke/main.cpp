// ---------------------------------------------------------------------------
// overlay_vscroll_smoke (#484) — el Acetato anclado bajo vscroll POR COLUMNA,
// EJECUTADO en la GPU real y comparado por píxel contra su especificación.
//
// POR QUÉ EXISTE. Las nubes del título de Golden Axe scrollean en modo VS del
// VDP: 16 tiras de 16 px, cada una con su fase vertical propia (dos
// velocidades entrelazadas). La lane Custom ancla por tira: recorta la banda
// de la lámina con el sub-rect UV y la desplaza con SU columna de VSRAM
// (FrameView::plane_vscroll_col). La fórmula por píxel es trivial —
// fila_lámina = (y + v_col(x/16)) mod img_h — y por eso se fija acá, texel a
// texel, con la lámina codificando fila y columna en el color.
//
// NO NECESITA ROM NI GRABACIÓN (molde: sprite_mask_smoke): un PNG sintético
// de 320×64 (R = fila·4 · G = 128 · B = banda·12), un FrameView armado a mano
// y un stack donde solo la capa Custom es visible. Canvas 320×240 = escala
// 1:1 → NEAREST → comparación exacta (±1 UNORM).
//
// LO QUE CAE ADENTRO:
//   · Whole-plane (vs_two_cell=false): fila = (y + V global) mod H — el
//     camino de siempre, byte-exacto (la referencia del resto).
//   · Por columna (vs_two_cell=true): cada tira de 16 px con su V — filas
//     DISTINTAS entre tiras vecinas en el mismo y de pantalla.
//   · Columnas replicadas: vs_two_cell=true con todas las columnas = global
//     da EXACTAMENTE el whole-plane (20 quads vs 1, mismo píxel).
//   · #484 anclaje H crudo: con la cámara EM-1 cargada de residuo
//     (plane_cam_x≠0) y tile_mode=1, la columna de textura NO se corre — el
//     residuo de pantallas anteriores corría la costura del wrap al medio.
//   · Determinismo: dos renders idénticos.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target overlay_vscroll_smoke (requiere GPU)
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

constexpr uint32_t kW = 320, kH = 240;   // canvas = res nativa → escala 1:1
constexpr int kImgW = 320, kImgH = 64;

// La lámina codifica su propia coordenada: R = fila·4 (0-252), B = banda
// horizontal de 16 px ·12 (0-228), G = 128 constante.
uint8_t row_r(int row)  { return (uint8_t)((row % kImgH) * 4); }
uint8_t col_b(int x)    { return (uint8_t)((x / 16) * 12); }

bool near3(const uint8_t* bgra, uint8_t r, uint8_t g, uint8_t b, int tol = 1) {
    return std::abs((int)bgra[2] - (int)r) <= tol &&
           std::abs((int)bgra[1] - (int)g) <= tol &&
           std::abs((int)bgra[0] - (int)b) <= tol;
}

}  // namespace

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "[FAIL] SDL_Init\n"); return 1; }
    SDL_Window* win = SDL_CreateWindow("overlay_vscroll_smoke", 64, 64,
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

    std::printf("=== overlay_vscroll_smoke (#484) — anclaje por columna contra su formula ===\n\n");

    const std::string png = std::string(AYTHER_SOURCE_DIR) + "/build/acetato_vs_lamina.png";
    {
        std::vector<uint8_t> img((size_t)kImgW * kImgH * 4);
        for (int y = 0; y < kImgH; ++y)
            for (int x = 0; x < kImgW; ++x) {
                uint8_t* p = &img[((size_t)y * kImgW + x) * 4];
                p[0] = row_r(y); p[1] = 128; p[2] = col_b(x); p[3] = 255;
            }
        if (!stbi_write_png(png.c_str(), kImgW, kImgH, 4, img.data(), kImgW * 4)) {
            std::fprintf(stderr, "[FAIL] no se pudo escribir %s\n", png.c_str());
            return 1;
        }
    }

    AytherLayerStack stack;
    const uint32_t lid = stack.insert_custom("acetato_vs", stack.layers().size());
    for (const AytherLayer& l : stack.layers())
        stack.set_visible(l.id, l.id == lid);
    AytherLayerContent cc{};
    std::snprintf(cc.asset, sizeof(cc.asset), "%s", png.c_str());
    cc.img_w = kImgW; cc.img_h = kImgH;
    cc.y = 0; cc.anchor = 0; cc.factor = 1.0f;   // ancla Plano B, 1:1
    cc.tile_mode = 1;
    stack.set_content(lid, cc);

    // Fases por columna: variadas y con wrap (v_s = 7·s·8 mod 512 — algunas
    // superan img_h para ejercitar el mod).
    int16_t vcol[20];
    for (int s = 0; s < 20; ++s) vcol[s] = (int16_t)((7 * s * 8) % 512);

    FrameView fv{};
    fv.fps_timing = 60.0;
    fv.plane_wpx = 512; fv.plane_hpx = 512;

    auto set_whole = [&](int16_t v) {
        fv.vs_two_cell = false;
        fv.plane_vscroll[1] = v;
        for (int s = 0; s < 20; ++s)
            fv.plane_vscroll_col[1][s] = v;   // la sesión replica el global
    };
    auto set_percol = [&]() {
        fv.vs_two_cell = true;
        fv.plane_vscroll[1] = vcol[0];
        for (int s = 0; s < 20; ++s) fv.plane_vscroll_col[1][s] = vcol[s];
    };

    const uint8_t* got = nullptr;
    auto px = [&](int x, int y) { return got + ((size_t)y * kW + x) * 4; };

    // -- Caso 1: whole-plane V=20 — y de paso la espera del decode async
    //    (gpu-oracle-texstate-ready: sin espera, el oráculo pasa VACUO).
    set_whole(20);
    bool ready = false;
    for (int it = 0; it < 240 && !ready; ++it) {
        got = renderer.export_frame(ctx, fv, nullptr, /*hd_on=*/true, &stack);
        if (!got) break;
        ready = near3(px(8, 8), row_r(8 + 20), 128, col_b(8));
    }
    check(got != nullptr, "el readback devuelve pixeles");
    if (!got) return 1;
    check(ready, "whole-plane: fila = (y + V) mod H (la lamina cargo y ancla)");
    check(near3(px(200, 100), row_r(100 + 20), 128, col_b(200)),
          "whole-plane: segundo punto (wrap vertical del tileado)");

    // -- Caso 2: por columna — tiras vecinas con filas DISTINTAS al mismo y.
    set_percol();
    got = renderer.export_frame(ctx, fv, nullptr, true, &stack);
    if (!got) return 1;
    bool all_ok = true;
    for (int s = 0; s < 20 && all_ok; ++s) {
        const int x = s * 16 + 8, y = 50;
        all_ok = near3(px(x, y), row_r(y + vcol[s]), 128, col_b(x));
    }
    check(all_ok, "por columna: cada tira de 16 px con SU fase (20/20)");
    check(!near3(px(24, 50), row_r(50 + vcol[0]), 128, col_b(24)) ||
              vcol[1] % kImgH == vcol[0] % kImgH,
          "no-vacuidad: la tira 1 NO muestra la fase de la tira 0");

    // -- Caso 3: columnas replicadas = whole-plane exacto.
    set_whole(20);
    got = renderer.export_frame(ctx, fv, nullptr, true, &stack);
    if (!got) return 1;
    std::vector<uint8_t> whole(got, got + (size_t)kW * kH * 4);
    fv.vs_two_cell = true;   // 20 quads, mismas fases
    got = renderer.export_frame(ctx, fv, nullptr, true, &stack);
    if (!got) return 1;
    check(std::memcmp(whole.data(), got, whole.size()) == 0,
          "columnas replicadas: identico al whole-plane (20 quads vs 1)");

    // -- Caso 4 (#484 anclaje H): la camara EM-1 con residuo NO corre la
    //    textura con tile_mode=1 (el hscroll crudo manda).
    set_percol();
    fv.plane_cam_valid = true;
    fv.plane_cam_x[1] = 203;      // residuo de pantallas anteriores
    fv.plane_hscroll[1] = 0;
    got = renderer.export_frame(ctx, fv, nullptr, true, &stack);
    if (!got) return 1;
    check(near3(px(200, 50), row_r(50 + vcol[12]), 128, col_b(200)),
          "camara con residuo: la columna de textura NO se corre (h = crudo)");
    fv.plane_cam_valid = false; fv.plane_cam_x[1] = 0;

    // -- Caso 5: determinismo — dos renders identicos.
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
