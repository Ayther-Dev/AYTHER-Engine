// ---------------------------------------------------------------------------
// group_hide_render_smoke (2026-07-31) — OCULTAR APAGA EL HD, EN PÍXELES.
//
// Los dos reportes que fija este oráculo, los dos del mismo mecanismo: un sub de
// sprite ANCLADO a un elemento de la escena lo despacha el pase de escena, en su
// z. Cuando el ancla se saltea (capa apagada o elemento oculto) el sub no se
// dibujaba ahí… pero la lane plana 4S lo re-dibujaba encima como si fuera un sub
// SIN ancla, y la pose con asset seguía viéndose.
//
//   1. Ojo de CAPA (header del timeline de Editar): con la capa Sprites apagada,
//      el frame con poses HD tiene que ser IDÉNTICO al mismo frame sin poses —
//      apagar la capa apaga también el HD que reemplaza a sus sprites.
//   2. Ojo del INSPECTOR: ocultar un miembro de la pose DESACTIVA el asset del
//      grupo, y eso en píxeles es exactamente lo mismo que no tener asset: el
//      frame con poses + un miembro oculto == el frame sin poses + ese miembro
//      oculto (los demás miembros vuelven a dibujar su original).
//   3. No vacuidad: sin nada oculto, con poses != sin poses (hay HD que apagar).
//
// Compara readbacks BGRA byte a byte al canvas nativo del emulador.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target group_hide_render_smoke (requiere GPU)
//   Args:  <poses.toml> <rec.arp> [f0] [f1]   (default barrido 0..600)
//   Env:   AYTHER_PROBE_ROM
// ---------------------------------------------------------------------------
#include "ayther_env.h"
#include "ayther_session.h"
#include "ayther_recording.h"
#include "ayther_renderer.h"
#include "ayther_layers.h"
#include "vulkan_backend/vk_context.h"
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
using SceneElement = ayther::AytherSession::SceneElement;
using PosePreview  = ayther::AytherSession::PosePreview;
using HE           = ayther::AytherSession::HiddenElement;

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

static std::string toml_quoted(const std::string& l) {
    const auto a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string() : l.substr(a + 1, b - a - 1);
}
static std::string resolve(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\') return p;
    return base + "/" + p;
}

