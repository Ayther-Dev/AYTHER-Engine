// ---------------------------------------------------------------------------
// snapshot_mask_smoke (2026-08-06) — las CAPAS elegidas en el diálogo Snapshot
// llegan al PNG, verificado POR PÍXEL sobre un frame real (GPU, sin UI).
//
// El Snapshot dejaba elegir calidad, espacio y versión, pero no qué capas
// entran en la imagen. El parámetro nuevo es `vdp_mask` de
// AytherRenderer::export_frame: los mismos bits de los ojos del timeline
// (A=1 · B=2 · W=4 · Sprites=8) aplicados SOLO al render del readback.
//
// Lo que fija este smoke es el plumbing, que es lo que se rompe en silencio:
//
//   1. NEUTRALIDAD — 0x0F y 0xFF (y el default sin argumento) dan el MISMO
//      byte: el snapshot de siempre no cambió. Un default mal ruteado se caza
//      acá y no en la captura del usuario.
//   2. EFECTO — apagar una capa CAMBIA la imagen, y el cambio no es vacuo
//      (>0 píxeles distintos). Sin este check, un vdp_mask ignorado pasaría
//      los otros dos: todo daría byte-exacto y el smoke se vería verde.
//   3. ACUMULACIÓN — apagar dos capas cambia al menos tanto como apagar una
//      sola de ellas, y con mask=0 no queda NADA del VDP (las 4 capas apagadas
//      dan una imagen distinta de cualquier subconjunto que dibuje algo).
//
// Corre con el HD APAGADO (hd_on=false): las capas VDP son el término que la
// máscara filtra, y así el resultado no depende de qué assets estén horneados.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target snapshot_mask_smoke (requiere GPU)
//   Args:  <rec.arp> [frame]   (default 900)
//   Env:   AYTHER_PROBE_ROM
// ---------------------------------------------------------------------------
#include "ayther_env.h"
#include "ayther_session.h"
#include "ayther_recording.h"
#include "ayther_renderer.h"
#include "../../tests/support/vulkan_test_context.h"
#include <SDL3/SDL.h>

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

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: snapshot_mask_smoke <rec.arp> [frame]\n");
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
    SDL_Window* win = SDL_CreateWindow("snapshot_mask_smoke", 64, 64,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!win) { std::fprintf(stderr, "[FAIL] SDL_CreateWindow\n"); return 1; }
    VulkanTestContext ctx;
    if (!ctx.init(win)) { std::fprintf(stderr, "[FAIL] VulkanTestContext::init\n"); return 1; }

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

    std::printf("=== snapshot_mask_smoke ===  frame %u (%ux%u)\n", F, W, H);

    // Un render por máscara, con el frame re-producido igual en cada pasada
    // (replay_invalidate + seek): así la única variable es la máscara.
    auto render_mask = [&](std::vector<uint8_t>& out, uint8_t mask,
                           bool pass_mask = true) -> bool {
        s->replay_invalidate();
        const FrameView* f2 = s->replay_seek(*rec, F);
        if (!f2) return false;
        const uint8_t* g = pass_mask
            ? renderer.export_frame(ctx, *f2, nullptr, false, nullptr, mask)
            : renderer.export_frame(ctx, *f2, nullptr, false);   // firma vieja
        if (!g) return false;
        out.resize(N);
        std::memcpy(out.data(), g, N);
        return true;
    };
    auto diff_px = [&](const std::vector<uint8_t>& a,
                       const std::vector<uint8_t>& b) -> size_t {
        size_t n = 0;
        for (size_t i = 0; i < N; i += 4)
            if (a[i] != b[i] || a[i+1] != b[i+1] || a[i+2] != b[i+2]) ++n;
        return n;
    };

    std::vector<uint8_t> all, all_ff, dflt, no_spr, no_win, no_spr_win, none;
    const bool got =
        render_mask(all,        0x0F) && render_mask(all_ff, 0xFF) &&
        render_mask(dflt,       0x00, /*pass_mask=*/false) &&
        render_mask(no_spr,     0x0F & ~0x08) &&
        render_mask(no_win,     0x0F & ~0x04) &&
        render_mask(no_spr_win, 0x0F & ~0x0C) &&
        render_mask(none,       0x00);
    if (!got) { std::fprintf(stderr, "[FAIL] export_frame devolvio null\n"); return 1; }

    // 1. Neutralidad — lo que ya funcionaba no cambia.
    std::printf("neutralidad:\n");
    check(std::memcmp(all.data(), all_ff.data(), N) == 0,
          "0x0F == 0xFF byte-exacto (los bits altos no significan nada)");
    check(std::memcmp(all.data(), dflt.data(), N) == 0,
          "todas las capas == la firma SIN mascara (default 0xFF)");

    // 2. Efecto — y que NO sea vacuo. Un frame donde una capa no dibuja nada
    // haria pasar 'cambia' por casualidad: por eso se exige >0 y se informa.
    std::printf("efecto por capa:\n");
    const size_t d_spr = diff_px(all, no_spr);
    const size_t d_win = diff_px(all, no_win);
    const size_t d_two = diff_px(all, no_spr_win);
    const size_t d_non = diff_px(all, none);
    std::printf("    px distintos vs todas: sin Sprites=%zu  sin Window=%zu  "
                "sin ambas=%zu  sin nada=%zu  (de %zu)\n",
                d_spr, d_win, d_two, d_non, N / 4);
    check(d_spr > 0, "apagar Sprites CAMBIA la imagen (no vacuo)");
    check(d_non > 0, "mask=0 (sin capas) CAMBIA la imagen");

    // 3. Acumulacion — apagar dos no puede cambiar menos que apagar una de
    // ellas, y apagar todo es el maximo.
    std::printf("acumulacion:\n");
    check(d_two >= d_spr && d_two >= d_win,
          "sin Sprites+Window cambia >= que sin cada una por separado");
    check(d_non >= d_two, "sin ninguna capa es el cambio maximo");

    renderer.readback_shutdown(ctx);
    std::printf("\n%s  (%d checks, %d fallos)\n",
                g_fails ? "FAILED" : "PASS", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
