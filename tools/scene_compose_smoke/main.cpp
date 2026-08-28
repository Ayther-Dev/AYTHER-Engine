// ---------------------------------------------------------------------------
// scene_compose_smoke (#274) — el renderer compone la escena SIN el blit y el
// resultado es el frame del emulador.
//
// Con scene compose ON (set_scene_compose), AytherRenderer reemplaza el blit
// del framebuffer por quads del pipeline indexado alimentados por la escena
// (fv.scene, R-3): backdrop + celdas de plano + sprites, en el orden del
// inventario. Este smoke renderiza frames reales de una toma OFFSCREEN a las
// dimensiones nativas del emulador (hd_on=false → el modo Original nuevo:
// «todos los elementos sin asset») y compara el readback BGRA byte a byte
// contra el framebuffer del emulador del mismo frame (RGB565 expandido con la
// MISMA conversión de color del pipeline).
//
// El presupuesto es el de R-1: en gameplay de GA la recomposición single-state
// es ~píxel-exacta, así que acá se exige 0 en los frames elegidos.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target scene_compose_smoke (requiere GPU)
//   Args:  <rec.arp> [frame ...]   (default: 300 900 1500)
//   Env:   AYTHER_PROBE_ROM
// ---------------------------------------------------------------------------
#include "ayther_env.h"
#include "ayther_session.h"
#include "ayther_recording.h"
#include "ayther_renderer.h"
#include "vulkan_backend/vk_context.h"
#include <SDL3/SDL.h>
#include <stb_image_write.h>

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
static bool g_saw_clean = false;   // algún frame probado compuso (dirty=0)
static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