struct PoseDef {
    std::string asset;
    std::vector<uint64_t> hashes;
    std::vector<std::pair<int16_t, int16_t>> rel, dims;
};
static std::vector<PoseDef> load_poses(const std::string& path) {
    std::vector<PoseDef> out;
    std::ifstream f(path);
    std::string line;
    PoseDef cur;
    auto flush = [&]() { if (!cur.hashes.empty()) out.push_back(cur); cur = PoseDef{}; };
    while (std::getline(f, line)) {
        if (line.find("[[pose]]") != std::string::npos) { flush(); continue; }
        auto starts = [&](const char* k) { return line.rfind(k, 0) == 0; };
        if (starts("default_asset")) cur.asset = toml_quoted(line);
        else if (starts("hashes")) {
            const std::string v = toml_quoted(line);
            for (size_t p = 0; p < v.size(); ) {
                size_t e = v.find('|', p);
                if (e == std::string::npos) e = v.size();
                cur.hashes.push_back(std::strtoull(v.substr(p, e - p).c_str(), nullptr, 16));
                p = e + 1;
            }
        } else if (starts("rel") || starts("dims")) {
            auto& dst = starts("rel") ? cur.rel : cur.dims;
            const std::string v = toml_quoted(line);
            for (size_t p = 0; p < v.size(); ) {
                size_t e = v.find('|', p);
                if (e == std::string::npos) e = v.size();
                const std::string it = v.substr(p, e - p);
                const size_t cma = it.find(',');
                if (cma != std::string::npos)
                    dst.push_back({ (int16_t)std::atoi(it.substr(0, cma).c_str()),
                                    (int16_t)std::atoi(it.substr(cma + 1).c_str()) });
                p = e + 1;
            }
        }
    }
    flush();
    return out;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "uso: group_hide_render_smoke <poses.toml> <rec.arp> [f0] [f1]\n");
        return 2;
    }
    const std::string poses_path = argv[1], rec_path = argv[2];
    const uint32_t f0 = argc > 3 ? (uint32_t)std::strtoul(argv[3], nullptr, 10) : 0u;
    const uint32_t f1 = argc > 4 ? (uint32_t)std::strtoul(argv[4], nullptr, 10) : 600u;

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

    std::vector<PosePreview> pvs;
    std::vector<std::string> assets;
    for (const auto& p : load_poses(poses_path)) {
        if (p.asset.empty() || p.rel.size() != p.hashes.size()) continue;
        PosePreview pv;
        pv.hashes = p.hashes;
        pv.asset  = p.asset;
        pv.hd     = true;
        for (auto& rl : p.rel) { pv.rel_x.push_back(rl.first); pv.rel_y.push_back(rl.second); }
        if (p.dims.size() == p.hashes.size())
            for (auto& d : p.dims) { pv.dim_w.push_back(d.first); pv.dim_h.push_back(d.second); }
        assets.push_back(p.asset);
        pvs.push_back(std::move(pv));
    }
    if (pvs.empty()) { std::fprintf(stderr, "[FAIL] sin poses con asset\n"); return 1; }

    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "[FAIL] SDL_Init\n"); return 1; }
    SDL_Window* win = SDL_CreateWindow("group_hide_render_smoke", 64, 64,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!win) { std::fprintf(stderr, "[FAIL] SDL_CreateWindow\n"); return 1; }
    VkContext ctx;
    if (!ctx.init(win)) { std::fprintf(stderr, "[FAIL] VkContext::init\n"); return 1; }

    s->set_pose_preview(pvs);
    const FrameView* fv0 = s->replay_seek(*rec, f0);
    if (!fv0 || !fv0->fb_width) { std::fprintf(stderr, "[FAIL] seek inicial\n"); return 1; }
    const uint32_t W = fv0->fb_width, H = fv0->fb_height;

    ayther::AytherRenderer renderer;
    const std::string sh = root + "/shaders/";
    if (!renderer.init(ctx, W, H, sh.c_str())) {
        std::fprintf(stderr, "[FAIL] AytherRenderer::init\n"); return 1;
    }
    if (!renderer.readback_init(ctx)) { std::fprintf(stderr, "[FAIL] readback\n"); return 1; }
    // El decode de assets es ASÍNCRONO: sin prewarm el primer render se saltea el
    // quad y el oráculo compara frames a medio calentar (flaky). Prewarm + un
    // puñado de renders (pump_uploads tiene presupuesto por frame) antes de medir.
    for (const std::string& a : assets) { renderer.prewarm_sprite(a, 0); renderer.prewarm_sprite(a, 1); }

    std::printf("=== group_hide_render_smoke ===  canvas %ux%u · poses %zu\n\n", W, H, pvs.size());

    // Stack por defecto y stack con la capa VDP "Sprites" apagada (== el ojo del
    // header del timeline: los dos entran por el mismo AND de scene_vis).
    AytherLayerStack stack_full, stack_no_spr;
    for (const AytherLayer& l : stack_no_spr.layers())
        if (l.kind == AytherLayerKind::VdpSprites) stack_no_spr.set_visible(l.id, false);

    const size_t bytes = (size_t)W * H * 4;
    auto shot = [&](uint32_t f, const AytherLayerStack& st, std::vector<uint8_t>& dst) -> bool {
        // Los canales de poses/ocultos son PRODUCE-ONLY: sin invalidar, el seek
        // devuelve el frame cacheado y el cambio de canal no se ve (el doble
        // produce es el patrón de element_hidden_smoke — converge al segundo).
        s->replay_invalidate();
        s->replay_seek(*rec, f);
        s->replay_invalidate();
        const FrameView* fv = s->replay_seek(*rec, f);
        if (!fv) return false;
        const uint8_t* px = nullptr;
        for (int i = 0; i < 24; ++i) {   // warmup: deja subir las texturas
            px = renderer.export_frame(ctx, *fv, nullptr, true, &st);
            if (!px) return false;
        }
        dst.assign(px, px + bytes);
        return true;
    };
    auto diff = [&](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        size_t n = 0;
        for (size_t i = 0; i < a.size() && i < b.size(); i += 4)
            if (std::memcmp(&a[i], &b[i], 4) != 0) ++n;
        return n;
    };

    // ---- elegir un frame con UNA pose anclada y >=2 elementos reclamados ----
    // Una sola pose: el check 2 apaga ESE asset, así que otro HD vivo en el
    // frame haría ruido. Todos los subs con ancla: el check 1 compara contra el
    // frame sin poses (un sub sin ancla se dibujaría en la lane 4S igual).
    uint32_t F = 0;
    uint64_t member_hash = 0;
    for (uint32_t f = f0; f <= f1 && !F; ++f) {
        s->replay_invalidate();
        s->replay_seek(*rec, f);
        s->replay_invalidate();
        const FrameView* fv = s->replay_seek(*rec, f);
        if (!fv || fv->sprite_sub_count != 1) continue;
        std::vector<SceneElement> inv;
        s->scene_inventory(inv);
        size_t claimed_n = 0, anchors = 0;
        uint64_t member = 0;
        for (const SceneElement& e : inv) {
            if (e.layer != 3 || !e.claimed) continue;
            ++claimed_n;
            if (e.sub >= 0) ++anchors;
            else if (!member) member = e.hash;
        }
        if (anchors == 1 && claimed_n >= 2 && member) { F = f; member_hash = member; }
    }
    if (!F) {
        check(false, "hay un frame con UNA pose anclada y >=2 elementos reclamados");
        std::printf("\n%d checks, %d fails\n", g_checks, g_fails);
        return 1;
    }
    std::printf("frame %u · miembro a ocultar 0x%016llx\n", F,
                (unsigned long long)member_hash);

    // El decode de la textura del asset es ASÍNCRONO y el prewarm sólo lo ENCOLA:
    // hasta que el sub queda Ready el quad HD se saltea, y comparar ahí mide la
    // supresión de los originales creyendo que mide el HD (así se coló un check
    // vacuo en la primera versión de este oráculo). Renderizar hasta Ready antes
    // de medir — pump_uploads sube de a poco por frame.
    {
        bool ready = false;
        for (int i = 0; i < 400 && !ready; ++i) {
            s->replay_invalidate(); s->replay_seek(*rec, F);
            s->replay_invalidate();
            const FrameView* fvd = s->replay_seek(*rec, F);
            if (!fvd) break;
            renderer.export_frame(ctx, *fvd, nullptr, true, &stack_full);
            std::vector<SceneElement> iv;
            s->scene_inventory(iv);
            for (const SceneElement& e : iv)
                if (e.layer == 3 && e.sub >= 0 &&
                    renderer.sub_texture_state(*fvd, e) == VkSprite::TexState::Ready)
                    ready = true;
        }
        std::printf("  textura del asset lista: %s\n\n", ready ? "sí" : "NO (test vacuo)");
        check(ready, "la textura del asset HD llegó a Ready (el HD se dibuja de verdad)");
    }

    std::vector<uint8_t> hd_on_full, hd_off_full, hd_on_nospr, hd_off_nospr,
                         hd_on_hidden, hd_off_hidden;
    bool ok = true;
    s->set_hidden_elements(nullptr, 0);
    s->set_pose_preview(pvs);
    ok &= shot(F, stack_full,   hd_on_full);
    ok &= shot(F, stack_no_spr, hd_on_nospr);
    s->set_pose_preview({});
    ok &= shot(F, stack_full,   hd_off_full);
    ok &= shot(F, stack_no_spr, hd_off_nospr);
    if (!ok) { check(false, "readbacks"); std::printf("\n%d checks, %d fails\n", g_checks, g_fails); return 1; }

    // ---- 3. no vacuidad: hay HD visible que apagar -------------------------
    {
        const size_t d = diff(hd_on_full, hd_off_full);
        std::printf("  (con poses vs sin poses, todo visible: %zu px distintos)\n", d);
        check(d > 0, "hay un HD de pose realmente visible en el frame (test no vacuo)");
    }

    // ---- 1. el ojo de CAPA apaga también el HD -----------------------------
    {
        const size_t d = diff(hd_on_nospr, hd_off_nospr);
        std::printf("  (capa Sprites apagada, con poses vs sin poses: %zu px distintos)\n", d);
        check(d == 0, "capa Sprites apagada: el HD de la pose se apaga con ella");
        // Que apagar la capa CAMBIE el frame: si no, el caso no se está
        // ejercitando y el check de arriba pasaría por vacío.
        const size_t d2 = diff(hd_on_full, hd_on_nospr);
        std::printf("  (dx capa Sprites ON vs OFF, con poses: %zu px)\n", d2);
        check(d2 > 0, "apagar la capa Sprites cambia el frame (el caso se ejercita)");
    }

    // ---- 2. el ojo del INSPECTOR desactiva el asset del grupo ---------------
    {
        HE h[1] = { { member_hash, 3 } };
        s->set_pose_preview(pvs);
        s->set_hidden_elements(h, 1);
        ok &= shot(F, stack_full, hd_on_hidden);
        s->set_pose_preview({});
        ok &= shot(F, stack_full, hd_off_hidden);
        s->set_hidden_elements(nullptr, 0);
        if (!ok) { check(false, "readbacks del ojo del Inspector"); }
        else {
            const size_t d = diff(hd_on_hidden, hd_off_hidden);
            std::printf("  (un miembro oculto, con poses vs sin poses: %zu px distintos)\n", d);
            check(d == 0, "ocultar un miembro desactiva el asset (== no tener asset)");
            const size_t d2 = diff(hd_on_hidden, hd_on_full);
            check(d2 > 0, "y el frame CAMBIA respecto de la pose intacta (el ojo se nota)");
        }
    }

    renderer.readback_shutdown(ctx);
    renderer.shutdown(ctx);
    ctx.shutdown();
    SDL_DestroyWindow(win);
    SDL_Quit();
    std::printf("\n%d checks, %d fails\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
