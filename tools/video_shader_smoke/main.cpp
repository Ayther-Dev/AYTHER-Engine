// ---------------------------------------------------------------------------
// video_shader_smoke (#378) — el shader de video EJECUTADO contra su
// especificación, por píxel y en la GPU real.
//
// POR QUÉ EXISTE. #378 movió la conversión I420→RGB de la CPU al fragment
// shader: era el 62% del costo por frame. `tests/video_bt601_test` fija la
// fórmula que la GPU tiene que dar, pero no ejecuta nada — compara la
// referencia contra una cuenta independiente. Este oráculo cierra el hueco:
// sube tres planos I420 sintéticos, dibuja la capa Video con el pipeline de
// verdad y compara el readback contra `ayther::video_i420_to_bgra_px` píxel a
// píxel. Sin él, un shader que convierta MAL no rompe nada: el video se ve,
// con los colores corridos, y nadie se entera hasta mirarlo con atención.
//
// NO NECESITA ROM NI GRABACIÓN. El renderer expone `export_frame(FrameView)`,
// así que la FrameView se arma a mano con los planos y un stack de capas donde
// sólo Video está visible. Todo lo que este oráculo prueba es el camino
// textura→shader→framebuffer.
//
// EL CROMA ES CONSTANTE EN CADA PASADA, y no es un atajo. La CPU replicaba el
// croma (`x >> 1`); el shader lo samplea con el sampler LINEAR, que INTERPOLA
// entre texels vecinos. Es una diferencia real y deliberada (transiciones de
// color más suaves). La igualdad exacta con la referencia sólo vale donde no
// hay nada que interpolar: con el plano de croma constante, `mix(c, c, t) == c`
// para cualquier t, así que el barrido recorre el espacio (Y,U,V) entero
// haciendo una pasada por cada par (U,V). La luma sí va a escala 1:1 — con un
// quad de canvas completo sobre una textura de las mismas dimensiones, el
// centro del píxel x cae exactamente en el centro del texel x y LINEAR devuelve
// el texel intacto.
//
// LO QUE ADEMÁS CAE ADENTRO:
//   · Los STRIDES. Los planos se llenan con pitch > ancho (libvpx alinea las
//     filas); si el upload lo ignorara, la imagen saldría cortada en diagonal.
//   · El ORDEN de los tres bindings. U y V cambiados dan un resultado distinto
//     en toda pasada con U≠V, y el barrido las tiene.
//   · El pipeline AUSENTE. Si falta video.frag.spv el init no aborta (a
//     propósito: sólo la Cinemática queda sin dibujar), así que acá se vería
//     como un frame entero en desacuerdo — el reporte lo dice explícito.
//
// SE VERIFICÓ QUE PUEDE FALLAR. Un oráculo que nunca falla no verifica nada, así
// que se le rompió el shader a propósito: cambiar UN coeficiente (409 → 408 en
// el término de R) lo pone en rojo con 14.898 de 129.792 píxeles en desacuerdo,
// y el primer desacuerdo que reporta es de una unidad (R=1 contra 0 esperado).
// Detecta errores de esa escala.
//
// LO QUE **NO** PUEDE DEFENDER, medido: reemplazar `floor(x/256.0)` por `x >> 8`
// deja las 169 pasadas en verde. En esta GPU (NVIDIA RTX 3060) el corrimiento de
// un signed negativo es aritmético y da lo mismo. O sea: la elección de floor()
// es un seguro de PORTABILIDAD contra §5.9 del spec, no la corrección de un bug
// observable acá, y este oráculo no la puede sostener — sólo la cita del spec.
// Si alguna vez «simplifica» el shader a `>>`, este smoke va a seguir verde y el
// video va a salir con los colores rotos en otra máquina.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target video_shader_smoke (requiere GPU)
//   Args:  ninguno
// ---------------------------------------------------------------------------
#include "ayther_layers.h"
#include "ayther_renderer.h"
#include "ayther_session.h"
#include "ayther_video.h"
#include "vulkan_backend/vk_context.h"
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

// El canvas y el video comparten dimensiones a propósito: es lo que hace que la
// luma se samplee 1:1 y la comparación pueda ser EXACTA (ver cabecera). 256 de
// ancho para que cada columna sea un valor de Y distinto y el barrido cubra los
// 256 sin repetir.
constexpr uint32_t kW = 256, kH = 256;

