// ---------------------------------------------------------------------------
// widescreen_spike — ¿cuánto fondo REAL hay fuera de la ventana 4:3? (EM-8 #231)
//
// El motor ya recorre la nametable ENTERA y descarta lo que cae fuera de la
// pantalla con dos `continue` (ayther_session.cpp, clip sx>=sw / sy>=sh). O sea
// que ensanchar a 16:9 no necesita un lector nuevo: necesita saber si lo que
// hay en las columnas laterales es ARTE DEL NIVEL o BASURA. La nametable
// wrapea cada wpx (512 px = 64 celdas en Sonic 2) contra 40 columnas visibles:
// una columna lateral puede contener arte del MISMO nivel desplazado 512 px —
// basura visualmente plausible que un test de "hay tile no vacío" no detecta.
//
// Este spike NO toca el render. Sólo mide, sobre el juego real:
//
//   VERDAD DE NIVEL — recorriendo la nametable con la MISMA geometría del
//   motor, toda celda que cae bien adentro de la pantalla (con margen contra
//   el borde de streaming) registra su código en (plano, columna_de_nivel,
//   fila_de_nametable). La columna de nivel sale de un unwrap del scroll por
//   ENTRADA de la tabla Hscroll, así que el parallax por bandas del plano B
//   no la corrompe. Una celda que a lo largo de la toma muestra más de un
//   código es ANIMADA y queda excluida del veredicto.
//
//   SONDAS — la misma pasada clasifica las celdas que caen en las columnas
//   laterales (k = 1..16 por lado) en VÁLIDA (coincide con la verdad) ·
//   RANCIA (no coincide: arte de otro tramo del nivel) · VACÍA (patrón 0) ·
//   DESCONOCIDA (esa posición de nivel nunca se vio). Se resuelven al FINAL
//   de la toma para que el lado líder tenga chance: sus columnas se vuelven
//   verdad unos frames después.
//
//   VEREDICTO — la RACHA CONTIGUA de columnas válidas desde el borde hacia
//   afuera, que es lo único que se puede dibujar sin agujeros. Se contrasta
//   contra lo que pide 16:9 (+5 columnas por lado con píxel cuadrado, +7 si
//   se preserva la relación de la imagen mostrada 4:3).
//
// Tres tomas: DERECHA, IZQUIERDA (asimetría: el lado líder sólo tiene el
// lookahead del streamer, el trasero conserva el rastro) y QUIETO (control
// negativo: sin scroll no hay evidencia — justifica el gate de EM-8.2).
//
//   Build:  -DAYTHER_BUILD_SPIKE=ON  →  target widescreen_spike
//   Run:    bin/widescreen_spike [core.dll rom]
//
//   Requiere un juego cuyo plano scrollee ENTERO: el H whole-plane se muestrea
//   en la línea 0, y Aladdin/Golden Axe llevan HUD de plano fijo ahí (señal 0).
//   Sonic 2 es el corpus (el mismo del stitcher y de cam_track_smoke).
// ---------------------------------------------------------------------------
#include "ayther_session.h"
#include "ayther_env.h"

#include <stb_image_write.h>   // impl linkeada desde el engine (tile_tex_cache)

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifndef AYTHER_SOURCE_DIR
#define AYTHER_SOURCE_DIR "."
#endif

using ayther::FrameView;

// ── Config ──────────────────────────────────────────────────────────────────
static constexpr int kProbeCols = 16;   // columnas laterales sondeadas por lado
// Frames por toma. Parametrizable porque la cobertura de la TIRA depende de
// cuánto nivel se recorrió: una toma corta deja huecos que no son del método
// sino de que el jugador no llegó hasta ahí. Ver el volcado `_tira`.
static int kMeasure = 360;
static constexpr int kEdgeGuard = 3;    // columnas del borde excluidas de la verdad
                                        // (el juego streamea la entrante 1-2 antes)

// 16:9 sobre 224 px de alto: con píxel cuadrado son 398 px (+39 por lado);
// preservando la relación de la imagen MOSTRADA (4:3 sobre 320×224), 426 px
// (+53 por lado).
static constexpr int kNeedSquare = 5;   // celdas por lado
static constexpr int kNeedPar    = 7;

// ── Helpers de config (mismo patrón que cam_track_smoke) ────────────────────
static std::string quoted(const std::string& l) {
    auto a = l.find('"'), b = l.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string() : l.substr(a + 1, b - a - 1);
}
static std::string resolve_path(const std::string& p, const std::string& base) {
    if (p.empty() || (p.size() > 1 && p[1] == ':') || p[0] == '/' || p[0] == '\\') return p;
    return base + "/" + p;
}

// División entera hacia abajo (las posiciones de nivel pueden ser negativas).
static inline int fdiv8(int v) { return (v >= 0) ? (v >> 3) : -(((-v) + 7) >> 3); }

// ── Volcado visual del widescreen crudo (#231) ──────────────────────────────
//
// Los porcentajes de abajo dicen CUÁNTO arte lateral hay; esto muestra QUÉ
// arte es. La diferencia importa: «8 celdas válidas» puede ser una pared de
// ladrillos que continúa perfecto o el borde de una plataforma cortada al
// medio, y eso no se decide leyendo una tabla.
//
// El centro sale de `AytherSession::recompose_layers()` (E-7 #406) — el frame
// REAL del emulador, no una reconstrucción. Los laterales se rasterizan acá
// desde VRAM+CRAM, y su color es una APROXIMACIÓN: se convierte el 3-3-3 del
// VDP linealmente a 8 bits y no se aplica shadow/highlight, así que un lateral
// puede verse un punto más plano que el centro. Para juzgar «¿esto es arte del
// nivel o basura?» alcanza; para juzgar color, no.
//
// Las celdas que la medición marcó RANCIAS (arte de otro tramo del nivel) se
// tiñen de magenta y las DESCONOCIDAS de gris: sin eso, una captura donde todo
// «se ve bien» esconde justo el caso que hace fracasar la feature.
struct WideCell { uint16_t code = 0; uint8_t verdict = 0; };  // 0=ok 1=rancia 2=descon.

