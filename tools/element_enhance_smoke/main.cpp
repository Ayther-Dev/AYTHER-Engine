// ---------------------------------------------------------------------------
// element_enhance_smoke (#493 / #496) — la MEJORA POR SOFTWARE es una
// propiedad del inventario por (capa, hash), como el ocultado y los efectos.
//
// Sobre un frame real de una toma (sin GPU: sólo sesión + inventario):
//   1. Marcar un elemento de PLANO (set_enhanced_elements) → el inventario
//      publica SceneElement.fx_enhance = 1 en ese (capa, hash) y en ningún
//      otro; el fb queda byte a byte intacto (es una propiedad, no un lienzo).
//   2. Ruteo por capa: marcar el hash como SPRITE (capa 3) no marca celdas de
//      plano con el mismo hash.
//   3. Elemento RECLAMADO por HD (assign_plane) → fx_enhance = 0: el asset
//      ganó. Al desasignar vuelve a 1.
//   4. Lista vacía → todo apagado; los smokes de hidden/effect no cambian.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target element_enhance_smoke
//   Args:  <rec.arp> [frame]   (default 900)
//   Env:   AYTHER_PROBE_ROM
// ---------------------------------------------------------------------------
#include "ayther_env.h"
#include "ayther_session.h"
#include "ayther_recording.h"

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
using EE = ayther::AytherSession::EnhancedElement;

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