// Los valores de croma barridos: los extremos (0 y 255), el neutro (128), los
// límites de limited range (16 y 240) y el resto repartido. Cada par (U,V) es
// una pasada, así que son 13×13 = 169 renders × 256 valores de Y = 43.264
// combinaciones (Y,U,V) verificadas en la GPU.
constexpr int kChroma[] = { 0, 16, 32, 64, 90, 110, 128, 145, 170, 200, 224, 240, 255 };

}  // namespace

int main() {
    // -- Vulkan sobre una ventana oculta (igual que el resto de los oráculos de
    //    render: VkContext no es headless, necesita una superficie).
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "[FAIL] SDL_Init\n"); return 1; }
    SDL_Window* win = SDL_CreateWindow("video_shader_smoke", 64, 64,
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

    std::printf("=== video_shader_smoke (#378) — el shader contra su especificacion ===\n");
    std::printf("canvas %ux%u · %d valores de croma por eje · %d pasadas\n\n",
                kW, kH, (int)(sizeof(kChroma) / sizeof(kChroma[0])),
                (int)((sizeof(kChroma) / sizeof(kChroma[0])) *
                      (sizeof(kChroma) / sizeof(kChroma[0]))));

    // -- Los tres planos, con pitch DISTINTO del ancho (libvpx alinea las filas
    //    y el renderer tiene que respetar el stride que le pasan).
    const uint32_t cw = (kW + 1) / 2, ch = (kH + 1) / 2;
    const uint32_t y_pitch = kW + 13, c_pitch = cw + 7;
    std::vector<uint8_t> plane_y((size_t)y_pitch * kH, 0xAA);
    std::vector<uint8_t> plane_u((size_t)c_pitch * ch, 0xAA);
    std::vector<uint8_t> plane_v((size_t)c_pitch * ch, 0xAA);
    // Luma: la columna x vale Y = x. El relleno entre el ancho y el pitch queda
    // en 0xAA — si el upload lo copiara, la imagen se correría y no hay pasada
    // que sobreviva.
    for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x)
            plane_y[(size_t)y * y_pitch + x] = (uint8_t)x;

    // -- Sólo la capa Video visible: sin emulador, sin pack, sin escena.
    AytherLayerStack stack;
    bool stack_configured = true;
    for (const AytherLayer& l : stack.layers())
        stack_configured &= stack.set_visible(l.id, l.kind == AytherLayerKind::Video);
    check(stack_configured,
          "el stack de capas quedo configurado (si no, se compone vacio)");

    FrameView fv{};
    fv.video_y = plane_y.data(); fv.video_y_stride = y_pitch;
    fv.video_u = plane_u.data(); fv.video_u_stride = c_pitch;
    fv.video_v = plane_v.data(); fv.video_v_stride = c_pitch;
    fv.video_w = kW; fv.video_h = kH;

    // -- El barrido -----------------------------------------------------------
    size_t   worst_bad = 0, total_bad = 0, total_px = 0;
    int      worst_y = -1, worst_u = -1, worst_v = -1;
    int      got_b = 0, got_g = 0, got_r = 0, exp_b = 0, exp_g = 0, exp_r = 0;
    size_t   alpha_bad = 0;
    uint64_t seq = 0;
    bool     any_render = true;
    unsigned distinct_seen = 0;

    for (int U : kChroma) {
        for (int V : kChroma) {
            std::fill(plane_u.begin(), plane_u.end(), (uint8_t)U);
            std::fill(plane_v.begin(), plane_v.end(), (uint8_t)V);
            fv.video_seq = ++seq;   // sin esto el renderer se saltea la re-subida

            const uint8_t* got = renderer.export_frame(ctx, fv, nullptr, /*hd_on=*/true,
                                                       &stack);
            if (!got) { any_render = false; break; }

            // Se compara la fila del medio: todas las filas son idénticas por
            // construcción (la luma sólo varía en x), así que barrer las 256
            // multiplicaría el tiempo sin agregar una sola combinación nueva.
            // La fila 0 y la última se comparan igual, para que un error de
            // muestreo en los bordes verticales no pase inadvertido.
            const uint32_t rows[3] = { 0, kH / 2, kH - 1 };
            unsigned distinct_here = 0;
            uint32_t prev = 0xFFFFFFFFu;
            for (uint32_t ri = 0; ri < 3; ++ri) {
                const uint32_t yy = rows[ri];
                size_t bad_here = 0;
                for (uint32_t x = 0; x < kW; ++x) {
                    const size_t i = ((size_t)yy * kW + x) * 4;
                    uint8_t want[4];
                    ayther::video_i420_to_bgra_px((int)x, U, V, want);
                    ++total_px;
                    if (got[i + 3] != 0) ++alpha_bad;
                    if (got[i] == want[0] && got[i + 1] == want[1] && got[i + 2] == want[2])
                        ;   // coincide
                    else {
                        ++bad_here; ++total_bad;
                        if (worst_y < 0) {   // el PRIMER desacuerdo, con su detalle
                            worst_y = (int)x; worst_u = U; worst_v = V;
                            got_b = got[i]; got_g = got[i + 1]; got_r = got[i + 2];
                            exp_b = want[0]; exp_g = want[1]; exp_r = want[2];
                        }
                    }
                    if (ri == 1) {
                        const uint32_t c = (uint32_t)got[i] | ((uint32_t)got[i + 1] << 8) |
                                           ((uint32_t)got[i + 2] << 16);
                        if (c != prev) { ++distinct_here; prev = c; }
                    }
                }
                if (bad_here > worst_bad) worst_bad = bad_here;
            }
            if (distinct_here > distinct_seen) distinct_seen = distinct_here;
        }
        if (!any_render) break;
    }

    check(any_render, "el readback devuelve pixeles en todas las pasadas");

    // NO-VACUIDAD. Un frame negro entero no puede pasar el barrido (la
    // referencia varía con Y), pero la comprobación se hace explícita: sin ella,
    // un oráculo que degenerara a comparar constante contra constante seguiría
    // diciendo «OK». Con Y de 0 a 255 tienen que aparecer decenas de colores.
    {
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "el render NO es vacuo: %u colores distintos en una fila",
                      distinct_seen);
        check(distinct_seen > 32, msg);
    }

    {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "el shader coincide EXACTAMENTE con video_i420_to_bgra_px "
                      "(%zu/%zu px en desacuerdo)", total_bad, total_px);
        check(total_bad == 0, msg);
        if (total_bad) {
            std::printf("       primer desacuerdo: Y=%d U=%d V=%d → GPU B=%d G=%d R=%d "
                        "· esperado B=%d G=%d R=%d\n",
                        worst_y, worst_u, worst_v, got_b, got_g, got_r,
                        exp_b, exp_g, exp_r);
            if (total_bad == total_px)
                std::printf("       TODOS los pixeles: sospechar video.frag.spv ausente "
                            "(el init no aborta, sólo deja la Cinemática sin dibujar) "
                            "en %s\n", sh.c_str());
        }
    }

    // OPACIDAD y ALPHA son dos cosas distintas, y conviene no confundirlas.
    //
    // Que el video sea OPACO ya lo prueba el barrido de arriba: el pipeline
    // mezcla con SRC_ALPHA/ONE_MINUS_SRC_ALPHA y cada export_frame arranca el
    // offscreen en UNDEFINED (contenido descartado). Si el video saliera con
    // alpha < 1 se mezclaría con esa basura y no habría pasada que coincidiera
    // exactamente. 0 desacuerdos en 169 renders = el color de atrás no entra.
    //
    // El ALPHA del framebuffer, en cambio, el pase de video NO lo escribe a
    // propósito (srcAlphaBlendFactor=ZERO, dstAlphaBlendFactor=ONE en
    // vk_sprite.cpp): conserva el que ya tenía el offscreen. Acá vale 0 porque
    // este oráculo apaga todas las capas menos Video y el blit del emulador —
    // que es quien deja el canvas opaco — nunca corre. En vivo vale 255. Lo que
    // se fija es justamente eso: que el video no lo toque.
    {
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "el pase de video NO escribe alpha: conserva el del offscreen "
                      "(%zu px lo cambiaron)", alpha_bad);
        check(alpha_bad == 0, msg);
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
