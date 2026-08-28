// ---------------------------------------------------------------------------
// focus_layer_smoke (#372) — el ATENUADO SIGUE A LA CAPA que se está autorando,
// verificado POR PÍXEL en el compose (GPU), sin UI.
//
// La HUELLA de un elemento (los píxeles que realmente aporta) se obtiene
// rindiendo dos veces: normal y con el elemento OCULTO. Donde difieren, ese
// elemento es el que manda — lo que otro dibuja encima queda excluido solo, sin
// razonar sobre solapes de bbox. Y el render con el elemento oculto es además
// el término `dst` del blending, que es lo que hace verificable la opacidad.
//
// Sobre un frame real de GA (compone al 100%, R-5):
//
//   Parte 1 — VRAM (HD apagado, las 4 capas):
//     · enfocada su capa, la huella queda BYTE-EXACTA: lo que se autora se ve
//       como es, nunca atenuado.
//     · enfocada OTRA capa, cada píxel cumple la fórmula del compositor
//         out = src·(16/64)·(191/255) + dst·(1 − 191/255)
//       o sea tinte 0.25 Y opacidad 75%. El contrafáctico —comparar contra
//       src·0.25 a secas— es el que prueba que la opacidad está puesta: si el
//       atenuado fuera opaco, ese check pasaría y este fallaría.
//
//   Parte 2 — HD (encendido): el atenuado tiene que ALCANZAR a los assets, que
//     es justamente lo que se está autorando. Fue el bug real de #372: dimear
//     sólo los quads VRAM se ve nada más en lo que NO tiene asset. Acá no hay
//     fórmula exacta (el alpha del PNG no se puede despejar de un render), así
//     que se verifica lo que sí es incondicional: enfocada su capa la huella HD
//     queda byte-exacta, y enfocada otra se oscurece en TODOS los canales.
//
//   Parte 3 — sin foco (-1) el render vuelve byte-exacto al camino de siempre.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target focus_layer_smoke (requiere GPU)
//   Args:  <rec.arp> [frame]   (default 900)
//   Env:   AYTHER_PROBE_ROM
// ---------------------------------------------------------------------------
#include "ayther_env.h"
#include "ayther_session.h"
#include "ayther_recording.h"
#include "ayther_renderer.h"
#include "vulkan_backend/vk_context.h"
#include <SDL3/SDL.h>

#include <stb_image_write.h>   // la implementación ya la linkea ayther_engine

#include <cmath>
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
using ayther::SceneElement;

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

// Los dos factores del atenuado, tal como los emite el compositor:
//   tinte    0x10 en Q2.6   → 16/64
//   opacidad 255·3/4 = 191  → 191/255   (el shader divide por 255)
static const float kTint = 16.0f / 64.0f;
static const float kOpac = 191.0f / 255.0f;

