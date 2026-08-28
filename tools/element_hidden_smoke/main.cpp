// ---------------------------------------------------------------------------
// element_hidden_smoke (#273) — la visibilidad POR ELEMENTO es una propiedad
// del inventario, y ocultar no toca nada fuera del elemento.
//
// Sobre un frame real de una toma:
//   1. Ocultar un elemento de PLANO por hash (set_hidden_elements) cambia
//      píxeles SOLO dentro de las celdas de ese gráfico (criterio de #273:
//      «ocultar no afecta a los elementos de otra capa que caigan en la misma
//      región») y el inventario lo refleja en SceneElement.hidden.
//   2. Ocultar un SPRITE por el MISMO canal unificado produce byte a byte el
//      mismo framebuffer que el canal específico set_sprite_hidden (la unión
//      no pisa ni cambia la semántica), confinado a los rects de sus occs.
//   3. Limpiar restaura el frame original exacto.
//
// Aplica en el produce del mismo frame (sin latencia): cada paso es
// replay_invalidate() + replay_seek() (patrón de plane_hide_smoke).
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target element_hidden_smoke
//   Args:  <rec.arp> [frame]   (default 900)
//   Env:   AYTHER_PROBE_ROM
// ---------------------------------------------------------------------------
#include "ayther_env.h"
#include "ayther_session.h"
#include "ayther_recording.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif
using ayther::FrameView;
using SceneElement = ayther::AytherSession::SceneElement;

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

struct Rect { int x0, y0, x1, y1; };
static bool in_rects(const std::vector<Rect>& rs, int x, int y) {
    for (const Rect& r : rs)
        if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1) return true;
    return false;
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

