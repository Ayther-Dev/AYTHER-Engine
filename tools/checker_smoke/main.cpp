// ---------------------------------------------------------------------------
// checker_smoke (#277) — el modo UV checker responde «¿qué falta?» por píxel.
//
//   1. Sin autoría, con checker ON, una celda de plano B sin mapear se pinta
//      EXACTA donde el tile es opaco y deja ver el backdrop donde es
//      transparente (la silueta se respeta) — los 64 píxeles se predicen desde
//      la VRAM del inventario. Desde 2026-08-06 la CASILLA es 1 tile, el par de
//      colores es el de SU CAPA (paleta del UV checker de Valle) y la paridad
//      del tablero va por posición de PANTALLA: la fórmula se replica acá, así
//      que un cambio en el shader sin cambiar el smoke falla.
//   1b. CADA CAPA SU PAR: una celda de otra capa del mismo frame no comparte
//      NINGÚN color con la primera.
//   1c. ÍNDICE de la casilla (fila/columna DENTRO del elemento, «A00»): no se
//      imprime a tamaño nativo —sería ilegible y ensuciaría el color plano— y
//      sí a 4×. Las dos mitades importan: sin la primera, un shader que
//      imprimiera siempre pasaría igual y rompería los checks de color.
//   2. Anclaje AL ELEMENTO: en un sprite de ≥2 tiles de ancho, la casilla del
//      tile x=8 alterna respecto de la de x=0 (tablero de ajedrez).
//   3. Autorado pero el asset NO cargó: assign_plane a un PNG inexistente →
//      checker de ALERTA (rosa+negro) exacto en la celda, distinto del par de
//      su capa (el negative-cache de VkSprite es la señal; el primer render lo
//      puebla, el segundo lo pinta).
//   4. Con asset: assign_plane a un PNG real → la celda muestra el HD (azul),
//      cero píxeles de checker.
//   5. Cobertura numérica: claimed del inventario — 0 al inicio, sube con las
//      asignaciones, vuelve a 0 al desasignar.
//   6. Checker OFF + sin asignaciones → byte-idéntico al baseline.
//
//   Build: -DAYTHER_BUILD_SPIKE=ON → target checker_smoke (GPU)
//   Args:  <rec.arp>  [frame]   (default 900)
//   Env:   AYTHER_PROBE_ROM
// ---------------------------------------------------------------------------
#include "ayther_env.h"
#include "ayther_session.h"
#include "ayther_recording.h"
#include "ayther_renderer.h"
#include "../../tests/support/vulkan_test_context.h"
#include <SDL3/SDL.h>
#include <stb_image_write.h>

#include <cstdint>
#include <cmath>
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

// Paleta del UV checker (la de Valle, muestreada del PNG de referencia,
// 2026-08-06) en RGB. DEBEN matchear indexed_plane.frag.
// Slot: 0=Plano B · 1=Plano A · 2=Window · 3=Sprites · 4=ALERTA (no cargó).
// Cada CAPA lleva su propio par para leer de qué plano es lo que falta.
static const uint8_t kDark[5][3] = {
    {  11,  93, 121 },   // teal
    { 207,  44, 101 },   // rosa
    { 127, 127, 127 },   // gris
    {  40,  40,  40 },   // negro
    { 207,  44, 101 },   // rosa      (alerta)
};
static const uint8_t kLite[5][3] = {
    {  94, 214, 194 },   // turquesa
    { 252, 143, 116 },   // salmón
    { 214, 214, 214 },   // gris claro
    { 254, 237, 170 },   // amarillo
    {  40,  40,  40 },   // negro     (alerta)
};

