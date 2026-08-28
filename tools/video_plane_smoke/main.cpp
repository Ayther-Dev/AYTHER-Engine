// ---------------------------------------------------------------------------
// video_plane_smoke (#479) — el video de la Cinemática compone POR PLANOS,
// verificado por píxel en la GPU real.
//
// POR QUÉ EXISTE. #479 se implementó y se verificó con los oráculos que ya
// había: 76/76 en ctest, video_shader 4/4, panorama sin fallos. Todos ésos
// dicen «no rompí nada» — ninguno dice «el video ya no tapa los Sprites
// vivos», que es la afirmación entera de la issue. Su diseño aprobado
// (docs/design/cinematica-por-planos.md, sección «Verificación») pide
// exactamente este oráculo y nunca se escribió; la issue quedó abierta
// esperando que alguien mirara una Cinemática a ojo.
//
// EL PAR ES NO VACÍO, y ése es el punto. Se rinde el MISMO frame dos veces:
//
//   con máscara A+B  el video va inline en el z del plano más alto de su
//                    máscara ⇒ el Sprite vivo queda ENCIMA
//   sin máscara      cae a la lane global 4V, pantalla completa y opaca ⇒ el
//                    video TAPA el Sprite
//
// El segundo caso ES el defecto que #479 arregla, y está acá a propósito: un
// oráculo que sólo comprueba el caso bueno pasa en verde aunque el arreglo no
// haga nada. Acá el mismo píxel tiene que dar rojo en uno y gris en el otro.
//
// SIN ROM NI GRABACIÓN. La escena se sintetiza: 4 bytes de VRAM, dos entradas
// de CRAM y dos SceneElement. Basta para que `scene_ready` sea cierto, que es
// la puerta del camino compuesto — el único que #479 toca.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target video_plane_smoke (requiere GPU)
//   Args:  ninguno
// ---------------------------------------------------------------------------
#include "ayther_layers.h"
#include "ayther_renderer.h"
#include "ayther_session.h"
#include "vulkan_backend/vk_context.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif

using ayther::FrameView;
using ayther::SceneElement;