// Diferencias contra la base: cuántas DENTRO de los rects y cuántas FUERA.
static void diff_split(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b,
                       int w, int h, const std::vector<Rect>& rects,
                       size_t& inside, size_t& outside) {
    inside = outside = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t)y * w + x;
            if (a[i] == b[i]) continue;
            (in_rects(rects, x, y) ? inside : outside)++;
        }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: element_hidden_smoke <rec.arp> [frame]\n");
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

    std::printf("=== element_hidden_smoke (#273) ===  frame %u\n\n", F);

    // Dos produces del MISMO frame: la máscara de tiles de plano se arma con
    // las occurrences de un produce y se aplica al siguiente (produce-only,
    // converge al re-producir — el Lab lo cubre con su settle de 2 frames;
    // mismo patrón que plane_hide_smoke). El punto de #273 es que el frame N
    // oculto SE VE como frame N — no que viaje al frame N+1 del playback.
    auto produce = [&]() -> const FrameView* {
        s->replay_invalidate();
        s->replay_seek(*rec, F);
        s->replay_invalidate();
        return s->replay_seek(*rec, F);
    };

    // ---- base + selección de elementos desde el inventario -----------------
    const FrameView* fv = produce();
    std::vector<uint16_t> fb0;
    int W = 0, H = 0;
    if (!copy_fb(fv, fb0, W, H)) { std::fprintf(stderr, "[FAIL] seek base\n"); return 1; }
    std::vector<SceneElement> inv;
    s->scene_inventory(inv);

    // Elemento de PLANO: los rects son TODAS las apariciones del gráfico en
    // los 3 planos (el canal 0x105 oculta el gráfico dondequiera que aparezca
    // en su plano, y el mismo hash puede vivir también en A/W). El hash se
    // elige con TODAS sus apariciones lejos del borde y 1..12 celdas, para que
    // «fuera de los rects» sea una afirmación exacta.
    std::unordered_map<uint64_t, std::vector<Rect>> plane_rects;
    std::unordered_map<uint64_t, bool> central;
    for (const SceneElement& e : inv) {
        if (e.layer > 2) continue;
        plane_rects[e.hash].push_back({ e.x, e.y, e.x + 8, e.y + 8 });
        const bool c = e.x >= 16 && e.y >= 16 && e.x + 8 <= W - 16 && e.y + 8 <= H - 16;
        auto it = central.find(e.hash);
        central[e.hash] = (it == central.end() ? true : it->second) && c;
    }
    uint64_t plane_hash = 0;
    for (const SceneElement& e : inv) {   // en orden del inventario (determinista)
        if (e.layer != 0) continue;
        const auto& rs = plane_rects[e.hash];
        if (central[e.hash] && rs.size() >= 1 && rs.size() <= 12) {
            plane_hash = e.hash;
            break;
        }
    }

    // Sprite: el primero del inventario (rects = sus occs, con margen 0).
    uint64_t spr_hash = 0;
    std::vector<Rect> spr_rects;
    for (const SceneElement& e : inv)
        if (e.layer == 3) {
            if (!spr_hash) spr_hash = e.hash;
            if (e.hash == spr_hash)
                spr_rects.push_back({ e.x, e.y, e.x + e.w, e.y + e.h });
        }
    std::printf("plano B hash=0x%016llx (%zu celdas) · sprite hash=0x%016llx (%zu occs)\n\n",
                (unsigned long long)plane_hash,
                plane_hash ? plane_rects[plane_hash].size() : 0,
                (unsigned long long)spr_hash, spr_rects.size());
    if (!plane_hash || !spr_hash) {
        check(false, "hay un elemento de plano y un sprite para el caso");
        std::printf("\n%d checks, %d fails\n", g_checks, g_fails);
        return 1;
    }

    // R-5 (#274): los canales de supresión MURIERON — el fb queda intacto
    // SIEMPRE (fuente de datos, no lienzo) y ocultar es una PROPIEDAD del
    // inventario que el compose aplica (el pixel-level, con su criterio de
    // región, lo valida scene_compose_smoke en GPU). Acá: marcado correcto,
    // ruteo por capa, equivalencia de canales y fb invariante.
    std::vector<uint16_t> fb1;

    // ---- 1. ocultar el elemento de plano -----------------------------------
    using HE = ayther::AytherSession::HiddenElement;
    HE hide1[1] = { { plane_hash, 0 } };   // capa 0 = Plano B
    s->set_hidden_elements(hide1, 1);
    fv = produce();
    if (!copy_fb(fv, fb1, W, H)) { check(false, "re-produce con plano oculto"); return 1; }
    check(fb1 == fb0, "el fb queda INTACTO (canal de supresión muerto)");
    {
        std::vector<SceneElement> inv2;
        s->scene_inventory(inv2);
        bool marked = false, others_clear = true;
        for (const SceneElement& e : inv2) {
            if (e.hash == plane_hash && e.layer == 0) marked |= e.hidden != 0;
            else if (e.hidden) others_clear = false;
        }
        check(marked, "el inventario marca hidden=1 en el elemento oculto");
        check(others_clear, "y ningún otro elemento quedó marcado");
    }

    // ---- 2. ruteo por capa + equivalencia de canales -----------------------
    // El MISMO gráfico puede existir como tiles de plano (mismo hash): el
    // ruteo por capa evita que ocultar el sprite marque las celdas.
    HE hide2[1] = { { spr_hash, 3 } };     // capa 3 = Sprite
    s->set_hidden_elements(hide2, 1);
    fv = produce();
    auto marks_of = [&](uint64_t h) {
        std::vector<SceneElement> inv2;
        s->scene_inventory(inv2);
        size_t spr = 0, pln = 0;
        for (const SceneElement& e : inv2)
            if (e.hash == h && e.hidden) (e.layer == 3 ? spr : pln)++;
        return std::make_pair(spr, pln);
    };
    auto [u_spr, u_pln] = marks_of(spr_hash);
    check(u_spr > 0, "ocultar el sprite (canal unificado) marca sus occs");
    check(u_pln == 0, "y NO marca celdas de plano con el mismo hash (ruteo por capa)");
    std::vector<uint16_t> fb2;
    copy_fb(fv, fb2, W, H);
    check(fb2 == fb0, "el fb sigue intacto con el sprite oculto");

    s->set_hidden_elements(nullptr, 0);
    uint64_t spr_only[1] = { spr_hash };
    s->set_sprite_hidden(spr_only, 1);
    fv = produce();
    auto [c_spr, c_pln] = marks_of(spr_hash);
    check(c_spr == u_spr && c_pln == 0,
          "canal unificado == set_sprite_hidden (mismo marcado)");
    s->set_sprite_hidden(nullptr, 0);

    // ---- 3. limpiar deja el inventario sin marcas --------------------------
    fv = produce();
    {
        std::vector<SceneElement> inv2;
        s->scene_inventory(inv2);
        bool any = false;
        for (const SceneElement& e : inv2) any |= e.hidden != 0;
        check(!any, "limpiar los ocultos deja el inventario sin marcas");
    }

    std::printf("\n%d checks, %d fails\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