static void rgb_from_cram(uint16_t c, uint8_t* out) {
    // Layout GPX: 0000 BBB0 GGG0 RRR0
    const unsigned r = (c >> 1) & 7, g = (c >> 5) & 7, b = (c >> 9) & 7;
    out[0] = (uint8_t)((r * 255) / 7);
    out[1] = (uint8_t)((g * 255) / 7);
    out[2] = (uint8_t)((b * 255) / 7);
}

/// Rasteriza una celda de nametable (8×8, 4bpp) en `dst` (RGB888, stride px).
static void draw_cell(uint8_t* dst, int stride_px, const uint8_t* vram, size_t vsz,
                      const uint8_t* cram, uint16_t code, uint8_t verdict) {
    const uint32_t pat  = (uint32_t)(code & 0x7FF) << 5;
    const bool     hf   = (code >> 11) & 1, vf = (code >> 12) & 1;
    const unsigned pal  = (code >> 13) & 3;
    for (int y = 0; y < 8; ++y) {
        const int sy = vf ? 7 - y : y;
        for (int x = 0; x < 8; ++x) {
            const int sx = hf ? 7 - x : x;
            const uint32_t off = pat + (uint32_t)sy * 4 + (uint32_t)(sx >> 1);
            if (off >= vsz) continue;
            const uint8_t byte = vram[off];
            const unsigned idx = (sx & 1) ? (byte & 0x0F) : (byte >> 4);
            uint8_t* p = dst + ((size_t)y * stride_px + x) * 3;
            if (!idx) { p[0] = p[1] = p[2] = 0; }      // transparente → backdrop negro
            else {
                const size_t ci = (pal * 16 + idx) * 2;
                const uint16_t c = (uint16_t)(cram[ci] | (cram[ci + 1] << 8));
                rgb_from_cram(c, p);
            }
            // El veredicto se TIÑE, no se pinta encima: se sigue viendo el arte
            // debajo, que es lo que hay que juzgar.
            if (verdict == 1) { p[0] = (uint8_t)(128 + p[0] / 2); p[2] = (uint8_t)(128 + p[2] / 2); p[1] /= 2; }
            else if (verdict == 2) { const int g = (p[0] + p[1] + p[2]) / 3; p[0] = p[1] = p[2] = (uint8_t)(g / 2 + 40); }
        }
    }
}

// ── Estado de una toma ──────────────────────────────────────────────────────
struct TruthCell {
    uint16_t code     = 0;
    int32_t  first    = -1;      // frame en que esa posición de nivel se vio en pantalla
    bool     animated = false;   // vio más de un código ⇒ fuera del veredicto
};

// Sonda pendiente de resolver contra la verdad final.
struct Probe {
    uint32_t slot;    // ((frame*2 + plano)*2 + lado)*kProbeCols + (k-1)
    int32_t  lvx;     // columna de nivel
    int32_t  lvy;     // fila de nivel (des-enrollada del V, NO la de nametable)
    uint16_t code;
    uint8_t  plane;
    uint8_t  cy;      // fila de nametable — sólo para el desglose de huecos
};

struct Bucket { int valid = 0, stale = 0, empty = 0, unknown = 0; };

/// Una celda LATERAL del frame elegido para el volcado visual, con su posición
/// en píxeles con signo respecto de la ventana (no en celdas: el scroll no está
/// alineado a 8 y redondear movería el arte medio tile).
struct SnapCell {
    int32_t  sxs;    ///< x respecto del borde izquierdo de la ventana (<0 = izquierda)
    int32_t  sy;
    int32_t  lvx;    ///< columna de nivel, para clasificar contra la verdad
    int32_t  lvy;    ///< fila de nivel (des-enrollada del V)
    uint16_t code;
    uint8_t  plane, cy;
};

struct Take {
    std::string          name;
    int                  frames    = 0;
    int                  scrolled  = 0;    // frames con delta de scroll ≠ 0 (plano A)
    int                  hmax      = 0;    // máximo H crudo visto (¿supera 10 bits/512?)
    std::map<std::tuple<uint8_t,int32_t,int32_t>, TruthCell> truth;
    std::vector<Probe>   probes;
    std::vector<Bucket>  agg;              // por slot
    int32_t              cam_span  = 0;    // px recorridos (plano A, línea 0)
    int                  px0 = 0, px1 = 0; // X del jugador al empezar/terminar (Sonic 2)
    // Control causal del signo del mapeo (k=1, plano A): de las sondas
    // VÁLIDAS, ¿cuántas corresponden a nivel que se ve DESPUÉS (lookahead del
    // lado líder) y cuántas a nivel que ya se vio ANTES (rastro del trasero)?
    long lead_future = 0, lead_valid = 0, trail_past = 0, trail_valid = 0;

    // -- Volcado visual (#231): un frame congelado con todo lo necesario ------
    // Se toma a MITAD de la toma y no al final: las columnas del lado líder se
    // vuelven verdad recién unos frames después, así que un snapshot del último
    // frame saldría con todo el lateral pintado de «desconocido» — un artefacto
    // del método, no del juego.
    /// Filas de nametable que la ventana vertical mostró has_any vez, por plano.
    uint8_t visible_row[2][32] = {};
    /// Cámara vertical des-enrollada (px de nivel) y su valor previo, por plano.
    int max_band_count[2] = {0, 0};
    long band_count_sum[2] = {0, 0};
    long band_frame_count[2] = {0, 0};
    int32_t camv[2] = {0, 0};
    int32_t prev_v[2] = {0, 0};
    std::vector<uint16_t> snap_fb;             ///< composite del frame (RGB565)
    std::vector<uint8_t>  snap_vram, snap_cram;
    std::vector<SnapCell> snap_cells;
    int snap_w = 0, snap_h = 0, snap_frame = -1;
};