namespace {

int g_checks = 0, g_fails = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

constexpr uint32_t kW = 320, kH = 224;

// CRAM del core: R = bits 0-2, G = 3-5, B = 6-8 (empaquetada, sin huecos).
constexpr uint16_t kRojo = 7;        // índice 1 → rojo puro
constexpr uint16_t kAzul = 7 << 6;   // índice 2 → azul puro

// Dónde vive el Sprite vivo, y el píxel que se interroga (bien adentro del
// elemento para que un error de medio píxel en el borde no decida el veredicto).
constexpr int kSprX = 160, kSprY = 100;
constexpr int kPxX  = kSprX + 4, kPxY = kSprY + 4;

struct Px { uint8_t b, g, r; };
Px pixel_at(const uint8_t* bgra, uint32_t w, int x, int y) {
    const size_t i = ((size_t)y * w + x) * 4;
    return Px{ bgra[i + 0], bgra[i + 1], bgra[i + 2] };
}

bool is_red(Px p) { return p.r > 200 && p.g < 60 && p.b < 60; }
bool is_gray(Px p) {
    const int mx = p.r > p.g ? (p.r > p.b ? p.r : p.b) : (p.g > p.b ? p.g : p.b);
    const int mn = p.r < p.g ? (p.r < p.b ? p.r : p.b) : (p.g < p.b ? p.g : p.b);
    return (mx - mn) < 24 && mx > 40 && mx < 220;
}

}  // namespace

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "[FAIL] SDL_Init\n"); return 1; }
    SDL_Window* win = SDL_CreateWindow("video_plane_smoke", 64, 64,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!win) { std::fprintf(stderr, "[FAIL] SDL_CreateWindow\n"); return 1; }
    VkContext ctx;
    if (!ctx.init(win)) { std::fprintf(stderr, "[FAIL] VkContext::init\n"); return 1; }

    ayther::AytherRenderer renderer;
    const std::string sh = std::string(AYTHER_SOURCE_DIR) +
                           "/shaders/";
    if (!renderer.init(ctx, kW, kH, sh.c_str()) || !renderer.readback_init(ctx)) {
        std::fprintf(stderr, "[FAIL] renderer (shaders en %s)\n", sh.c_str());
        return 1;
    }

    std::printf("=== video_plane_smoke (#479) — el video en el z de su mascara ===\n");

    // -- La escena sintética ---------------------------------------------------
    // Patrón 1 = todo índice 1 (rojo) · patrón 2 = todo índice 2 (azul). 4 bpp,
    // 32 bytes por tile: el byte 0x11 da los dos nibbles en 1 sea cual sea el
    // orden, así que el tile sale uniforme sin depender del decode.
    std::vector<uint8_t> vram(0x10000, 0);
    for (int i = 0; i < 32; ++i) { vram[1 * 32 + i] = 0x11; vram[2 * 32 + i] = 0x22; }

    std::vector<uint8_t> cram(128, 0);
    cram[1 * 2] = (uint8_t)(kRojo & 0xFF); cram[1 * 2 + 1] = (uint8_t)(kRojo >> 8);
    cram[2 * 2] = (uint8_t)(kAzul & 0xFF); cram[2 * 2 + 1] = (uint8_t)(kAzul >> 8);

    // Dos elementos: el fondo azul en el plano B y el Sprite ROJO vivo. El
    // sprite es el testigo — es justo lo que el defecto tapaba.
    std::vector<SceneElement> scene;
    for (int cy = 0; cy < (int)kH / 8; ++cy)
        for (int cx = 0; cx < (int)kW / 8; ++cx) {
            SceneElement e{};
            e.x = (int16_t)(cx * 8); e.y = (int16_t)(cy * 8);
            e.pattern = 2; e.layer = 0;             // plano B, azul
            e.hash = 0x100 + cy * 64 + cx;
            scene.push_back(e);
        }
    {
        SceneElement e{};
        e.x = kSprX; e.y = kSprY; e.w = 16; e.h = 16;
        e.pattern = 1; e.layer = 3;                 // Sprite VIVO, rojo
        e.slot = 0; e.chain = 0; e.hash = 0xABCD;
        scene.push_back(e);
    }

    // -- El video: gris plano (U = V = 128, Y medio) --------------------------
    const uint32_t vw = kW, vh = kH, cw = (vw + 1) / 2, chh = (vh + 1) / 2;
    std::vector<uint8_t> py((size_t)vw * vh, 128), pu((size_t)cw * chh, 128),
                         pv((size_t)cw * chh, 128);

    AytherLayerStack stack;

    // El framebuffer del emulador tiene que estar aunque no se vea: es lo unico
    // que le da al renderer las dimensiones del frame (`emu_w_`), y sin ellas
    // `scene_ready` es falso y el camino compuesto —el unico que #479 toca— no
    // corre. Se llena de VERDE, que no aparece en ningun otro lado: si alguna
    // vez se ve, es que el compose no dibujo y el frame es el blit crudo.
    std::vector<uint16_t> fb((size_t)kW * kH, 0x07E0);   // RGB565 verde puro

    FrameView fv{};
    fv.fb_width = kW; fv.fb_height = kH;
    fv.fb_pixels = fb.data(); fv.fb_pitch = kW * 2; fv.fb_format = 2;
    fv.scene = scene.data(); fv.scene_count = (uint32_t)scene.size();
    fv.scene_vram = vram.data(); fv.scene_vram_size = vram.size();
    fv.scene_cram = cram.data(); fv.scene_cram_size = cram.size();
    fv.scene_dirty = 0;
    fv.video_y = py.data(); fv.video_y_stride = vw;
    fv.video_u = pu.data(); fv.video_u_stride = cw;
    fv.video_v = pv.data(); fv.video_v_stride = cw;
    fv.video_w = vw; fv.video_h = vh;

    uint64_t seq = 0;
    auto render = [&](uint8_t mask, std::vector<uint8_t>& out) -> bool {
        fv.video_plane_mask = mask;
        fv.video_seq = ++seq;   // sin esto el renderer se saltea la re-subida
        const uint8_t* px = renderer.export_frame(ctx, fv, nullptr, true, &stack);
        if (!px) return false;
        out.assign(px, px + (size_t)kW * kH * 4);
        return true;
    };

    // -- CONTROL DE NO VACUIDAD: sin video, el sprite se ve ------------------
    // Si el testigo no fuera rojo ni siquiera sin video, los dos casos de abajo
    // darían «no es gris» por la razón equivocada y el oráculo sería falso.
    std::vector<uint8_t> without_video;
    {
        const void* y = fv.video_y; fv.video_y = nullptr;
        const bool ok = render(0, without_video);
        fv.video_y = y;
        check(ok, "rinde el frame sin video");
        if (!ok) return 1;
        const Px p = pixel_at(without_video.data(), kW, kPxX, kPxY);
        check(is_red(p), "CONTROL: sin video el Sprite vivo es ROJO en el testigo");
        std::printf("        testigo (%d,%d) = R%u G%u B%u\n", kPxX, kPxY, p.r, p.g, p.b);
    }

    // -- CASO VIEJO: sin máscara, el video TAPA lo vivo ----------------------
    std::vector<uint8_t> unmasked_frame;
    {
    check(render(0, unmasked_frame), "rinde el frame sin mascara (lane global 4V)");
    const Px p = pixel_at(unmasked_frame.data(), kW, kPxX, kPxY);
        check(is_gray(p), "sin mascara el video TAPA el Sprite (el defecto de #479)");
        std::printf("        testigo (%d,%d) = R%u G%u B%u\n", kPxX, kPxY, p.r, p.g, p.b);
    }

    // -- CASO NUEVO: máscara A+B, el Sprite queda encima ---------------------
    std::vector<uint8_t> masked_frame;
    {
    check(render(0x03, masked_frame), "rinde el frame con mascara A+B");
    const Px p = pixel_at(masked_frame.data(), kW, kPxX, kPxY);
        check(is_red(p), "con mascara A+B el Sprite vivo queda ENCIMA del video");
        std::printf("        testigo (%d,%d) = R%u G%u B%u\n", kPxX, kPxY, p.r, p.g, p.b);
    }

    // -- Y el video SÍ está: fuera del sprite el fondo dejó de ser azul ------
    // Sin esto, «el sprite se ve» pasaría también si el video no se dibujara en
    // absoluto — que es el modo en que este oráculo podría mentir.
    {
    const Px new_background = pixel_at(masked_frame.data(), kW, 20, 20);
        const Px background_without_video = pixel_at(without_video.data(), kW, 20, 20);
        check(is_gray(new_background),
              "con mascara el video SI cubre el fondo (no es que no se dibuje)");
        check(background_without_video.b > 200 && background_without_video.r < 60,
              "CONTROL: sin video ese mismo fondo es AZUL");
        std::printf("        fondo (20,20): con video R%u G%u B%u · sin video R%u G%u B%u\n",
                    new_background.r, new_background.g, new_background.b,
                    background_without_video.r, background_without_video.g, background_without_video.b);
    }

    // -- DECISIÓN 3: el Cuadro pri-1 manda el video al FRENTE ----------------
    // El mismo frame, con la única diferencia de `video_front`. Sirve de
    // control cruzado del caso anterior: si el inline se dibujara al frente por
    // accidente, los dos darían gris y el par de arriba nunca lo notaría.
    {
        std::vector<uint8_t> frente;
        fv.video_front = 1;
        check(render(0x03, frente), "rinde el frame con mascara A+B y pri-1");
        const Px p = pixel_at(frente.data(), kW, kPxX, kPxY);
        check(is_gray(p),
              "con Cuadro pri-1 el video va al FRENTE y TAPA el Sprite (decision 3)");
        std::printf("        testigo (%d,%d) = R%u G%u B%u\n", kPxX, kPxY, p.r, p.g, p.b);
        fv.video_front = 0;
    }

    renderer.readback_shutdown(ctx);
    renderer.shutdown(ctx);
    ctx.shutdown();
    SDL_DestroyWindow(win);
    SDL_Quit();

    std::printf("\n%d checks, %d fails\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