// RGB565 del emulador → BGRA con la expansión del pipeline (replicación).
static void expand565_bgra(uint16_t p, uint8_t* d) {
    const unsigned r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
    d[0] = (uint8_t)((b << 3) | (b >> 2));
    d[1] = (uint8_t)((g << 2) | (g >> 4));
    d[2] = (uint8_t)((r << 3) | (r >> 2));
    d[3] = 0xFF;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: scene_compose_smoke <rec.arp> [frame ...]\n");
        return 2;
    }
    const std::string rec_path = argv[1];
    std::vector<uint32_t> frames;
    for (int i = 2; i < argc; ++i)
        frames.push_back((uint32_t)std::strtoul(argv[i], nullptr, 10));
    if (frames.empty()) frames = { 300, 900, 1500 };

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
    std::unique_ptr<ayther::AytherSession>& s = *r;   // la escena se publica siempre (R-5 2b)
    auto rec = ayther::AytherRecording::load(rec_path);
    if (!rec) { std::fprintf(stderr, "[FAIL] no se pudo cargar %s\n", rec_path.c_str()); return 1; }

    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "[FAIL] SDL_Init\n"); return 1; }
    SDL_Window* win = SDL_CreateWindow("scene_compose_smoke", 64, 64,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!win) { std::fprintf(stderr, "[FAIL] SDL_CreateWindow\n"); return 1; }
    VkContext ctx;
    if (!ctx.init(win)) { std::fprintf(stderr, "[FAIL] VkContext::init\n"); return 1; }

    const FrameView* fv0 = s->replay_seek(*rec, frames[0]);
    if (!fv0 || !fv0->fb_width) { std::fprintf(stderr, "[FAIL] seek inicial\n"); return 1; }
    uint32_t W = fv0->fb_width, H = fv0->fb_height;

    ayther::AytherRenderer renderer;
    const std::string sh = root + "/shaders/";
    if (!renderer.init(ctx, W, H, sh.c_str())) {
        std::fprintf(stderr, "[FAIL] AytherRenderer::init\n");
        return 1;
    }
    if (!renderer.readback_init(ctx)) { std::fprintf(stderr, "[FAIL] readback\n"); return 1; }

    std::printf("=== scene_compose_smoke (#274) ===  canvas %ux%u (1:1 con el emu)\n\n", W, H);

    std::vector<uint8_t> expected;
    for (uint32_t f : frames) {
        const FrameView* fv = s->replay_seek(*rec, f);
        if (!fv || !fv->fb_pixels) { std::printf("f%u:\n", f); check(false, "seek"); continue; }
        if (fv->fb_width != W || fv->fb_height != H) {
            renderer.readback_shutdown(ctx);
            W = fv->fb_width; H = fv->fb_height;
            if (!renderer.resize(ctx, W, H) || !renderer.readback_init(ctx)) {
                std::printf("f%u:\n", f);
                check(false, "re-armado a la resolución nueva");
                continue;
            }
            std::printf("(canvas → %ux%u)\n", W, H);
        }
        if (!fv->scene || !fv->scene_count) {
            std::printf("f%u:\n", f);
            check(false, "la sesión publica la escena (scene compose ON)");
            continue;
        }

        std::printf("f%u: dirty=%u (raster %u · dim %u · band %u)\n", f,
                    fv->scene_dirty, fv->scene_dirty & 1,
                    (fv->scene_dirty >> 1) & 1, (fv->scene_dirty >> 2) & 1);
        if (!fv->scene_dirty) g_saw_clean = true;

        // hd_on=false → el Original nuevo: todos los elementos, sin assets.
        const uint8_t* got = renderer.export_frame(ctx, *fv, nullptr, false);
        if (!got) { std::printf("f%u:\n", f); check(false, "export_frame"); continue; }

        expected.resize((size_t)W * H * 4);
        const uint8_t* src = static_cast<const uint8_t*>(fv->fb_pixels);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const uint16_t p = *reinterpret_cast<const uint16_t*>(
                    src + (size_t)y * fv->fb_pitch + (size_t)x * 2);
                expand565_bgra(p, &expected[((size_t)y * W + x) * 4]);
            }

        size_t mismatch = 0;
        for (size_t i = 0; i < (size_t)W * H * 4; ++i) mismatch += (got[i] != expected[i]);
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "f%u: compose sin blit == frame del emulador (%zu bytes distintos)",
                      f, mismatch);
        check(mismatch == 0, msg);
        if (mismatch) {
            int shown = 0;
            for (uint32_t y = 0; y < H && shown < 12; ++y)
                for (uint32_t x = 0; x < W && shown < 12; ++x) {
                    const size_t i = ((size_t)y * W + x) * 4;
                    if (got[i] != expected[i] || got[i + 1] != expected[i + 1] ||
                        got[i + 2] != expected[i + 2]) {
                        std::printf("        diff (%3u,%3u): got %02X%02X%02X want %02X%02X%02X\n",
                                    x, y, got[i + 2], got[i + 1], got[i],
                                    expected[i + 2], expected[i + 1], expected[i]);
                        ++shown;
                    }
                }
            std::vector<uint8_t> png((size_t)W * H * 4);
            for (size_t i = 0; i < (size_t)W * H; ++i) {
                png[i * 4 + 0] = got[i * 4 + 2];
                png[i * 4 + 1] = got[i * 4 + 1];
                png[i * 4 + 2] = got[i * 4 + 0];
                png[i * 4 + 3] = 0xFF;
            }
            char name[64];
            std::snprintf(name, sizeof(name), "scene_compose_f%u_got.png", f);
            stbi_write_png(name, (int)W, (int)H, 4, png.data(), (int)W * 4);
            for (size_t i = 0; i < (size_t)W * H; ++i) {
                png[i * 4 + 0] = expected[i * 4 + 2];
                png[i * 4 + 1] = expected[i * 4 + 1];
                png[i * 4 + 2] = expected[i * 4 + 0];
                png[i * 4 + 3] = 0xFF;
            }
            std::snprintf(name, sizeof(name), "scene_compose_f%u_want.png", f);
            stbi_write_png(name, (int)W, (int)H, 4, png.data(), (int)W * 4);
            std::printf("        (dump: scene_compose_f%u_{got,want}.png)\n", f);
        }
    }

    // ── Ocultado por ELEMENTO a nivel de PIXEL (R-5: los canales murieron —
    // esta es LA prueba del criterio de región, ahora en el compose): ocultar
    // un gráfico de plano B cambia píxeles SOLO dentro de sus celdas.
    if (g_saw_clean) {
        uint32_t f = frames[0];
        const FrameView* fv = s->replay_seek(*rec, f);
        if (fv && !fv->scene_dirty && fv->scene_count) {
            std::vector<uint8_t> base((size_t)W * H * 4);
            const uint8_t* got0 = renderer.export_frame(ctx, *fv, nullptr, false);
            if (got0) std::memcpy(base.data(), got0, base.size());

            // Elegir un gráfico de B con todas sus celdas y juntar sus rects.
            uint64_t hh = 0;
            std::vector<std::array<int, 4>> rects;
            for (uint32_t i = 0; i < fv->scene_count && !hh; ++i)
                if (fv->scene[i].layer == 0 && fv->scene[i].x >= 8 &&
                    fv->scene[i].y >= 8) hh = fv->scene[i].hash;
            for (uint32_t i = 0; i < fv->scene_count; ++i)
                if (fv->scene[i].hash == hh && fv->scene[i].layer <= 2)
                    rects.push_back({ fv->scene[i].x, fv->scene[i].y,
                                      fv->scene[i].x + 8, fv->scene[i].y + 8 });
            ayther::HiddenElement he{ hh, 0 };
            s->set_hidden_elements(&he, 1);
            s->replay_invalidate();
            fv = s->replay_seek(*rec, f);
            const uint8_t* got1 = fv ? renderer.export_frame(ctx, *fv, nullptr, false)
                                     : nullptr;
            size_t inside = 0, outside = 0;
            if (got0 && got1)
                for (uint32_t y = 0; y < H; ++y)
                    for (uint32_t x = 0; x < W; ++x) {
                        const size_t i = ((size_t)y * W + x) * 4;
                        if (base[i] == got1[i] && base[i + 1] == got1[i + 1] &&
                            base[i + 2] == got1[i + 2]) continue;
                        bool in = false;
                        for (const auto& r : rects)
                            in |= (int)x >= r[0] && (int)x < r[2] &&
                                  (int)y >= r[1] && (int)y < r[3];
                        (in ? inside : outside)++;
                    }
            std::printf("\nocultado por elemento (hash 0x%016llx, %zu celdas): "
                        "%zu px dentro · %zu fuera\n",
                        (unsigned long long)hh, rects.size(), inside, outside);
            check(inside > 0, "ocultar el elemento cambia píxeles DENTRO de sus celdas (compose)");
            check(outside == 0, "y NO cambia nada FUERA (criterio de región, sin canales)");
            s->set_hidden_elements(nullptr, 0);
        }
    }

    // ── Salud del HÍBRIDO: barrido de la toma midiendo qué fracción de
    // frames es SUCIA (cae al blit). Si el gameplay normal saliera sucio, el
    // compose nunca correría y el híbrido sería un always-blit disfrazado.
    {
        uint32_t total = 0, dirty = 0, raster = 0, band = 0;
        for (uint32_t f = 0; f < rec->frame_count(); f += 10) {
            const FrameView* fv = s->replay_seek(*rec, f);
            if (!fv) continue;
            ++total;
            if (fv->scene_dirty) ++dirty;
            if (fv->scene_dirty & 1) ++raster;
            if (fv->scene_dirty & 4) ++band;
        }
        std::printf("\nhíbrido: %u/%u frames muestreados caen al blit (%.1f%%) — "
                    "raster %u · band de scroll %u; el resto compone\n",
                    dirty, total, total ? 100.0 * dirty / total : 0.0,
                    raster, band);
        // El assert de "no es always-blit" sólo vale donde ALGÚN frame probado
        // compuso (GA compone todo; un juego de parallax por band cae al blit
        // legítimamente hasta que R-7 dibuje strips por línea — y su salida
        // igual es exacta, que es lo que afirman los checks por frame).
        if (g_saw_clean)
            check(total > 0 && dirty < total,
                  "hay frames limpios: el compose corre (no es always-blit)");
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