// ── Medición de una toma ────────────────────────────────────────────────────
//
// Recorre la nametable de A y B con la MISMA geometría que produce_frame:
// H por entrada de la tabla Hscroll (indexada por línea de pantalla), V global
// de la columna 0 (VSRAM no está expuesta en AytherSession; la refinación por
// 2-cell sólo aplica a columnas visibles y no cambia la identidad horizontal).
//
// La cámara se lleva POR ENTRADA de la tabla Hscroll: así el parallax por
// bandas del plano B no mezcla posiciones de nivel de distintas filas.
static void measure(ayther::AytherSession& s, uint16_t input, int frames,
                    Take& t, const char* name) {
    t.name   = name;
    t.frames = frames;
    t.px0    = (int)s.ram_u16(0xB008);   // X del jugador (Sonic 2): detecta muerte/respawn

    // cam[plano][idx de la tabla Hscroll]  — unwrap acumulado
    static constexpr int kH = 256;
    std::vector<int32_t> cam(2 * kH, 0);
    std::vector<int32_t> prev(2 * kH, 0);
    bool first = true;

    for (int f = 0; f < frames; ++f) {
        s.set_input(0, input);
        const FrameView& fv = s.step();

        size_t vsz = 0, rsz = 0;
        const uint8_t* vram = s.video_ram(&vsz);
        const uint8_t* regs = s.vdp_regs(&rsz);
        if (!vram || !regs || rsz < 0x20) continue;

        // -- Frame congelado para el volcado visual (#231) -------------------
        // El centro sale de la recomposición multicapa (E-7 #406): es el frame
        // REAL del emulador. Si el core no la soporta, el volcado se saltea y
        // la medición sigue — no es motivo para perder la toma.
        if (f == t.snap_frame) {
            const auto L = s.recompose_layers();
            if (L.ok()) {
                t.snap_w = (int)L.width; t.snap_h = (int)L.height;
                t.snap_fb.assign(L.composite, L.composite + (size_t)L.width * L.height);
                t.snap_vram.assign(vram, vram + vsz);
                size_t csz = 0;
                if (const uint8_t* cr = s.color_ram(&csz)) t.snap_cram.assign(cr, cr + csz);
            } else {
                std::printf("     [aviso] sin volcado visual: %s\n", s.layers_error());
                t.snap_frame = -1;
            }
        }

        auto rd16 = [&](uint32_t o) -> uint32_t {
            return (o + 1 < vsz) ? (uint32_t)vram[o] | ((uint32_t)vram[o + 1] << 8) : 0u;
        };
        auto rd32 = [&](uint32_t o) -> uint32_t {
            return (o + 3 < vsz) ? (uint32_t)vram[o] | ((uint32_t)vram[o+1] << 8)
                                 | ((uint32_t)vram[o+2] << 16) | ((uint32_t)vram[o+3] << 24) : 0u;
        };

        auto cells = [](uint8_t b) { return b == 1 ? 64 : b == 3 ? 128 : 32; };
        const int wc  = cells(regs[0x10] & 3);
        const int hc  = cells((regs[0x10] >> 4) & 3);
        const int wpx = wc * 8, hpx = hc * 8;
        const int sw  = (int)fv.fb_width  ? (int)fv.fb_width  : 320;
        const int sh  = (int)fv.fb_height ? (int)fv.fb_height : 224;
        const uint32_t base[2] = { ((uint32_t)regs[0x02] & 0x38u) << 10,
                                   ((uint32_t)regs[0x04] & 0x07u) << 13 };
        const uint32_t hscb  = ((uint32_t)regs[0x0D] << 10) & 0xFC00u;
        const uint32_t hmaskTab[4] = { 0x00, 0x07, 0xF8, 0xFF };
        const uint32_t hmask = hmaskTab[regs[0x0B] & 3];
        const int V0[2] = { fv.plane_vscroll[0], fv.plane_vscroll[1] };

        // -- Cámara VERTICAL, des-enrollada igual que la horizontal ----------
        // Sin esto la posición de nivel vertical no existe y hay que indexar
        // por fila de NAMETABLE, que no es absoluta: con streaming vertical la
        // misma fila contiene distintas alturas del nivel en distintos momentos,
        // y la lámina termina comparando manzanas con naranjas. Es el mismo
        // unwrap del H, en el módulo en que el scroll realmente wrapea (hpx).
        for (int p = 0; p < 2; ++p) {
            if (!first) {
                int dv = (V0[p] % hpx) - (t.prev_v[p] % hpx);
                if (dv >  hpx / 2) dv -= hpx;
                else if (dv < -hpx / 2) dv += hpx;
                t.camv[p] += dv;   // V mueve la imagen hacia ARRIBA (screen_y = cy*8 - V)
            }
            t.prev_v[p] = V0[p];
        }

        // -- Cámara por entrada de la tabla Hscroll --------------------------
        // Se des-wrapea en el módulo en que el scroll REALMENTE wrapea, que es
        // el ancho del plano (sólo `H mod wpx` decide la imagen). El campo del
        // VDP es de 10 bits y el juego puede escribir por encima de wpx, así
        // que primero se REDUCE y recién ahí se unwrapea: cualquiera de los dos
        // módulos a secas acierta en un juego y falla en el otro.
        for (int p = 0; p < 2; ++p) {
            for (int idx = 0; idx < kH; ++idx) {
                if (((uint32_t)idx & hmask) != (uint32_t)idx) continue;
                const uint32_t hw = rd32(hscb + ((uint32_t)idx << 2));
                const int H = (p == 0) ? (int)(hw & 0x3FF) : (int)((hw >> 16) & 0x3FF);
                t.hmax = std::max(t.hmax, H);
                const int slot = p * kH + idx;
                if (!first) {
                    int dh = (H % wpx) - (prev[slot] % wpx);
                    if (dh >  wpx / 2) dh -= wpx;
                    else if (dh < -wpx / 2) dh += wpx;
                    if (p == 0 && idx == 0 && dh != 0) ++t.scrolled;
                    cam[slot] += -dh;
                    if (p == 0 && idx == 0) t.cam_span = cam[slot];
                }
                prev[slot] = H;
            }
        }
        first = false;

        // #231 EM-8.0: cuantas BANDAS de parallax tiene cada plano en este
        // frame — valores DISTINTOS de H entre las entradas activas de la
        // tabla Hscroll. El stitcher de Fondos acumula con UNA camara por
        // plano, asi que todo lo que este por encima de 1 banda se le mezcla.
        for (int p = 0; p < 2; ++p) {
            std::vector<int> hs;
            for (int idx = 0; idx < kH; ++idx) {
                if (((uint32_t)idx & hmask) != (uint32_t)idx) continue;
                const uint32_t hw = rd32(hscb + ((uint32_t)idx << 2));
                hs.push_back((p == 0) ? (int)(hw & 0x3FF) : (int)((hw >> 16) & 0x3FF));
            }
            std::sort(hs.begin(), hs.end());
            hs.erase(std::unique(hs.begin(), hs.end()), hs.end());
            const int n = (int)hs.size();
            t.max_band_count[p] = std::max(t.max_band_count[p], n);
            t.band_count_sum[p] += n;
            ++t.band_frame_count[p];
        }

        // -- Una sola pasada: verdad de nivel + sondas laterales --------------
        for (int plane = 0; plane < 2; ++plane) {
            for (int cy = 0; cy < hc; ++cy) {
                for (int cx = 0; cx < wc; ++cx) {
                    const uint16_t w = (uint16_t)rd16(base[plane] + (uint32_t)(cy * wc + cx) * 2u);
                    const uint16_t code = w & 0x7FFF;      // patrón|flips|paleta (sin prioridad)
                    const int sline = ((cy * 8 + 4 - V0[plane]) % hpx + hpx) % hpx;
                    const uint32_t hidx = (uint32_t)sline & hmask;
                    const uint32_t hw = rd32(hscb + (hidx << 2));
                    const int Hh = (plane == 0) ? (int)(hw & 0x3FF) : (int)((hw >> 16) & 0x3FF);
                    const int sx = ((cx * 8 + Hh) % wpx + wpx) % wpx;
                    const int sy = ((cy * 8 - V0[plane]) % hpx + hpx) % hpx;
                    // ¿Que filas de nametable llega a mostrar la ventana en TODA
                    // la toma? Es la pregunta que decide si los huecos de la
                    // lamina son falta de recorrido o filas que el juego nunca
                    // dibujo — y sin el dato, EM-8.0 se disenaria a ciegas.
                    if (sy < sh && cy < 32) t.visible_row[plane][cy] = 1;
                    if (sy >= sh) continue;                // misma ventana vertical que el motor

                    // El sx sale reducido mod wpx, así que las columnas de la
                    // IZQUIERDA aparecen como sx grande (cerca de wpx). Para la
                    // posición de nivel hay que usar la x con SIGNO respecto de
                    // la ventana: si no, una columna a la izquierda se compara
                    // contra un tramo del nivel que está wpx px a la derecha.
                    const int sxs = (sx >= sw && sx >= wpx - kProbeCols * 8) ? sx - wpx : sx;
                    const int32_t lvx = fdiv8(cam[plane * kH + hidx] + sxs);
                    // Fila de NIVEL, no de nametable: la cámara vertical
                    // des-enrollada más la posición en pantalla. Es lo que hace
                    // que una celda vista con un V y otra vista con otro V se
                    // comparen entre sí sólo si son el MISMO pedazo de nivel.
                    const int32_t lvy = fdiv8(t.camv[plane] + sy);
                    const auto    key = std::make_tuple((uint8_t)plane, lvx, lvy);

                    if (sx >= 0 && sx < sw) {
                        // VISIBLE. Con margen contra los bordes de streaming
                        // (el juego escribe la columna entrante 1-2 celdas antes
                        // de que se vea) pasa a ser verdad de nivel.
                        const int scx = sx / 8;
                        if (scx < kEdgeGuard || scx >= sw / 8 - kEdgeGuard) continue;
                        auto [it, fresh] = t.truth.try_emplace(key, TruthCell{ code, f, false });
                        if (!fresh && it->second.code != code) it->second.animated = true;
                        continue;
                    }

                    // LATERAL: sx viene reducido mod wpx, así que la izquierda
                    // aparece como sx grande. Se convierte a coordenada con
                    // signo respecto de la ventana visible.
                    int side, k;
                    if (sxs < 0) {
                        side = 0; k = (-sxs) / 8;                        // izquierda
                    } else if (sx >= sw && sx < sw + kProbeCols * 8) {
                        side = 1; k = (sx - sw) / 8 + 1;                 // derecha
                    } else {
                        continue;                                         // fuera del interés
                    }
                    if (k < 1 || k > kProbeCols) continue;
                    const uint32_t slot =
                        (((uint32_t)f * 2u + (uint32_t)plane) * 2u + (uint32_t)side)
                        * (uint32_t)kProbeCols + (uint32_t)(k - 1);
                    t.probes.push_back(Probe{ slot, lvx, lvy, code, (uint8_t)plane, (uint8_t)cy });
                    // La MISMA celda, guardada para el volcado visual del frame
                    // elegido. Sale de esta pasada y no de una propia para que
                    // la imagen muestre exactamente lo que la medición midió.
                    if (f == t.snap_frame)
                        t.snap_cells.push_back(SnapCell{
                            (side == 0) ? sxs : sx, sy, lvx, lvy, code,
                            (uint8_t)plane, (uint8_t)cy });
                }
            }
        }
    }

    t.px1 = (int)s.ram_u16(0xB008);

    // -- Resolución contra la verdad FINAL de la toma -------------------------
    t.agg.assign((size_t)frames * 2u * 2u * kProbeCols, Bucket{});
    for (const Probe& p : t.probes) {
        Bucket& b = t.agg[p.slot];
        auto it = t.truth.find(std::make_tuple(p.plane, p.lvx, p.lvy));
        if (it != t.truth.end() && it->second.animated) continue;   // excluida
        if (it != t.truth.end()) {
            if (it->second.code == p.code) {
                ++b.valid;
                // Control causal del signo (sólo plano A, primera columna): una
                // sonda del lado LÍDER debe describir nivel que se ve DESPUÉS;
                // una del TRASERO, nivel que se vio ANTES.
                const uint32_t k    = p.slot % (uint32_t)kProbeCols;
                const uint32_t side = (p.slot / (uint32_t)kProbeCols) % 2u;
                const int32_t  fr   = (int32_t)(p.slot / (uint32_t)(4 * kProbeCols));
                if (p.plane == 0 && k == 0) {
                    if (side == 1) { ++t.lead_valid;  if (it->second.first >  fr) ++t.lead_future; }
                    else           { ++t.trail_valid; if (it->second.first <= fr) ++t.trail_past;  }
                }
            } else ++b.stale;
        } else if ((p.code & 0x7FF) == 0) {
            ++b.empty;                                              // celda vacía: inocua
        } else {
            ++b.unknown;
        }
    }
}

