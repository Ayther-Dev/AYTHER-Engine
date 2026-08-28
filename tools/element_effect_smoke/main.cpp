// ---------------------------------------------------------------------------
// element_effect_smoke (#275) — los EFECTOS son una propiedad del elemento y
// se verifican POR PÍXEL en el compose (GPU), sin UI.
//
// Sobre un frame real de GA (compone al 100%, R-5):
//   1. TINTE 2× rojo en un elemento de plano B → los píxeles de sus celdas
//      cambian EXACTAMENTE según la fórmula del shader (c' = clamp(c·t/64),
//      tolerancia ±1 por redondeo UNORM) y NADA cambia fuera.
//   2. OPACIDAD 128 → cambia dentro (se transluce lo de atrás), 0 px fuera.
//   3. SILUETA → aparecen píxeles del color de silueta en el anillo expandido
//      (~2px) alrededor de las celdas; nada cambia lejos del anillo.
//   4. limpiar → el render vuelve byte-exacto al baseline.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target element_effect_smoke (requiere GPU)
//   Args:  <rec.arp> [frame]   (default 900)
//   Env:   AYTHER_PROBE_ROM
// ---------------------------------------------------------------------------
#include "ayther_env.h"
#include "ayther_session.h"
#include "ayther_recording.h"
#include "ayther_renderer.h"
#include "vulkan_backend/vk_context.h"
#include <SDL3/SDL.h>

#include <array>
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

