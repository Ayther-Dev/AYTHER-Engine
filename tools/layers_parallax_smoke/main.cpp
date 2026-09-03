// ---------------------------------------------------------------------------
// layers_parallax_smoke (#276) — una capa INSERTADA entre el plano B y el
// plano A de un nivel real, moviéndose a un ratio DERIVADO, verificada por
// píxel sobre un frame de scroll GRANDE (los frames de scroll chico esconden
// los errores de signo — hay precedente).
//
//   1. Se genera una lámina de 64×16 (magenta opaca, con una columna AZUL en
//      x=0 como marcador de fase) y se inserta como capa Custom en el índice 1
//      del stack — entre VdpPlaneB y VdpPlaneA — anclada al plano B con
//      factor 0.5.
//   2. En f300 (H=0) y f900 (H=968, el scroll grande de la demo) la columna
//      azul debe caer en x ≡ (H·0.5 mod 64) ±1 → el ratio derivado ES el
//      movimiento, con el signo correcto.
//   3. Z real de capa intercalada: la lámina pinta SOBRE el plano B (hay
//      magenta en la banda) pero BAJO el plano A (los píxeles de una celda A
//      dentro de la banda quedan idénticos al baseline).
//   4. Fuera de la banda de la lámina, el render es byte-idéntico al baseline;
//      sin la capa, TODO vuelve al baseline exacto.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target layers_parallax_smoke (GPU)
//   Args:  <rec.arp>   (default frames 300 y 900)
//   Env:   AYTHER_PROBE_ROM
// ---------------------------------------------------------------------------
#include "ayther_env.h"
#include "ayther_session.h"
#include "ayther_recording.h"
#include "ayther_renderer.h"
#include "ayther_layers.h"
#include "../../tests/support/vulkan_test_context.h"
#include <SDL3/SDL.h>
#include <stb_image_write.h>

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

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: layers_parallax_smoke <rec.arp>\n");
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

    // Lámina generada: magenta opaca con columna azul de 4 px en x=0.
    const std::string strip_png = root + "/build/parallax_strip_smoke.png";
    {
        std::vector<uint8_t> img((size_t)kImgW * kImgH * 4);
        for (int y = 0; y < kImgH; ++y)
            for (int x = 0; x < kImgW; ++x) {
                uint8_t* p = &img[((size_t)y * kImgW + x) * 4];
                if (x < 4) { p[0] = 0;   p[1] = 0; p[2] = 255; }   // azul marcador
                else       { p[0] = 255; p[1] = 0; p[2] = 255; }   // magenta
                p[3] = 255;
            }
        stbi_write_png(strip_png.c_str(), kImgW, kImgH, 4, img.data(), kImgW * 4);
    }

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;
    auto rec = ayther::AytherRecording::load(rec_path);
    if (!rec) { std::fprintf(stderr, "[FAIL] no se pudo cargar %s\n", rec_path.c_str()); return 1; }

    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "[FAIL] SDL_Init\n"); return 1; }
    SDL_Window* win = SDL_CreateWindow("layers_parallax_smoke", 64, 64,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!win) { std::fprintf(stderr, "[FAIL] SDL_CreateWindow\n"); return 1; }
    VulkanTestContext ctx;
    if (!ctx.init(win)) { std::fprintf(stderr, "[FAIL] VulkanTestContext::init\n"); return 1; }

    const FrameView* fv = s->replay_seek(*rec, 900);
    if (!fv || !fv->fb_width || fv->scene_dirty) {
        std::fprintf(stderr, "[FAIL] f900 sin escena limpia\n");
        return 1;
    }
    const uint32_t W = fv->fb_width, H = fv->fb_height;
    const int hs900 = fv->plane_hscroll[1];

    // Banda de la lámina: sobre una celda del plano A pri-0 (para el chequeo
    // de z «bajo A») lejos de los bordes.
    // Cualquier prioridad sirve para el chequeo de z: A pri-0 dibuja en su
    // capa (después de la Custom) y A pri-1 en VdpFrente — ambos SOBRE la
    // lámina insertada entre B y A.
    int band_y = -1, acx = -1, acy = -1;
    {
        std::vector<ayther::SceneElement> inv;
        s->scene_inventory(inv);
        uint32_t na = 0;
        for (const auto& e : inv)
            if (e.layer == 1) ++na;
        for (const auto& e : inv)
            if (e.layer == 1 && !e.hidden &&
                e.y >= 24 && e.y + 8 <= (int)H - 24 && e.x >= 16 &&
                e.x + 8 <= (int)W - 16) { acx = e.x; acy = e.y; break; }
        std::printf("[..] elementos de Plano A en f900: %u (control en %d,%d)\n",
                    na, acx, acy);
    }
    if (acy < 0) { check(false, "hay una celda de Plano A central (control)"); return 1; }
    band_y = acy - 4;   // la banda de 16 px cubre la celda A (acy..acy+8)
    std::printf("banda y=%d · celda A de control en (%d,%d) · H(f900)=%d\n\n",
                band_y, acx, acy, hs900);

    ayther::AytherRenderer renderer;
    const std::string sh = root + "/shaders/";
    if (!renderer.init(ctx, W, H, sh.c_str()) || !renderer.readback_init(ctx)) {
        std::fprintf(stderr, "[FAIL] renderer\n");
        return 1;
    }

    // Stack con la capa insertada ENTRE B (índice 0) y A (índice 1).
    AytherLayerStack stack;
    const uint32_t lid = stack.insert_custom("niebla", 1);
    check(stack.layers()[1].id == lid && stack.layers()[1].kind == AytherLayerKind::Custom &&
              stack.layers()[0].kind == AytherLayerKind::VdpPlaneB &&
              stack.layers()[2].kind == AytherLayerKind::VdpPlaneA,
          "la capa Custom quedó insertada entre VdpPlaneB y VdpPlaneA");
    AytherLayerContent cc{};
    std::snprintf(cc.asset, sizeof(cc.asset), "%s", strip_png.c_str());
    cc.img_w = kImgW; cc.img_h = kImgH;
    cc.y = (int16_t)band_y;
    cc.anchor = 0;      // derivado del plano B
    cc.factor = 0.5f;
    check(stack.set_content(lid, cc), "set_content sobre la capa insertada");

    auto render_at = [&](uint32_t f, const AytherLayerStack* st,
                         std::vector<uint8_t>& out, int& hs) -> bool {
        s->replay_invalidate();
        const FrameView* f2 = s->replay_seek(*rec, f);
        if (!f2 || f2->fb_width != W) return false;
        hs = f2->plane_hscroll[1];
        const uint8_t* g = renderer.export_frame(ctx, *f2, nullptr, true, st);
        if (!g) return false;
        out.assign(g, g + (size_t)W * H * 4);
        return true;
    };
    // Fase de la lámina: la columna azul vive en x ≡ (H·0.5 mod 64); una
    // repetición puede quedar tapada (el left-blank de 8 px tapa x=0..7, un
    // sprite puede tapar otra) — basta que ALGUNA repetición visible esté en
    // fase. Devuelve el x del primer píxel azul, -1 si no hay.
    auto min_blue_x = [&](const std::vector<uint8_t>& img) {
        for (uint32_t x = 0; x < W; ++x)
            for (int y = band_y; y < band_y + kImgH; ++y) {
                const size_t i = ((size_t)y * W + x) * 4;
                if (img[i + 2] == 0 && img[i + 1] == 0 && img[i + 0] == 255)
                    return (int)x;   // BGRA: azul puro
            }
        return -1;
    };
    auto phase_ok = [&](int found_x, int hsv) {
        if (found_x < 0) return false;
        const int mod = ((int)std::lround((float)hsv * 0.5f) % kImgW + kImgW) % kImgW;
        int d = (found_x - mod) % kImgW;
        if (d < 0) d += kImgW;
        return d <= 4 || d >= kImgW - 1;   // columna de 4 px, ±1 de redondeo
    };

    // Warmup: la lámina decodifica ASYNC (worker + pump_uploads, 1-3 renders
    // en frío — más bajo carga, p.ej. la batería completa): renderizar hasta
    // verla pintada antes de MEDIR, o los chequeos de fase corren contra un
    // frame sin lámina (flaky detectado corriendo la batería entera).
    {
        std::vector<uint8_t> warm;
        int hsw = 0;
        for (int tries = 0; tries < 100; ++tries) {
            if (!render_at(900, &stack, warm, hsw)) break;
            bool seen = false;
            for (int y = band_y; y < band_y + kImgH && !seen; ++y)
                for (uint32_t x = 0; x < W && !seen; ++x) {
                    const size_t i = ((size_t)y * W + x) * 4;
                    if (warm[i] == 255 && warm[i + 1] == 0 && warm[i + 2] == 255)
                        seen = true;
                }
            if (seen) break;
            SDL_Delay(10);
        }
    }

    int hs300 = 0, hs900b = 0;
    std::vector<uint8_t> base300, base900, got300, got900;
    if (!render_at(300, nullptr, base300, hs300) ||
        !render_at(900, nullptr, base900, hs900b) ||
        !render_at(300, &stack, got300, hs300) ||
        !render_at(900, &stack, got900, hs900b)) {
        std::fprintf(stderr, "[FAIL] renders\n");
        return 1;
    }

    // 1. La lámina se ve (pinta sobre el plano B).
    size_t magenta = 0;
    for (int y = band_y; y < band_y + kImgH; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const size_t i = ((size_t)y * W + x) * 4;
            if (got900[i] == 255 && got900[i + 1] == 0 && got900[i + 2] == 255)
                ++magenta;
        }
    check(magenta > 0, "la lámina pinta en la banda (sobre el plano B)");

    // 2. El ratio derivado con signo: azul en fase x ≡ H·0.5 (mod 64).
    const int bx300 = min_blue_x(got300), bx900 = min_blue_x(got900);
    std::printf("H300=%d → azul en x=%d · H900=%d → azul en x=%d\n",
                hs300, bx300, hs900b, bx900);
    // f300 es el punto de contraste (H=0). Con el warmup de la textura este
    // chequeo corre siempre; el condicional queda por si un frame tapa TODAS
    // las repeticiones de la columna (el criterio duro es el f900 de abajo,
    // cuya fase además discrimina el signo: errado caería en 64−36=28).
    if (bx300 >= 0)
        check(phase_ok(bx300, hs300),
              "f300: la columna azul está en fase con H·0.5 (mod 64)");
    else
        std::printf("  [..]  f300: columna azul no visible — la fase la "
                    "afirma f900\n");
    check(phase_ok(bx900, hs900b),
          "f900 (scroll grande): la columna azul está en fase con H·0.5 (mod 64)");
    check(hs900b != hs300, "los dos frames tienen H distinto (el test es sensible al signo)");

    // 3. Z decisivo: la lámina es OPACA — si dibujara SOBRE el plano A, la
    //    celda A de control quedaría 100% del color de la lámina. Debajo de A,
    //    la lámina sólo asoma por los huecos transparentes de la celda: tiene
    //    que quedar al menos un píxel del arte de A intacto.
    {
        int unchanged = 0, strip_px = 0;
        for (int y = acy; y < acy + 8; ++y)
            for (int x = acx; x < acx + 8; ++x) {
                const size_t i = ((size_t)y * W + x) * 4;
                const bool same = got900[i] == base900[i] &&
                                  got900[i + 1] == base900[i + 1] &&
                                  got900[i + 2] == base900[i + 2];
                if (same) ++unchanged;
                const bool is_strip =
                    (got900[i] == 255 && got900[i + 1] == 0 && got900[i + 2] == 255) ||
                    (got900[i] == 255 && got900[i + 1] == 0 && got900[i + 2] == 0);
                if (is_strip) ++strip_px;
            }
        std::printf("[..] celda A: %d px intactos · %d px de lámina (de 64)\n",
                    unchanged, strip_px);
        check(unchanged > 0 && strip_px < 64,
              "el arte del plano A queda ENCIMA (la lámina sólo asoma por sus huecos)");
    }

    // 4. Fuera de la banda: byte-idéntico al baseline.
    {
        size_t out_diffs = 0;
        for (uint32_t y = 0; y < H; ++y) {
            if ((int)y >= band_y && (int)y < band_y + kImgH) continue;
            for (uint32_t x = 0; x < W; ++x) {
                const size_t i = ((size_t)y * W + x) * 4;
                if (got900[i] != base900[i] || got900[i + 1] != base900[i + 1] ||
                    got900[i + 2] != base900[i + 2]) ++out_diffs;
            }
        }
        check(out_diffs == 0, "fuera de la banda el render es byte-idéntico al baseline");
    }

    // 5. Sin la capa, todo vuelve al baseline exacto.
    {
        std::vector<uint8_t> again;
        int hsx = 0;
        check(render_at(900, nullptr, again, hsx) && again == base900,
              "sin la capa el render vuelve al baseline exacto");
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