// ── Reporte ─────────────────────────────────────────────────────────────────
// ── El PNG del widescreen crudo ─────────────────────────────────────────────
// Centro = el frame real. Laterales = lo que hay en la nametable, rasterizado,
// teñido de magenta si es arte de OTRO tramo del nivel y de gris si esa
// posición nunca se vio. Dos guías verticales marcan el borde del 4:3 original:
// sin ellas no se distingue lo que el emulador ya daba de lo que se agregó.
// `from_truth` cambia la FUENTE del arte lateral, no el dibujo:
//
//   false — la nametable VIVA de ese frame: lo que el emulador tiene ahora
//           mismo fuera de la ventana. Es el enfoque que describe EM-8.1.
//   true  — la TIRA DE NIVEL: para cada posición lateral se busca el código que
//           esa posición mostró cuando estuvo en pantalla, en cualquier momento
//           de la toma. Es lo que reconstruye el stitcher de Fondos, y es la
//           diferencia decisiva: el lado LÍDER deja de depender de cuánto
//           adelanta el streamer del juego, porque el arte ya se recorrió una
//           vez. Lo que la tira NO puede tener es lo que el nivel nunca mostró
//           (queda gris) ni las celdas animadas (agua, cascadas: la tira las
//           congela, y por eso se excluyen del veredicto).
static bool dump_wide_png(const Take& t, int extra_cols, const std::string& path,
                          bool from_truth = false) {
    if (t.snap_frame < 0 || t.snap_fb.empty() || t.snap_cram.size() < 128) return false;
    const int sw = t.snap_w, sh = t.snap_h, pad = extra_cols * 8;
    const int W = sw + pad * 2;
    std::vector<uint8_t> img((size_t)W * sh * 3, 0);

    // Centro: RGB565 del composite → RGB888.
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            const uint16_t p = t.snap_fb[(size_t)y * sw + x];
            uint8_t* d = &img[((size_t)y * W + pad + x) * 3];
            const unsigned r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
            d[0] = (uint8_t)((r << 3) | (r >> 2));
            d[1] = (uint8_t)((g << 2) | (g >> 4));
            d[2] = (uint8_t)((b << 3) | (b >> 2));
        }

    // Laterales: plano B primero y A encima, que es el orden del VDP.
    int pintadas = 0, rancias = 0, descon = 0;
    // Dónde caen los huecos de la tira. Si se concentran en las filas de
    // nametable que la ventana vertical casi nunca muestra, el 24% «sin arte»
    // no es un límite del enfoque sino contenido que el juego nunca dibujó.
    int rows_without_art[32] = {0};
    int diag = 0;
    for (int pass = 1; pass >= 0; --pass) {
        for (const SnapCell& c : t.snap_cells) {
            if ((int)c.plane != (pass == 1 ? 1 : 0)) continue;
            const int x0 = pad + c.sxs, y0 = c.sy;
            if (x0 + 8 <= 0 || x0 >= W || y0 + 8 <= 0 || y0 >= sh) continue;
            if (x0 >= pad && x0 + 8 <= pad + sw) continue;   // eso ya es el centro
            uint8_t  verdict = 2;
            uint16_t code    = c.code;
            auto it = t.truth.find(std::make_tuple(c.plane, c.lvx, c.lvy));
            if (from_truth) {
                // Desde la tira: si esa posición de nivel se vio has_any vez, se
                // dibuja LO QUE MOSTRÓ. No hay «rancio» posible — o hay arte de
                // esa posición, o no hay nada.
                if (it == t.truth.end()) {
                    ++descon; ++pintadas;
                    if (c.cy < 32) ++rows_without_art[c.cy];
                    // ¿Falta la COLUMNA entera o sólo esta fila? Es la pregunta
                    // que separa «el jugador nunca pasó por ahí» de «la lámina
                    // está mal indexada verticalmente», y las dos se arreglan
                    // distinto. Se muestran las primeras, con las filas de nivel
                    // que esa columna sí tiene.
                    if (diag < 3) {
                        int rows_in_column = 0; int32_t lvy_min = 0, lvy_max = 0;
                        for (const auto& [k, v] : t.truth) {
                            if (std::get<0>(k) != c.plane || std::get<1>(k) != c.lvx) continue;
                            const int32_t y = std::get<2>(k);
                            if (!rows_in_column || y < lvy_min) lvy_min = y;
                            if (!rows_in_column || y > lvy_max) lvy_max = y;
                            ++rows_in_column;
                        }
                        std::printf("       [diag] hueco plano %u lvx=%d lvy=%d (cy=%u) → "
                                    "esa columna tiene %d filas de verdad, lvy %d..%d\n",
                                    c.plane, c.lvx, c.lvy, c.cy, rows_in_column, lvy_min, lvy_max);
                        ++diag;
                    }
                    continue;
                }
                code = it->second.code;
                verdict = it->second.animated ? 2 : 0;   // animada: se marca, no se afirma
                if (verdict == 2) ++descon;
            } else {
                if (it != t.truth.end() && !it->second.animated)
                    verdict = (it->second.code == c.code) ? 0 : 1;
                else if (it != t.truth.end()) verdict = 0;   // animada: no se acusa
                if (verdict == 1) ++rancias; else if (verdict == 2) ++descon;
            }
            ++pintadas;
            // Se rasteriza a un buffer de celda y se copia con recorte: una
            // celda a medio entrar es normal (el scroll no está alineado a 8) y
            // tiene que verse a medias, no desaparecer.
            uint8_t cell[8 * 8 * 3];
            draw_cell(cell, 8, t.snap_vram.data(), t.snap_vram.size(),
                      t.snap_cram.data(), code, verdict);
            for (int y = 0; y < 8; ++y) {
                const int yy = y0 + y;
                if (yy < 0 || yy >= sh) continue;
                for (int x = 0; x < 8; ++x) {
                    const int xx = x0 + x;
                    if (xx < 0 || xx >= W) continue;
                    const uint8_t* sp = &cell[((size_t)y * 8 + x) * 3];
                    if (!sp[0] && !sp[1] && !sp[2]) continue;   // transparente
                    std::memcpy(&img[((size_t)yy * W + xx) * 3], sp, 3);
                }
            }
        }
    }

    // Guías del 4:3 original.
    for (int y = 0; y < sh; ++y) {
        for (int gx : { pad - 1, pad + sw }) {
            if (gx < 0 || gx >= W) continue;
            uint8_t* d = &img[((size_t)y * W + gx) * 3];
            d[0] = 255; d[1] = 210; d[2] = 0;
        }
    }

    const bool ok = stbi_write_png(path.c_str(), W, sh, 3, img.data(), W * 3) != 0;
    std::printf("     %-9s %s  (%dx%d · %d celdas laterales · %d rancias · %d sin arte)\n",
                from_truth ? "[tira]" : "[nametable]",
                ok ? path.c_str() : "FALLO", W, sh, pintadas, rancias, descon);
    if (from_truth && descon) {
        std::printf("       huecos por fila de nametable:");
        for (int r = 0; r < 32; ++r)
            if (rows_without_art[r]) std::printf(" %d:%d", r, rows_without_art[r]);
        std::printf("\n       filas que la ventana mostro en TODA la toma (plano A):");
        for (int r = 0; r < 32; ++r) if (t.visible_row[0][r]) std::printf(" %d", r);
        std::printf("\n       …y las que NUNCA se vieron:");
        bool has_any = false;
        for (int r = 0; r < 32; ++r)
            if (!t.visible_row[0][r]) { std::printf(" %d", r); has_any = true; }
        std::printf("%s\n", has_any ? "" : " (ninguna)");
    }
    return ok;
}