using Rect = std::array<int, 4>;
static bool in_rects(const std::vector<Rect>& rs, int x, int y, int margin = 0) {
    for (const Rect& r : rs)
        if (x >= r[0] - margin && x < r[2] + margin &&
            y >= r[1] - margin && y < r[3] + margin) return true;
    return false;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: element_effect_smoke <rec.arp> [frame]\n");
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
    SDL_Window* win = SDL_CreateWindow("element_effect_smoke", 64, 64,
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

    ayther::AytherRenderer renderer;
    const std::string sh = root + "/shaders/";
    if (!renderer.init(ctx, W, H, sh.c_str()) || !renderer.readback_init(ctx)) {
        std::fprintf(stderr, "[FAIL] renderer\n");
        return 1;
    }

    std::printf("=== element_effect_smoke (#275) ===  frame %u (%ux%u)\n\n", F, W, H);

    auto render_now = [&]() -> const uint8_t* {
        s->replay_invalidate();
        const FrameView* f2 = s->replay_seek(*rec, F);
        return f2 ? renderer.export_frame(ctx, *f2, nullptr, false) : nullptr;
    };

    // Baseline + elemento de plano B con todas sus celdas centrales.
    std::vector<uint8_t> base((size_t)W * H * 4);
    {
        const uint8_t* g = render_now();
        if (!g) { std::fprintf(stderr, "[FAIL] baseline\n"); return 1; }
        std::memcpy(base.data(), g, base.size());
    }
    std::vector<ayther::SceneElement> inv;
    s->scene_inventory(inv);
    uint64_t hh = 0;
    std::vector<Rect> rects;
    {
        // elegir un hash de B cuyas apariciones (en A/B/W) sean todas centrales
        for (const auto& e : inv) {
            if (e.layer != 0) continue;
            bool ok = true;
            std::vector<Rect> rs;
            for (const auto& e2 : inv)
                if (e2.hash == e.hash && e2.layer <= 2) {
                    ok &= e2.x >= 16 && e2.y >= 16 &&
                          e2.x + 8 <= (int)W - 16 && e2.y + 8 <= (int)H - 16;
                    rs.push_back({ e2.x, e2.y, e2.x + 8, e2.y + 8 });
                }
            if (ok && rs.size() >= 1 && rs.size() <= 16) { hh = e.hash; rects = rs; break; }
        }
    }
    std::printf("elemento B hash=0x%016llx (%zu celdas)\n\n", (unsigned long long)hh,
                rects.size());
    if (!hh) { check(false, "hay un elemento de plano B central"); return 1; }

    auto diff_split = [&](const uint8_t* got, int margin, size_t& in, size_t& out) {
        in = out = 0;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const size_t i = ((size_t)y * W + x) * 4;
                if (base[i] == got[i] && base[i + 1] == got[i + 1] &&
                    base[i + 2] == got[i + 2]) continue;
                (in_rects(rects, (int)x, (int)y, margin) ? in : out)++;
            }
    };

    // ── 1. TINTE 2× rojo — fórmula exacta del shader (±1 por redondeo) ──────
    ayther::ElementEffect fx{};
    fx.hash = hh; fx.layer = 0;
    fx.tint[0] = 128; fx.tint[1] = 64; fx.tint[2] = 64;   // R ×2
    s->set_element_effects(&fx, 1);
    {
        const uint8_t* got = render_now();
        size_t in = 0, out = 0;
        diff_split(got, 0, in, out);
        check(in > 0, "el tinte cambia píxeles DENTRO de las celdas del elemento");
        check(out == 0, "y NO cambia nada FUERA");
        // Fórmula: BGRA → R' = clamp(R*2), G' = G, B' = B (±1) — SOLO sobre
        // los píxeles que cambiaron: dentro de la celda puede haber píxeles de
        // OTROS elementos dibujados encima (un sprite sobre el plano B), que
        // el tinte de B legítimamente no toca.
        size_t bad = 0, npx = 0;
        for (const Rect& rr : rects)
            for (int y = rr[1]; y < rr[3]; ++y)
                for (int x = rr[0]; x < rr[2]; ++x) {
                    const size_t i = ((size_t)y * W + x) * 4;
                    if (base[i] == got[i] && base[i + 1] == got[i + 1] &&
                        base[i + 2] == got[i + 2]) continue;
                    ++npx;
                    const int er = std::min(255, (int)base[i + 2] * 2);
                    if (std::abs((int)got[i + 2] - er) > 1 ||
                        std::abs((int)got[i + 1] - (int)base[i + 1]) > 1 ||
                        std::abs((int)got[i + 0] - (int)base[i + 0]) > 1)
                        ++bad;
                }
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "la fórmula del shader se cumple por píxel (%zu/%zu fuera de ±1)",
                      bad, npx);
        check(bad == 0, msg);
    }

    // ── 2. OPACIDAD 128 — se transluce lo de atrás, confinado ───────────────
    fx.tint[0] = fx.tint[1] = fx.tint[2] = 64;
    fx.opacity = 128;
    s->set_element_effects(&fx, 1);
    {
        const uint8_t* got = render_now();
        size_t in = 0, out = 0;
        diff_split(got, 0, in, out);
        check(in > 0, "la opacidad 128 cambia píxeles dentro (transluce)");
        check(out == 0, "confinada a las celdas del elemento");
    }

    // ── 3. SILUETA — color plano en el anillo expandido ─────────────────────
    fx.opacity = 255;
    fx.outline = 0xFF00FF00u;   // verde opaco (AABBGGRR)
    s->set_element_effects(&fx, 1);
    {
        const uint8_t* got = render_now();
        size_t in = 0, out = 0;
        diff_split(got, 3, in, out);   // margen 3 px: el anillo es ~2 px
        check(out == 0, "la silueta queda dentro del anillo (~2px) del elemento");
        size_t ring_green = 0;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                if (in_rects(rects, (int)x, (int)y, 0) ||
                    !in_rects(rects, (int)x, (int)y, 3)) continue;
                const size_t i = ((size_t)y * W + x) * 4;
                if (got[i + 1] == 0xFF && got[i] == 0 && got[i + 2] == 0) ++ring_green;
            }
        char msg[96];
        std::snprintf(msg, sizeof(msg),
                      "hay silueta verde en el anillo (%zu px)", ring_green);
        check(ring_green > 0, msg);
    }

    // ── 4. limpiar → baseline exacto ────────────────────────────────────────
    s->set_element_effects(nullptr, 0);
    {
        const uint8_t* got = render_now();
        check(got && std::memcmp(got, base.data(), base.size()) == 0,
              "limpiar los efectos restaura el render byte-exacto");
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