// El color EXACTO que el shader escribe. La CASILLA es 1 tile (8 px) y su
// paridad va por posición en PANTALLA (`sc`,`sr` = casilla en la grilla de la
// pantalla) — no por el elemento: en los planos casi todo lo no autorado son
// celdas sueltas y con la paridad del elemento el tablero se veía de un solo
// color. El borde de la casilla va aclarado 15% hacia el blanco.
// Replicar la fórmula acá es a propósito — si el shader cambia, esto falla.
// (ex, ey) = coord ANCLADA AL ELEMENTO, que es la que decide el borde.
// Casilla de PANTALLA de un quad que arranca en (x, y) px nativos. El shader
// hace `floor(pc.x / pc.w + 0.5)` — con hscroll fino los planos NO caen en
// múltiplos de 8 (p.ej. y=23), así que dividir entero da otra paridad y todos
// los píxeles salen «errados». Replicar el redondeo es el punto.
static void screen_cell(int x, int y, int* sc, int* sr) {
    *sc = (int)std::floor((double)x / 8.0 + 0.5);
    *sr = (int)std::floor((double)y / 8.0 + 0.5);
}

// (El marco aclarado del borde de casilla se RETIRÓ el 2026-08-06: con la
// casilla = 1 tile, el propio damero ya delimita cada casilla por color y la
// línea no cumplía función.)
static void checker_rgb(int slot, int sc, int sr, uint8_t out[3]) {
    const bool     alt = ((sc + sr) & 1) != 0;
    const uint8_t* c   = alt ? kDark[slot] : kLite[slot];
    for (int i = 0; i < 3; ++i) out[i] = c[i];
}

static bool px_is(const uint8_t* p, const uint8_t c[3]) {
    return p[0] == c[0] && p[1] == c[1] && p[2] == c[2];
}

// El readback es BGRA y `rgb` viene en RGB. Tolerancia ±1: el aclarado del
// borde de tile pasa por float y la cuantización puede diferir en un nivel.
static bool px_near_rgb(const uint8_t* p, const uint8_t rgb[3], int tol = 1) {
    return std::abs((int)p[0] - (int)rgb[2]) <= tol &&
           std::abs((int)p[1] - (int)rgb[1]) <= tol &&
           std::abs((int)p[2] - (int)rgb[0]) <= tol;
}

// ¿El píxel es de checker, sea cual sea el slot? (para afirmar que NO lo es).
static bool px_any_checker(const uint8_t* p) {
    for (int slot = 0; slot < 5; ++slot)
        for (int e = 0; e < 2; ++e) {
            const uint8_t* c = e ? kDark[slot] : kLite[slot];
            const uint8_t  rgb[3] = { c[0], c[1], c[2] };
            if (px_near_rgb(p, rgb, 0)) return true;
        }
    return false;
}