struct RunStats { int p5 = 0, p50 = 0, p95 = 0, mx = 0; };

static RunStats percentiles(std::vector<int>& v) {
    RunStats r;
    if (v.empty()) return r;
    std::sort(v.begin(), v.end());
    auto at = [&](double q) { return v[(size_t)((v.size() - 1) * q)]; };
    r.p5 = at(0.05); r.p50 = at(0.50); r.p95 = at(0.95); r.mx = v.back();
    return r;
}

// Racha contigua de columnas DIBUJABLES desde el borde hacia afuera — lo único
// que se puede ensanchar sin agujeros.
//
// Una columna es dibujable cuando lo que hay en ella es del nivel: se mide por
// proporción, no por unanimidad (28 filas por columna; una sola fila rancia no
// invalida la columna, y exigir cero DESCONOCIDA la invalidaría siempre porque
// la verdad sólo cubre lo que la toma llegó a ver).
//
//   RANCIA / (VÁLIDA + RANCIA) <= kStaleTol   ⇒ el contenido coincide con el nivel
//   VÁLIDA + RANCIA            >= 25% de las celdas ⇒ hay evidencia suficiente
//
// Una columna VACÍA es inocua (no hay nada que dibujar mal), pero una columna
// TODA vacía o TODA desconocida no cuenta como verificada.
static constexpr double kStaleTol = 0.10;

