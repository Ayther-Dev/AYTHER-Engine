// ---------------------------------------------------------------------------
// ab_compare_smoke (#305) — la imagen de COMPARACIÓN del preview A/B, por píxel.
//
// POR QUÉ EXISTE. El A/B parte el viewport en Original | AYTHER y las dos
// mitades tienen que ser el MISMO cuadro: si no, lo que el artista lee como una
// diferencia del pack es en realidad una diferencia de tiempo, y la herramienta
// miente justo cuando más se la cree. Lo que sostiene esa promesa es una sola
// cosa —que `capture_compare` congele exactamente lo que el offscreen tenía—, y
// eso no se puede afirmar mirando: hay que leer la copia.
//
// El oráculo, sobre un canvas sintético (sin ROM, sin grabación, como el de
// #378 — el renderer expone export_frame(FrameView) y eso alcanza):
//
//   1. la copia es BYTE-IDÉNTICA al offscreen que la originó;
//   2. re-renderizar ENCIMA no la toca — es lo que permite mostrar las dos
//      versiones a la vez, y el motivo de que exista una segunda imagen;
//   3. capturar de nuevo la ACTUALIZA (el A/B corre en vivo: cada frame
//      recaptura, y una copia pegada mostraría un cuadro viejo);
//   4. la copia sobrevive a re-renders sucesivos sin degradarse;
//   5. NO VACUIDAD: las dos versiones que se comparan son distintas entre sí.
//      Sin este check todo lo anterior pasaría con un canvas negro.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target ab_compare_smoke (GPU)
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_renderer.h"
#include "ayther_layers.h"
#include "vulkan_backend/vk_context.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using ayther::FrameView;

namespace {

int g_checks = 0, g_fails = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

constexpr uint32_t kW = 128, kH = 96;

/// Un frame de emulador sintético: bandas verticales de color, distintas por
/// columna, para que cualquier corrimiento o media imagen se note.
std::vector<uint32_t> emu_frame(uint8_t seed) {
    std::vector<uint32_t> px((size_t)kW * kH);
    for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x) {
            const uint8_t r = (uint8_t)(x * 2 + seed);
            const uint8_t g = (uint8_t)(y * 2 + seed);
            const uint8_t b = (uint8_t)(seed + 40);
            px[(size_t)y * kW + x] =
                (0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    return px;
}

}  // namespace

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "[FAIL] SDL_Init\n"); return 1; }
    SDL_Window* win = SDL_CreateWindow("ab_compare_smoke", 64, 64,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!win) { std::fprintf(stderr, "[FAIL] SDL_CreateWindow\n"); return 1; }
    VkContext ctx;
    if (!ctx.init(win)) { std::fprintf(stderr, "[FAIL] VkContext::init\n"); return 1; }

    ayther::AytherRenderer renderer;
    const std::string sh =
        std::string(AYTHER_SOURCE_DIR) + "/shaders/";
    if (!renderer.init(ctx, kW, kH, sh.c_str()) || !renderer.readback_init(ctx)) {
        std::fprintf(stderr, "[FAIL] renderer (shaders en %s)\n", sh.c_str());
        return 1;
    }

    std::printf("=== ab_compare_smoke (#305) — la copia del A/B, por pixel ===\n");
    std::printf("canvas %ux%u\n\n", kW, kH);

    const size_t bytes = (size_t)kW * kH * 4;
    AytherLayerStack stack;

    auto render_seed = [&](uint8_t seed, std::vector<uint8_t>& out) -> bool {
        const std::vector<uint32_t> px = emu_frame(seed);
        FrameView fv{};
        fv.fb_pixels = px.data();
        fv.fb_width  = kW;
        fv.fb_height = kH;
        fv.fb_pitch  = kW * 4;
        fv.fb_format = 1;   // XRGB8888 — el layout de emu_frame()
        const uint8_t* g = renderer.export_frame(ctx, fv, nullptr, /*hd_on=*/true,
                                                 &stack);
        if (!g) return false;
        out.assign(g, g + bytes);
        return true;
    };

    // ---- El cuadro «Original» y su copia ------------------------------------
    std::vector<uint8_t> orig, hd, copy;
    if (!render_seed(0, orig)) { std::fprintf(stderr, "[FAIL] render A\n"); return 1; }
    check(!renderer.compare_ready(),
          "la copia NO existe hasta que se pide (un Lab sin A/B no paga la memoria)");

    if (!renderer.capture_compare_now(ctx)) {
        std::fprintf(stderr, "[FAIL] capture_compare\n"); return 1;
    }
    check(renderer.compare_ready(), "capturar crea la imagen de comparacion");
    check(renderer.compare_extent().width == kW &&
              renderer.compare_extent().height == kH,
          "…con el extent del offscreen");

        if (const uint8_t* c = renderer.readback_compare(ctx)) copy.assign(c, c + bytes);
        check(copy.size() == bytes && copy == orig,
          "1. la copia es BYTE-IDENTICA al offscreen que la origino");

    // ---- Re-renderizar encima NO la toca ------------------------------------
    if (!render_seed(90, hd)) { std::fprintf(stderr, "[FAIL] render B\n"); return 1; }
    check(hd != orig,
          "5. NO VACUIDAD: los dos cuadros que se comparan son DISTINTOS");

    copy.clear();
    if (const uint8_t* c = renderer.readback_compare(ctx)) copy.assign(c, c + bytes);
    check(copy == orig,
          "2. re-renderizar encima NO altera la copia (las dos versiones a la vez)");

    // ---- Capturar de nuevo la actualiza (el A/B corre en vivo) --------------
    if (!renderer.capture_compare_now(ctx)) {
        std::fprintf(stderr, "[FAIL] capture_compare 2\n"); return 1;
    }
    copy.clear();
    if (const uint8_t* c = renderer.readback_compare(ctx)) copy.assign(c, c + bytes);
    check(copy == hd,
          "3. capturar de nuevo la ACTUALIZA (sin esto el A/B mostraria un cuadro viejo)");

    // ---- Y aguanta una tanda de re-renders sin degradarse -------------------
    bool estable = true;
    for (uint8_t i = 1; i <= 5 && estable; ++i) {
        std::vector<uint8_t> tmp;
        if (!render_seed((uint8_t)(120 + i * 7), tmp)) { estable = false; break; }
        std::vector<uint8_t> again;
        if (const uint8_t* c = renderer.readback_compare(ctx))
            again.assign(c, c + bytes);
        estable = (again == hd);
    }
    check(estable, "4. la copia sobrevive intacta a 5 re-renders sucesivos");

    // ---- Liberarla vuelve al estado inicial ---------------------------------
    renderer.compare_release(ctx);
    check(!renderer.compare_ready(), "salir del A/B devuelve la memoria de la copia");
    check(renderer.readback_compare(ctx) == nullptr,
          "…y leerla despues no devuelve basura");

    renderer.readback_shutdown(ctx);
    renderer.shutdown(ctx);
    ctx.shutdown();
    SDL_DestroyWindow(win);
    SDL_Quit();

    std::printf("\n%s — %d checks, %d fallos\n",
                g_fails ? "=== FAIL ===" : "=== OK ===", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