static const char* kLayerName[4] = { "Plano B", "Plano A", "Window", "Sprites" };

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: focus_layer_smoke <rec.arp> [frame]\n");
        return 2;
    }
    const std::string rec_path = argv[1];
    const uint32_t F = argc > 2 ? (uint32_t)std::strtoul(argv[2], nullptr, 10) : 900u;

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

    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "[FAIL] SDL_Init\n"); return 1; }
    SDL_Window* win = SDL_CreateWindow("focus_layer_smoke", 64, 64,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!win) { std::fprintf(stderr, "[FAIL] SDL_CreateWindow\n"); return 1; }
    VkContext ctx;
    if (!ctx.init(win)) { std::fprintf(stderr, "[FAIL] VkContext::init\n"); return 1; }

    const FrameView* fv = s->replay_seek(*rec, F);
    if (!fv || !fv->fb_width || fv->scene_dirty || !fv->scene_count) {
        std::fprintf(stderr, "[FAIL] frame sin escena limpia (elegí uno que componga)\n");
        return 1;
    }
    const uint32_t W = fv->fb_width, H = fv->fb_height;
    const size_t   N = (size_t)W * H * 4;

    ayther::AytherRenderer renderer;
    const std::string sh = root + "/shaders/";
    if (!renderer.init(ctx, W, H, sh.c_str()) || !renderer.readback_init(ctx)) {
        std::fprintf(stderr, "[FAIL] renderer\n");
        return 1;
    }

    std::printf("=== focus_layer_smoke (#372) ===  frame %u (%ux%u)\n", F, W, H);

    auto render_now = [&]() -> const uint8_t* {
        s->replay_invalidate();
        const FrameView* f2 = s->replay_seek(*rec, F);
        return f2 ? renderer.export_frame(ctx, *f2, nullptr, false) : nullptr;
    };
    auto render_to = [&](std::vector<uint8_t>& out) -> bool {
        const uint8_t* g = render_now();
        if (!g) return false;
        out.resize(N);
        std::memcpy(out.data(), g, N);
        return true;
    };

    // Píxeles donde el elemento REALMENTE aporta: base vs el render con el
    // elemento oculto. Lo que otro dibuje encima no entra (ahí ocultar no
    // cambia nada), así que el bbox no necesita estar libre de solapes.
    auto footprint = [&](const std::vector<uint8_t>& base,
                         const std::vector<uint8_t>& behind) {
        std::vector<size_t> px;
        for (size_t i = 0; i < N; i += 4)
            if (base[i] != behind[i] || base[i+1] != behind[i+1] ||
                base[i+2] != behind[i+2]) px.push_back(i);
        return px;
    };

    // Elige, por capa, el elemento con más celdas del tipo pedido (HD o VRAM):
    // más celdas = más huella = un check menos susceptible a un frame flaco.
    auto pick_by_layer = [&](const std::vector<SceneElement>& inv, int layer,
                             bool want_claimed) -> uint64_t {
        uint64_t best = 0; size_t best_n = 0;
        for (const auto& e : inv) {
            if ((int)e.layer != layer || !e.hash || e.hidden) continue;
            if ((bool)e.claimed != want_claimed) continue;
            size_t n = 0;
            for (const auto& e2 : inv)
                if (e2.hash == e.hash && (int)e2.layer == layer) ++n;
            if (n > best_n) { best_n = n; best = e.hash; }
        }
        return best;
    };

    std::vector<uint8_t> base, behind, dst, got;

    // Los tres checks de una capa. `base` tiene que estar rendereado sin foco.
    //
    // El `dst` de la fórmula se mide CON EL FOCO PUESTO, no del baseline: lo que
    // hay detrás también se atenúa (enfocar el Window atenúa A y B a la vez), y
    // con el dst del baseline la cuenta sólo cerraría cuando la capa de atrás
    // resultara ser la enfocada.
    auto check_layer = [&](int L, uint64_t hh, const char* what) -> bool {
        ayther::HiddenElement he{};
        he.hash = hh; he.layer = (uint8_t)L;

        renderer.set_focus_layer(-1);
        s->set_hidden_elements(&he, 1);
        if (!render_to(behind)) { std::fprintf(stderr, "[FAIL] render oculto\n"); return false; }
        s->set_hidden_elements(nullptr, 0);

        const std::vector<size_t> fp = footprint(base, behind);
        std::printf("\ncapa %d (%s) %s hash=0x%016llx — huella %zu px\n",
                    L, kLayerName[L], what, (unsigned long long)hh, fp.size());
        if (fp.empty()) { check(false, "el elemento aporta pixeles visibles"); return true; }

        // 1. enfocar SU capa → byte-exacto
        renderer.set_focus_layer(L);
        if (!render_to(got)) { std::fprintf(stderr, "[FAIL] render foco\n"); return false; }
        {
            size_t diff = 0;
            for (size_t i : fp)
                if (base[i] != got[i] || base[i+1] != got[i+1] || base[i+2] != got[i+2])
                    ++diff;
            char msg[192];
            std::snprintf(msg, sizeof(msg),
                          "enfocada, %s de %s no se toca (%zu/%zu px distintos)",
                          what, kLayerName[L], diff, fp.size());
            check(diff == 0, msg);
        }

        // 2-3. enfocar OTRA capa → fórmula exacta (tinte Y opacidad)
        const int other = (L + 1) & 3;
        renderer.set_focus_layer(other);
        s->set_hidden_elements(&he, 1);
        if (!render_to(dst)) { std::fprintf(stderr, "[FAIL] render dst\n"); return false; }
        s->set_hidden_elements(nullptr, 0);
        if (!render_to(got)) { std::fprintf(stderr, "[FAIL] render foco otro\n"); return false; }
        {
            size_t bad = 0, opaque_would_fail = 0;
            for (size_t i : fp) {
                bool ok = true, valid_without_opacity = true;
                for (int ch = 0; ch < 3; ++ch) {
                    const float src = (float)base[i + ch];
                    const float exp = src * kTint * kOpac + (float)dst[i + ch] * (1.0f - kOpac);
                    if (std::fabs((float)got[i + ch] - exp) > 2.0f) ok = false;
                    if (std::fabs((float)got[i + ch] - src * kTint) > 2.0f)
                        valid_without_opacity = false;
                }
                if (!ok) ++bad;
                if (!valid_without_opacity) ++opaque_would_fail;
            }
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "con %s enfocada, %s de %s cumple src*0.25*0.749 + "
                          "dst*0.251 por pixel (%zu/%zu fuera de +-2)",
                          kLayerName[other], what, kLayerName[L], bad, fp.size());
            check(bad == 0, msg);
            std::snprintf(msg, sizeof(msg),
                          "y la OPACIDAD se nota: %zu/%zu px no coinciden con el "
                          "atenuado opaco (src*0.25)",
                          opaque_would_fail, fp.size());
            check(opaque_would_fail > 0, msg);
        }
        renderer.set_focus_layer(-1);
        return true;
    };

    // ── Parte 1: VRAM pura (HD apagado) — las 4 capas ───────────────────────
    s->set_hd_enabled(false);
    renderer.set_focus_layer(-1);
    if (!render_to(base)) { std::fprintf(stderr, "[FAIL] baseline VRAM\n"); return 1; }
    std::vector<SceneElement> inv;
    s->scene_inventory(inv);
    std::printf("\n--- Parte 1: quads VRAM (HD apagado) ---\n");

    for (int L = 0; L < 4; ++L) {
        const uint64_t hh = pick_by_layer(inv, L, false);
        if (!hh) {
            std::printf("\ncapa %d (%s): sin elemento propio en este frame — omitida\n",
                        L, kLayerName[L]);
            continue;
        }
        if (!check_layer(L, hh, "el elemento")) return 1;
    }

    // ── Parte 2: el atenuado alcanza al HD ──────────────────────────────────
    // El asset lo pone el smoke: un PNG OPACO de color plano asignado a un
    // sprite del frame. Así la Parte 2 no depende de que el proyecto del que
    // salió la grabación tenga assets autorados, y como es opaco vale la misma
    // fórmula que en VRAM (donde el asset pinta, `base` ES el src del quad).
    std::printf("\n--- Parte 2: lanes HD (asset propio del smoke) ---\n");
    s->set_hd_enabled(true);
    renderer.set_focus_layer(-1);
    if (!render_to(base)) { std::fprintf(stderr, "[FAIL] baseline HD\n"); return 1; }
    s->scene_inventory(inv);

    uint64_t spr = 0;
    for (const auto& e : inv)
        if (e.layer == 3 && e.hash && !e.hidden && !e.claimed) { spr = e.hash; break; }
    if (!spr) {
        // No-vacuidad: sin sprite al que asignarle un asset, la Parte 2 no
        // prueba nada — y un smoke que pasa sin medir es peor que uno rojo.
        check(false, "el frame tiene un sprite al que asignarle un asset "
                     "(si no, elegí otro frame)");
    } else {
        const std::string png = "focus_layer_smoke_asset.png";
        std::vector<uint8_t> rgba(64 * 64 * 4);
        for (size_t i = 0; i < rgba.size(); i += 4) {
            rgba[i+0] = 0xC0; rgba[i+1] = 0x40; rgba[i+2] = 0x20; rgba[i+3] = 0xFF;
        }
        if (!stbi_write_png(png.c_str(), 64, 64, 4, rgba.data(), 64 * 4)) {
            std::fprintf(stderr, "[FAIL] no se pudo escribir %s\n", png.c_str());
            return 1;
        }
        s->assign_sprite(spr, png);
        // El decode del asset es ASÍNCRONO: los primeros frames el quad se
        // saltea (la textura sigue en vuelo) y la huella saldría vacía.
        for (int tries = 0; tries < 120; ++tries) {
            if (!render_to(base)) { std::fprintf(stderr, "[FAIL] render HD\n"); return 1; }
            s->scene_inventory(inv);
            bool claimed = false;
            for (const auto& e : inv)
                if (e.hash == spr && e.layer == 3 && e.claimed) { claimed = true; break; }
            if (claimed) {
                // claimed no alcanza: el quad puede seguir sin textura. Se
                // espera a que el asset CAMBIE píxeles respecto del original.
                ayther::HiddenElement he{}; he.hash = spr; he.layer = 3;
                s->set_hidden_elements(&he, 1);
                if (!render_to(behind)) return 1;
                s->set_hidden_elements(nullptr, 0);
                if (!footprint(base, behind).empty()) break;
            }
        }
        if (!check_layer(3, spr, "el asset")) return 1;
        s->unassign_sprite(spr);
        std::remove(png.c_str());
    }

    // ── Parte 3: sin foco, byte-exacto ──────────────────────────────────────
    std::printf("\n--- Parte 3: restaurar ---\n");
    renderer.set_focus_layer(-1);
    if (!render_to(got)) { std::fprintf(stderr, "[FAIL] render final\n"); return 1; }
    check(std::memcmp(got.data(), base.data(), N) == 0,
          "sin foco (-1) el render vuelve byte-exacto al camino de siempre");

    vkDeviceWaitIdle(ctx.device());
    renderer.readback_shutdown(ctx);
    renderer.shutdown(ctx);
    ctx.shutdown();
    SDL_DestroyWindow(win);
    SDL_Quit();

    std::printf("\n%d checks, %d fails\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