static bool copy_fb(const FrameView* fv, std::vector<uint16_t>& out, int& w, int& h) {
    if (!fv || !fv->fb_pixels || !fv->fb_width) return false;
    w = (int)fv->fb_width; h = (int)fv->fb_height;
    out.resize((size_t)w * h);
    const uint8_t* src = static_cast<const uint8_t*>(fv->fb_pixels);
    for (int y = 0; y < h; ++y)
        std::memcpy(&out[(size_t)y * w], src + (size_t)y * fv->fb_pitch, (size_t)w * 2);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: element_enhance_smoke <rec.arp> [frame]\n");
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

    std::printf("=== element_enhance_smoke (#493) ===  frame %u\n\n", F);

    // Dos produces del mismo frame (patrón de element_hidden_smoke).
    auto produce = [&]() -> const FrameView* {
        s->replay_invalidate();
        s->replay_seek(*rec, F);
        s->replay_invalidate();
        return s->replay_seek(*rec, F);
    };

    const FrameView* fv = produce();
    std::vector<uint16_t> fb0;
    int W = 0, H = 0;
    if (!copy_fb(fv, fb0, W, H)) { std::fprintf(stderr, "[FAIL] seek base\n"); return 1; }
    std::vector<SceneElement> inv;
    s->scene_inventory(inv);

    // Un elemento de plano B no reclamado y un sprite.
    uint64_t plane_hash = 0, spr_hash = 0;
    for (const SceneElement& e : inv) {
        if (!plane_hash && e.layer == 0 && !e.claimed && !e.hidden && e.hash) plane_hash = e.hash;
        if (!spr_hash && e.layer == 3 && e.hash) spr_hash = e.hash;
    }
    std::printf("plano B hash=0x%016llx · sprite hash=0x%016llx\n\n",
                (unsigned long long)plane_hash, (unsigned long long)spr_hash);
    if (!plane_hash || !spr_hash) {
        check(false, "hay un elemento de plano y un sprite para el caso");
        std::printf("\n%d checks, %d fails\n", g_checks, g_fails);
        return 1;
    }
    {
        bool any = false;
        for (const SceneElement& e : inv) any |= e.fx_enhance != 0;
        check(!any, "baseline: sin marcas, ningún elemento lleva fx_enhance");
    }
    auto count_marks = [&](uint64_t h, size_t& same, size_t& others, size_t& spr, size_t& pln) {
        std::vector<SceneElement> inv2;
        s->scene_inventory(inv2);
        same = others = spr = pln = 0;
        for (const SceneElement& e : inv2) {
            if (!e.fx_enhance) continue;
            if (e.hash == h) { ++same; (e.layer == 3 ? spr : pln)++; }
            else ++others;
        }
    };

    // ---- 1. marcar el elemento de plano ------------------------------------
    EE mark1[1] = { { plane_hash, 0 } };   // capa 0 = Plano B
    s->set_enhanced_elements(mark1, 1);
    fv = produce();
    std::vector<uint16_t> fb1;
    if (!copy_fb(fv, fb1, W, H)) { check(false, "re-produce con marca"); return 1; }
    check(fb1 == fb0, "el fb queda INTACTO (la mejora es una propiedad, no un lienzo)");
    {
        size_t same, others, spr, pln;
        count_marks(plane_hash, same, others, spr, pln);
        check(same > 0 && pln == same, "el inventario marca fx_enhance=1 en el elemento de plano");
        check(others == 0, "y ningún otro elemento quedó marcado");
        // #503: la intensidad viaja con la marca; el default es 255.
        std::vector<SceneElement> invk;
        s->scene_inventory(invk);
        bool k255 = true;
        for (const SceneElement& e : invk) if (e.fx_enhance && e.fx_enhance_k != 255) k255 = false;
        check(k255, "k ausente en la marca = 255 en el inventario (#503)");
        EE markk[1] = { { plane_hash, 0, 77 } };
        s->set_enhanced_elements(markk, 1);
        fv = produce();
        s->scene_inventory(invk);
        bool k77 = false, kother = false;
        for (const SceneElement& e : invk) {
            if (!e.fx_enhance) continue;
            if (e.fx_enhance_k == 77) k77 = true; else kother = true;
        }
        check(k77 && !kother, "k = 77 llega a SceneElement.fx_enhance_k (#503)");
        s->set_enhanced_elements(mark1, 1);
        fv = produce();
    }

    // ---- 2. ruteo por capa -------------------------------------------------
    EE mark2[1] = { { spr_hash, 3 } };     // capa 3 = Sprite
    s->set_enhanced_elements(mark2, 1);
    produce();
    {
        size_t same, others, spr, pln;
        count_marks(spr_hash, same, others, spr, pln);
        check(spr > 0, "marcar el sprite marca sus occs");
        check(pln == 0, "y NO marca celdas de plano con el mismo hash (ruteo por capa)");
        size_t s2, o2, sp2, pl2;
        count_marks(plane_hash, s2, o2, sp2, pl2);
        check(s2 == 0, "reemplazar la lista quita la marca anterior del plano");
    }

    // ---- 3. reclamado por HD: el asset gana --------------------------------
    s->set_enhanced_elements(mark1, 1);
    s->assign_plane(plane_hash, root + "/tests/fixtures/no_existe.png");
    produce();
    {
        std::vector<SceneElement> inv2;
        s->scene_inventory(inv2);
        size_t claimed = 0, enhanced = 0;
        for (const SceneElement& e : inv2)
            if (e.layer == 0 && e.hash == plane_hash) { claimed += e.claimed; enhanced += e.fx_enhance; }
        check(claimed > 0, "assign_plane reclama el elemento (claimed=1)");
        check(enhanced == 0, "reclamado por HD → fx_enhance=0 (el asset ganó)");
    }
    s->unassign_plane(plane_hash);
    produce();
    {
        size_t same, others, spr, pln;
        count_marks(plane_hash, same, others, spr, pln);
        check(same > 0, "al desasignar el HD la mejora vuelve (fx_enhance=1)");
    }

    // ---- 4. limpiar --------------------------------------------------------
    s->set_enhanced_elements(nullptr, 0);
    produce();
    {
        std::vector<SceneElement> inv2;
        s->scene_inventory(inv2);
        bool any = false;
        for (const SceneElement& e : inv2) any |= e.fx_enhance != 0;
        check(!any, "lista vacía deja el inventario sin marcas");
    }

    std::printf("\n%d checks, %d fails\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