// Índice de color del tile `pat` en (row, col) desde la VRAM cruda (vista de
// bus off^1, nibble alto = pixel par) — el MISMO unpack que upload_vram.
static uint8_t tile_idx(const uint8_t* vram, uint32_t pat, int row, int col) {
    const uint32_t off = pat * 32u + (uint32_t)row * 4u + (uint32_t)(col / 2);
    const uint8_t  b   = vram[off ^ 1u];
    return (col & 1) ? (b & 0x0F) : (b >> 4);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: checker_smoke <rec.arp> [frame]\n");
        return 2;
    }
    const std::string rec_path = argv[1];
    const uint32_t    frame    = argc > 2 ? (uint32_t)std::atoi(argv[2]) : 900u;

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

    // PNG real de 8×8 (azul puro) para el caso «con asset».
    const std::string real_png = root + "/build/checker_real_smoke.png";
    {
        std::vector<uint8_t> img(8 * 8 * 4);
        for (int i = 0; i < 64; ++i) {
            img[i * 4 + 0] = 0; img[i * 4 + 1] = 0;
            img[i * 4 + 2] = 255; img[i * 4 + 3] = 255;
        }
        stbi_write_png(real_png.c_str(), 8, 8, 4, img.data(), 32);
    }
    const std::string missing_png = root + "/build/checker_missing_smoke.png";
    std::remove(missing_png.c_str());   // debe NO existir

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;
    auto rec = ayther::AytherRecording::load(rec_path);
    if (!rec) { std::fprintf(stderr, "[FAIL] no se pudo cargar %s\n", rec_path.c_str()); return 1; }

    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "[FAIL] SDL_Init\n"); return 1; }
    SDL_Window* win = SDL_CreateWindow("checker_smoke", 64, 64,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!win) { std::fprintf(stderr, "[FAIL] SDL_CreateWindow\n"); return 1; }
    VulkanTestContext ctx;
    if (!ctx.init(win)) { std::fprintf(stderr, "[FAIL] VulkanTestContext::init\n"); return 1; }

    const FrameView* fv = s->replay_seek(*rec, frame);
    if (!fv || !fv->fb_width || fv->scene_dirty || !fv->scene_vram) {
        std::fprintf(stderr, "[FAIL] f%u sin escena limpia\n", frame);
        return 1;
    }
    const uint32_t W = fv->fb_width, H = fv->fb_height;

    ayther::AytherRenderer renderer;
    const std::string sh = root + "/shaders/";
    if (!renderer.init(ctx, W, H, sh.c_str()) || !renderer.readback_init(ctx)) {
        std::fprintf(stderr, "[FAIL] renderer\n");
        return 1;
    }

    auto render = [&](std::vector<uint8_t>& out) -> bool {
        const uint8_t* g = renderer.export_frame(ctx, *fv, nullptr, true, nullptr);
        if (!g) return false;
        out.assign(g, g + (size_t)W * H * 4);
        return true;
    };
    auto reseek = [&]() -> bool {
        s->replay_invalidate();
        fv = s->replay_seek(*rec, frame);
        return fv && fv->fb_width == W && !fv->scene_dirty;
    };

    // ---- Selección de celdas de control: plano B, pri 0, sin flips, sin
    //      NADA encima (ningún otro elemento intersecta su rect) y lejos de
    //      los bordes/left-blank. Tres celdas de hash distinto.
    std::vector<SceneElement> inv;
    s->scene_inventory(inv);
    auto overlapped = [&](const SceneElement& e) {
        for (const auto& o : inv) {
            if (&o == &e) continue;
            if (o.hidden) continue;
            if (e.x < o.x + o.w && o.x < e.x + e.w &&
                e.y < o.y + o.h && o.y < e.y + e.h) return true;
        }
        return false;
    };
    const SceneElement* cell[3] = { nullptr, nullptr, nullptr };
    for (const auto& e : inv) {
        if (e.layer != 0 || e.priority != 0 || e.flips || e.hidden || e.claimed)
            continue;
        if (e.x < 16 || e.y < 16 || e.x + 8 > (int)W - 16 || e.y + 8 > (int)H - 16)
            continue;
        if (overlapped(e)) continue;
        if (cell[0] && e.hash == cell[0]->hash) continue;
        if (cell[1] && e.hash == cell[1]->hash) continue;
        if (!cell[0]) cell[0] = &e;
        else if (!cell[1]) cell[1] = &e;
        else if (!cell[2]) { cell[2] = &e; break; }
    }
    if (!cell[2]) {
        check(false, "hay 3 celdas B de control (pri0, sin flips, despejadas)");
        return 1;
    }
    std::printf("[..] celdas de control B: (%d,%d) (%d,%d) (%d,%d) · f%u\n",
                cell[0]->x, cell[0]->y, cell[1]->x, cell[1]->y,
                cell[2]->x, cell[2]->y, frame);
    const SceneElement cA = *cell[0], cM = *cell[1], cR = *cell[2];

    // Backdrop en BGRA (misma conversión ÚNICA que la paleta subida).
    const uint32_t bd = VkIndexedPlane::genesis_color_rgba(fv->scene_backdrop);
    const uint8_t  bg[3] = { (uint8_t)((bd >> 16) & 0xFF),
                             (uint8_t)((bd >>  8) & 0xFF),
                             (uint8_t)((bd >>  0) & 0xFF) };

    std::vector<uint8_t> base, got;
    if (!render(base)) { std::fprintf(stderr, "[FAIL] render base\n"); return 1; }

    // ---- 1. Celda sin mapear = checker de SU CAPA, EXACTO por píxel --------
    renderer.set_checker(true);
    if (!render(got)) { std::fprintf(stderr, "[FAIL] render checker\n"); return 1; }
    {
        int bad = 0, opaque = 0;
        for (int row = 0; row < 8; ++row)
            for (int col = 0; col < 8; ++col) {
                const uint8_t* p =
                    &got[(((size_t)(cA.y + row)) * W + (cA.x + col)) * 4];
                const uint8_t idx = tile_idx(fv->scene_vram, cA.pattern, row, col);
                if (idx == 0) { if (!px_is(p, bg)) ++bad; continue; }
                ++opaque;
                // Celda de PLANO B → su par (teal/turquesa), lx=ly=0.
                uint8_t want[3];
                int sc, sr; screen_cell(cA.x, cA.y, &sc, &sr);
                checker_rgb((int)cA.layer, sc, sr, want);
                if (!px_near_rgb(p, want)) ++bad;
            }
        std::printf("[..] celda A-checker: %d opacos · %d errados (de 64)\n",
                    opaque, bad);
        check(opaque > 0 && bad == 0,
              "celda sin mapear: checker de SU CAPA exacto + silueta respetada");
    }

    // ---- 1b. CADA CAPA SU PAR (2026-08-06) ---------------------------------
    // El pedido que originó la paleta: «todas las capas no deben usar la misma
    // combinación de colores». Se afirma sobre una celda de OTRA capa del mismo
    // frame — no alcanza con que el color de una capa sea el esperado, hay que
    // ver que la de al lado sea DISTINTA. Si no hay celda de otra capa visible
    // en este frame se informa y no se falla (depende del contenido).
    {
        // OJO: acá no sirve `overlapped` (mira TODOS los elementos) — una celda
        // de Plano A tiene SIEMPRE una de Plano B debajo, así que descartaría
        // todas. Lo que importa es que nada dibuje ENCIMA: como el inventario
        // es back→front, sólo cuentan los POSTERIORES. Mismo criterio que el
        // check del sprite.
        const SceneElement* other = nullptr;
        for (size_t i = 0; i < inv.size() && !other; ++i) {
            const SceneElement& e = inv[i];
            if (e.layer == cA.layer || e.layer > 3 || e.hidden || e.claimed ||
                e.flips || e.priority != 0)
                continue;
            if (e.x < 16 || e.y < 16 || e.x + 8 > (int)W - 16 ||
                e.y + 8 > (int)H - 16) continue;
            bool covered = false;
            for (size_t j = i + 1; j < inv.size() && !covered; ++j) {
                const SceneElement& o = inv[j];
                if (o.hidden) continue;
                if (e.x < o.x + o.w && o.x < e.x + e.w &&
                    e.y < o.y + o.h && o.y < e.y + e.h) covered = true;
            }
            if (!covered) other = &e;
        }
        if (!other) {
            std::printf("  [..]  sin celda de otra capa despejada en f%u — el "
                        "par por capa queda afirmado por la tabla del shader\n",
                        frame);
        } else {
            int bad = 0, opaque = 0, matches_layer_a = 0;
            for (int row = 0; row < 8; ++row)
                for (int col = 0; col < 8; ++col) {
                    const uint8_t idx =
                        tile_idx(fv->scene_vram, other->pattern, row, col);
                    if (idx == 0) continue;
                    ++opaque;
                    const uint8_t* p =
                        &got[(((size_t)(other->y + row)) * W +
                              (other->x + col)) * 4];
                    uint8_t want[3], want_cA[3];
                    int osc, osr; screen_cell(other->x, other->y, &osc, &osr);
                    checker_rgb((int)other->layer, osc, osr, want);
                    // Mismo lugar, pero con el par de la OTRA capa: si el par
                    // fuera compartido, coincidiría.
                    checker_rgb((int)cA.layer, osc, osr, want_cA);
                    if (!px_near_rgb(p, want)) ++bad;
                    if (px_near_rgb(p, want_cA, 0)) ++matches_layer_a;
                }
            std::printf("[..] celda de capa %u: %d opacos · %d errados · %d px "
                        "iguales al par de la capa %u\n",
                        other->layer, opaque, bad, matches_layer_a, cA.layer);
            check(opaque > 0 && bad == 0 && matches_layer_a == 0,
                  "otra capa = OTRO par de colores (ninguno coincide)");
        }
    }

    // ---- 1c. ÍNDICE de la casilla (2026-08-06) -----------------------------
    // Cada casilla lleva su índice (fila/columna DENTRO del elemento) impreso
    // en el centro. Se dibuja sólo si la casilla se ve grande —`pc.w >= 24` px
    // de pantalla— para no ensuciar el color plano a tamaño nativo. Se afirman
    // las dos mitades: a 1× NO hay tinta, a 4× SÍ. Sin la primera, un shader
    // que imprimiera siempre pasaría igual y rompería los checks de color.
    {
        int tinta_1x = 0;
        for (int row = 0; row < 8; ++row)
            for (int col = 0; col < 8; ++col) {
                if (tile_idx(fv->scene_vram, cA.pattern, row, col) == 0) continue;
                const uint8_t* p =
                    &got[(((size_t)(cA.y + row)) * W + (cA.x + col)) * 4];
                // Tinta = blanco o negro puros (contraste), que no están en el
                // par de NINGUNA capa salvo el negro de Sprites/alerta — cA es
                // de Plano B (teal/turquesa), así que acá son inequívocos.
                if ((p[0] == 255 && p[1] == 255 && p[2] == 255) ||
                    (p[0] == 0 && p[1] == 0 && p[2] == 0)) ++tinta_1x;
            }

        // Mismo frame a 4×: la casilla mide 32 px de pantalla y el índice entra.
        std::vector<uint8_t> big;
        const uint32_t BW = W * 4, BH = H * 4;
        bool big_ok = false;
        if (renderer.resize(ctx, BW, BH) && renderer.readback_init(ctx)) {
            const uint8_t* g =
                renderer.export_frame(ctx, *fv, nullptr, true, nullptr);
            if (g) { big.assign(g, g + (size_t)BW * BH * 4); big_ok = true; }
        }
        int tinta_4x = 0;
        if (big_ok)
            for (uint32_t y = (uint32_t)cA.y * 4; y < (uint32_t)(cA.y + 8) * 4; ++y)
                for (uint32_t x = (uint32_t)cA.x * 4;
                     x < (uint32_t)(cA.x + 8) * 4; ++x) {
                    const uint8_t* p = &big[((size_t)y * BW + x) * 4];
                    if ((p[0] == 255 && p[1] == 255 && p[2] == 255) ||
                        (p[0] == 0 && p[1] == 0 && p[2] == 0)) ++tinta_4x;
                }
        std::printf("[..] indice: %d px de tinta a 1x · %d a 4x\n",
                    tinta_1x, tinta_4x);
        check(big_ok && tinta_1x == 0 && tinta_4x > 0,
              "el índice se imprime con resolución (4x) y NO a tamaño nativo");

        // Volver al canvas nativo para el resto de los checks.
        renderer.resize(ctx, W, H);
        renderer.readback_init(ctx);
        if (!render(got)) { std::fprintf(stderr, "[FAIL] re-render 1x\n"); return 1; }
    }

    // ---- 2. Anclaje al elemento: sprite ≥2 tiles de ancho ------------------
    // Un sprite SIEMPRE pisa celdas de plano (cubren toda la pantalla): lo que
    // importa es que nada dibuje ENCIMA — el inventario es back→front, así que
    // solo cuentan los elementos POSTERIORES al candidato.
    {
        const SceneElement* sp = nullptr;
        for (size_t i = 0; i < inv.size() && !sp; ++i) {
            const SceneElement& e = inv[i];
            if (e.layer != 3 || e.flips || e.hidden || e.claimed || e.w < 16)
                continue;
            if (e.x < 16 || e.y < 16 || e.x + e.w > (int)W - 16 ||
                e.y + e.h > (int)H - 16) continue;
            bool covered = false;
            for (size_t j = i + 1; j < inv.size() && !covered; ++j) {
                const SceneElement& o = inv[j];
                if (o.hidden) continue;
                if (e.x < o.x + o.w && o.x < e.x + e.w &&
                    e.y < o.y + o.h && o.y < e.y + e.h) covered = true;
            }
            if (!covered) sp = &e;
        }
        if (!sp) {
            std::printf("  [..]  sin sprite ancho despejado en f%u — el anclaje "
                        "lo afirma la fase (col/6) del checker de la celda\n",
                        frame);
        } else {
            // Tile visual (dc=1, dr=0) — column-major: pattern + 1*ht.
            const int ht = sp->h / 8;
            const uint16_t pat = (uint16_t)((sp->pattern + ht) & 0x7FF);
            int bad = 0, opaque = 0, distinct_samples = 0;
            for (int row = 0; row < 8; ++row)
                for (int col = 0; col < 8; ++col) {
                    const uint8_t idx = tile_idx(fv->scene_vram, pat, row, col);
                    if (idx == 0) continue;
                    ++opaque;
                    const uint8_t* p =
                        &got[(((size_t)(sp->y + row)) * W + (sp->x + 8 + col)) * 4];
                    // El par es el de SPRITES (amarillo/negro), distinto al de
                    // los planos; la casilla es la de PANTALLA (x+8).
                    uint8_t want[3];
                    int ssc, ssr; screen_cell(sp->x + 8, sp->y, &ssc, &ssr);
                    checker_rgb((int)sp->layer, ssc, ssr, want);
                    if (!px_near_rgb(p, want)) ++bad;
                    // Casillas contiguas ALTERNAN (tablero de ajedrez): la de
                    // x+8 tiene la paridad opuesta a la de x.
                    int p0c, p0r; screen_cell(sp->x, sp->y, &p0c, &p0r);
                    const bool alt  = ((ssc + ssr) & 1) != 0;
                    const bool alt0 = ((p0c + p0r) & 1) != 0;
                    if (alt != alt0) ++distinct_samples;
                }
            std::printf("[..] sprite (%d,%d) %dx%d: tile x=8 → %d opacos · %d "
                        "errados · %d px donde la fase difiere del sello suelto\n",
                        sp->x, sp->y, sp->w, sp->h, opaque, bad, distinct_samples);
            check(opaque > 0 && bad == 0 && distinct_samples > 0,
                  "checker ANCLADO al elemento (el tile x=8 continúa el patrón)");
        }
    }

    // ---- 3. Autorado pero NO cargó = checker de ALERTA (rosa+negro) --------
    s->assign_plane(cM.hash, missing_png);
    if (!reseek()) { std::fprintf(stderr, "[FAIL] reseek\n"); return 1; }
    if (!render(got) || !render(got)) {   // 1º puebla el negative-cache, 2º pinta
        std::fprintf(stderr, "[FAIL] render magenta\n"); return 1;
    }
    {
        int bad = 0, opaque = 0, same_as_layer = 0;
        for (int row = 0; row < 8; ++row)
            for (int col = 0; col < 8; ++col) {
                const uint8_t idx = tile_idx(fv->scene_vram, cM.pattern, row, col);
                if (idx == 0) continue;
                ++opaque;
                const uint8_t* p =
                    &got[(((size_t)(cM.y + row)) * W + (cM.x + col)) * 4];
                // ALERTA (slot 4): pisa el par de la capa — la misma celda de
                // Plano B que arriba salía teal/turquesa, acá va rosa/negro.
                uint8_t want[3], want_layer[3];
                int mc, mr; screen_cell(cM.x, cM.y, &mc, &mr);
                checker_rgb(4, mc, mr, want);
                checker_rgb((int)cM.layer, mc, mr, want_layer);
                if (!px_near_rgb(p, want)) ++bad;
                if (px_near_rgb(p, want_layer, 0)) ++same_as_layer;
            }
        std::printf("[..] celda M-checker: %d opacos · %d errados · %d px "
                    "iguales al par de su capa\n", opaque, bad, same_as_layer);
        check(opaque > 0 && bad == 0,
              "autorado pero el asset no cargó: checker de ALERTA exacto");
        check(same_as_layer == 0,
              "el par de alerta NO se confunde con el par de la capa");
    }

    // ---- 4. Con asset = HD normal, cero checker ----------------------------
    s->assign_plane(cR.hash, real_png);
    if (!reseek()) { std::fprintf(stderr, "[FAIL] reseek\n"); return 1; }
    {
        const SceneElement* er = nullptr;
        bool ready = false;
        for (int tries = 0; tries < 100 && !ready; ++tries) {
            if (!render(got)) break;
            std::vector<SceneElement> inv2;
            s->scene_inventory(inv2);
            er = nullptr;
            for (const auto& e : inv2)
                if (e.layer == 0 && e.x == cR.x && e.y == cR.y) { er = &e; break; }
            if (er && er->sub >= 0 &&
                renderer.sub_texture_state(*fv, *er) ==
                    VkSprite::TexState::Ready) {
                ready = true;
                if (!render(got)) break;   // un render más con la textura lista
            } else {
                SDL_Delay(10);
            }
        }
        int blue = 0, checkered = 0;
        for (int row = 0; row < 8; ++row)
            for (int col = 0; col < 8; ++col) {
                const uint8_t* p =
                    &got[(((size_t)(cR.y + row)) * W + (cR.x + col)) * 4];
                if (p[0] == 255 && p[1] == 0 && p[2] == 0) ++blue;
                if (px_any_checker(p)) ++checkered;
            }
        std::printf("[..] celda R: ready=%d · %d px azules (HD) · %d px de checker\n",
                    (int)ready, blue, checkered);
        check(ready && blue == 64 && checkered == 0,
              "con asset: el HD dibuja normal y la celda no lleva checker");
    }

    // ---- 5. Cobertura numérica ---------------------------------------------
    {
        std::vector<SceneElement> inv2;
        s->scene_inventory(inv2);
    uint32_t total = 0, claimed_count = 0;
        for (const auto& e : inv2)
        if (e.layer < 4) { ++total; if (e.claimed) ++claimed_count; }
    std::printf("[..] cobertura: %u/%u con asset\n", claimed_count, total);
    check(claimed_count > 0 && claimed_count < total,
              "la cobertura sube con las asignaciones (con_asset > 0)");

        s->unassign_plane(cM.hash);
        s->unassign_plane(cR.hash);
        if (!reseek()) { std::fprintf(stderr, "[FAIL] reseek\n"); return 1; }
        inv2.clear();
        s->scene_inventory(inv2);
    claimed_count = 0;
    for (const auto& e : inv2) if (e.layer < 4 && e.claimed) ++claimed_count;
    check(claimed_count == 0, "al desasignar la cobertura vuelve a 0");
    }

    // ---- 6. Checker OFF + sin asignaciones = baseline exacto ---------------
    renderer.set_checker(false);
    {
        std::vector<uint8_t> again;
        check(render(again) && again == base,
              "checker OFF vuelve al baseline byte-exacto");
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