static bool column_ok(const Bucket& b) {
    const int n = b.valid + b.stale + b.empty + b.unknown;
    if (n == 0) return false;
    const int ev = b.valid + b.stale;
    if (ev * 4 < n) return false;                       // <25% de evidencia
    return double(b.stale) / double(ev) <= kStaleTol;
}

static RunStats runs(const Take& t, int plane, int side) {
    std::vector<int> out;
    out.reserve(t.frames);
    for (int f = 0; f < t.frames; ++f) {
        int run = 0;
        for (int k = 1; k <= kProbeCols; ++k) {
            const size_t slot =
                (((size_t)f * 2u + (size_t)plane) * 2u + (size_t)side) * kProbeCols + (k - 1);
            if (!column_ok(t.agg[slot])) break;
            ++run;
        }
        out.push_back(run);
    }
    return percentiles(out);
}

static void report_take(const Take& t) {
    std::printf("\n── toma «%s» ─────────────────────────────────────────────\n", t.name.c_str());
    size_t animated = 0;
    for (const auto& [k, v] : t.truth) if (v.animated) ++animated;
    std::printf("[..] frames %d · con scroll %d · recorrido %d px · jugador X %d→%d · "
                "celdas de verdad %zu (animadas %zu = %.0f%%) · H crudo máx %d\n",
                t.frames, t.scrolled, (int)t.cam_span, t.px0, t.px1, t.truth.size(), animated,
                t.truth.empty() ? 0.0 : 100.0 * animated / t.truth.size(), t.hmax);

    static const char* kPlane[2] = { "A", "B" };
    static const char* kSide[2]  = { "izq", "der" };
    for (int p = 0; p < 2; ++p) {
        for (int side = 0; side < 2; ++side) {
            const RunStats st = runs(t, p, side);
            std::printf("     plano %s · lado %s · racha dibujable p5/p50/p95/max = "
                        "%d/%d/%d/%d celdas (p50 = %d px)\n",
                        kPlane[p], kSide[side], st.p5, st.p50, st.p95, st.mx, st.p50 * 8);
        }
    }
    // Distribución por columna para el plano A (el que lleva el nivel).
    std::printf("     plano A por columna (%% de celdas):  k  válida rancia  vacía descon.\n");
    for (int side = 0; side < 2; ++side) {
        for (int k = 1; k <= kProbeCols; ++k) {
            long v = 0, st = 0, e = 0, u = 0;
            for (int f = 0; f < t.frames; ++f) {
                const size_t slot = (((size_t)f * 2u + 0u) * 2u + (size_t)side)
                                    * kProbeCols + (k - 1);
                const Bucket& b = t.agg[slot];
                v += b.valid; st += b.stale; e += b.empty; u += b.unknown;
            }
            const double n = double(v + st + e + u);
            if (n == 0) continue;
            std::printf("       %s %2d   %5.0f%% %5.0f%% %5.0f%% %5.0f%%\n", kSide[side], k,
                        100.0 * v / n, 100.0 * st / n, 100.0 * e / n, 100.0 * u / n);
        }
    }
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    const std::string root = AYTHER_SOURCE_DIR;
    std::ifstream cfg(root + "/tests/test_config.toml");
    std::string core, rom, line;
    bool in_rom = false;
    if (cfg) {
        while (std::getline(cfg, line)) {
            if (line.find("[[rom]]") != std::string::npos) in_rom = true;
            else if (line.find("core") != std::string::npos
                     && line.find('=') != std::string::npos && core.empty())
                core = quoted(line);
            else if (in_rom && rom.empty() && line.find("path") != std::string::npos)
                rom = quoted(line);
        }
    }
    core = resolve_path(core, root);
    rom  = resolve_path(rom, root);
    // AYTHER_PROBE_ROM gana sobre el config, igual que en el resto de los
    // oraculos. Sin esto, este spike agarraba la PRIMERA `[[rom]]` del config
    // —Aladdin— mientras su propia doc dice que el corpus es Sonic 2, y quien
    // exportara la variable creyendo que elegia la ROM medía otro juego sin
    // enterarse. Paso de verdad, y el numero equivocado llego a un reporte.
    if (const char* er = ayther::env_get("AYTHER_PROBE_ROM")) if (*er) rom = er;
    if (const char* ef = ayther::env_get("AYTHER_WS_FRAMES"))
        if (*ef && std::atoi(ef) > 60) kMeasure = std::atoi(ef);
    if (argc >= 3) { core = argv[1]; rom = argv[2]; }
    // El boot scripteado de abajo (START x N + RIGHT) es de Sonic 2, y el
    // veredicto usa `ram_u16(0xB008)` que es SU direccion del jugador. Con otra
    // ROM la toma puede salir sin scroll y el analisis no significa nada.
    if (rom.find("Sonic The Hedgehog 2") == std::string::npos)
        std::printf("[aviso] el corpus de este spike es Sonic 2; con «%s» el boot "
                    "scripteado y la lectura del jugador NO aplican\n", rom.c_str());
    if (core.empty() || rom.empty()) { std::fprintf(stderr, "[FAIL] config incompleta\n"); return 1; }
    std::printf("=== widescreen_spike (EM-8 #231) ===\ncore: %s\nrom : %s\n",
                core.c_str(), rom.c_str());

    ayther::AytherSession::Config c;
    c.core_path = core; c.rom_path = rom; c.enable_audio = false;
    auto r = ayther::AytherSession::create(c);
    if (!r) { std::fprintf(stderr, "[FAIL] create: %s\n", r.error.message.c_str()); return 1; }
    std::unique_ptr<ayther::AytherSession>& s = *r;

    // Boot de Sonic 2 (idéntico a cam_track_smoke / background_smoke).
    constexpr uint16_t START = 1u << 3, LEFT = 1u << 6, RIGHT = 1u << 7, JUMP = 1u << 0;
    for (int i = 0; i < 1400; ++i) { s->set_input(0, (i % 32 == 0) ? START : 0); s->step(); }
    for (int i = 0; i < 200;  ++i) { s->set_input(0, RIGHT); s->step(); }

    Take right, still;
    // El frame del volcado visual va a MITAD de la toma: así la verdad de nivel
    // que lo clasifica incluye los frames posteriores, que son los que revelan
    // si lo que hoy está en el lateral líder era realmente el nivel que venía.
    right.snap_frame = kMeasure / 2;
    measure(*s, RIGHT, kMeasure, right, "derecha");
    // Frenar del todo para el control negativo. (Una toma con scroll a la
    // IZQUIERDA no se puede producir con input scripteado: Sonic se traba en
    // el relieve y la toma sale sin scroll — probado con LEFT pelado y con
    // saltos periódicos. La asimetría por dirección se demuestra igual, y
    // mejor, con el control causal de abajo: dentro de la MISMA toma, el
    // lateral líder describe nivel FUTURO y el trasero nivel PASADO.)
    for (int i = 0; i < 120; ++i) { s->set_input(0, 0); s->step(); }
    measure(*s, 0, 120, still, "quieto (control negativo)");

    report_take(right);
    report_take(still);

    // ── El widescreen crudo, para verlo ─────────────────────────────────────
    // Los dos anchos que pide 16:9: píxel cuadrado (+5 celdas por lado) y
    // preservando la relación del 4:3 mostrado (+7).
    {
        const std::string dir = std::string(AYTHER_SOURCE_DIR) + "/build/bin";
        // #231 EM-8.0: el stitcher de Fondos acumula con UNA cámara por plano.
        // Todo lo que pase de 1 banda se le mezcla, y ése es el trabajo que la
        // lámina necesita — no hacerla 2D, que ya lo es.
        std::printf("\n── bandas de parallax (una cámara por plano NO alcanza si >1) ──\n");
        for (int p = 0; p < 2; ++p)
            std::printf("     plano %c · bandas por frame: media %.1f · máximo %d\n",
                        p ? 'B' : 'A',
                        right.band_frame_count[p]
                            ? double(right.band_count_sum[p]) / double(right.band_frame_count[p]) : 0.0,
                        right.max_band_count[p]);

        std::printf("\n── volcado visual del widescreen crudo ──────────────────\n");
        dump_wide_png(right, kNeedSquare, dir + "/widescreen_5col.png");
        dump_wide_png(right, kNeedPar,    dir + "/widescreen_7col.png");
        // La MISMA escena alimentada por la tira de nivel, que es lo que el
        // stitcher de Fondos ya reconstruye. Es la comparación que decide el
        // enfoque: si acá el lateral se llena, la feature no depende del
        // lookahead del juego.
        dump_wide_png(right, kNeedPar, dir + "/widescreen_7col_tira.png", true);
    }

    // ── Veredicto ───────────────────────────────────────────────────────────
    // Yendo a la DERECHA: el lado trasero es el izquierdo (rastro recién salido
    // de pantalla) y el líder es el derecho (sólo el lookahead del streamer).
    const RunStats trail_r = runs(right, 0, 0);
    const RunStats lead_r  = runs(right, 0, 1);
    const double   fut = right.lead_valid  ? 100.0 * right.lead_future / right.lead_valid : 0.0;
    const double   pas = right.trail_valid ? 100.0 * right.trail_past  / right.trail_valid : 0.0;

    std::printf("\n── veredicto (plano A, racha dibujable p50) ────────────────\n");
    std::printf("[..] yendo a la derecha : trasero(izq) %d · líder(der) %d celdas\n",
                trail_r.p50, lead_r.p50);
    std::printf("[..] control causal (k=1): del lateral LÍDER, %.0f%% describe nivel que se "
                "ve DESPUÉS · del TRASERO, %.0f%% nivel que ya se vio ANTES\n", fut, pas);
    std::printf("[..] 16:9 pide %d celdas por lado (píxel cuadrado, 398 px) o %d "
                "(preservando el 4:3 mostrado, 426 px)\n", kNeedSquare, kNeedPar);
    std::printf("[..] simétrico desde la nametable viva = min(líder, trasero) = %d celdas → %s\n",
                std::min(lead_r.p50, trail_r.p50),
                std::min(lead_r.p50, trail_r.p50) >= kNeedSquare
                    ? "ALCANZA"
                    : "NO ALCANZA — el lado LÍDER es el techo real (el juego sólo "
                      "streamea 1-2 celdas por delante)");

    int fail = 0;
    auto check = [&](bool ok, const char* m) {
        std::printf("[%s] %s\n", ok ? "ok" : "FAIL", m); if (!ok) ++fail;
    };
    check(right.scrolled > 200,
          "control: la toma derecha tiene scroll real (si falla: ROM sin scroll whole-plane)");
    check((int)right.truth.size() > 80 * 20,
          "control: la verdad de nivel abarca varias pantallas");
    check(trail_r.p50 >= kNeedPar,
          "el lado TRASERO cubre lo que pide 16:9 (>=7 celdas de arte real del nivel)");
    check(lead_r.p50 <= trail_r.p50,
          "asimetria: el lado LIDER nunca supera al trasero");
    check(fut >= 60.0 && pas >= 90.0,
          "control causal: el lateral lider es LOOKAHEAD y el trasero es RASTRO "
          "(valida el signo del mapeo, no solo su coherencia)");
    check(lead_r.p50 < kNeedSquare,
          "hallazgo: el lado LIDER NO alcanza 16:9 desde la nametable viva");
    check(still.scrolled < 20,
          "control negativo: la toma quieta no scrollea (sin scroll no hay evidencia lateral)");

    std::printf("\n%s (%d fallos)\n", fail ? "FALLÓ" : "OK", fail);
    return fail ? 1 : 0;
}
