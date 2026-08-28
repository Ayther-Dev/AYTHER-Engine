// ---------------------------------------------------------------------------
// xbr_ref_test (#504) — la REFERENCIA CPU del filtro «Mejorar por software»
// (tools/enhance_shader_smoke/xbr_ref.h) bajo test sin GPU: las propiedades
// DISCRETAS del contrato del shader. El smoke GPU compara el .frag contra
// esta misma referencia; si esto falla, el contrato cambió, no el shader.
// ---------------------------------------------------------------------------
#include "../../tools/enhance_shader_smoke/xbr_ref.h"

#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstring>

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

// Tile 8×8 desde un dibujo ASCII: '.' = 0 (transparente), dígito = índice.
static void tile_from(const char* rows[8], uint8_t out[64]) {
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            out[y * 8 + x] = rows[y][x] == '.' ? 0 : (uint8_t)(rows[y][x] - '0');
}

int main() {
    std::printf("=== xbr_ref_test (#504) ===\n");
    constexpr int S = 6;   // escala del quad

    // ---- 1. Diagonal 45° entre dos índices OPACOS: hay borde y mezcla -------
    {
        const char* d[8] = { "11111111", "21111111", "22111111", "22211111",
                             "22221111", "22222111", "22222211", "22222221" };
        uint8_t t[64]; tile_from(d, t);
        xbr_ref::Tile T; T.idx = t;
        int blended = 0, discards = 0;
        for (int y = 0; y < 8 * S; ++y)
            for (int x = 0; x < 8 * S; ++x) {
                const xbr_ref::Out o = xbr_ref::shade(T, x, y, S);
                if (o.discard) ++discards;
                else if (o.blend && o.cov > 0.0f && o.cov < 1.0f) ++blended;
            }
        check(discards == 0, "diagonal opaca: ningún discard");
        check(blended > 0, "diagonal opaca: hay píxeles con mezcla parcial (anti-alias)");
        // Lejos del borde el píxel es su índice, sin mezcla.
        const xbr_ref::Out a = xbr_ref::shade(T, 7 * S + 3, 0 * S + 3, S);
        check(!a.discard && a.idx == 1 && a.blend == 0, "esquina sup-der: índice 1 puro");
        const xbr_ref::Out b = xbr_ref::shade(T, 0 * S + 3, 7 * S + 3, S);
        check(!b.discard && b.idx == 2 && b.blend == 0, "esquina inf-izq: índice 2 puro");
    }

    // ---- 2. Línea de 1 px (anti-muesca #500): se conserva íntegra ----------
    {
        const char* d[8] = { "........", "........", "........", "11111111",
                             "........", "........", "........", "........" };
        uint8_t t[64]; tile_from(d, t);
        xbr_ref::Tile T; T.idx = t;
        bool intact = true;
        for (int y = 3 * S; y < 4 * S; ++y)
            for (int x = 0; x < 8 * S; ++x) {
                const xbr_ref::Out o = xbr_ref::shade(T, x, y, S);
                if (o.discard || o.idx != 1 || o.blend || o.alpha < 0.999f) intact = false;
            }
        check(intact, "línea de 1 px: todos sus píxeles opacos, sin mezcla ni alpha");
        bool clean = true;
        for (int y = 0; y < 8 * S; ++y) {
            if (y >= 3 * S && y < 4 * S) continue;
            for (int x = 0; x < 8 * S; ++x)
                if (!xbr_ref::shade(T, x, y, S).discard) clean = false;
        }
        check(clean, "línea de 1 px: fuera de la línea todo descartado");
    }

    // ---- 3. Silueta: diagonal opaco/transparente → alpha parcial ----------
    {
        const char* d[8] = { "11111111", ".1111111", "..111111", "...11111",
                             "....1111", ".....111", "......11", ".......1" };
        uint8_t t[64]; tile_from(d, t);
        xbr_ref::Tile T; T.idx = t;
        int partial = 0, filled = 0;
        for (int y = 0; y < 8 * S; ++y)
            for (int x = 0; x < 8 * S; ++x) {
                const xbr_ref::Out o = xbr_ref::shade(T, x, y, S);
                if (o.discard) continue;
                if (o.alpha > 0.002f && o.alpha < 0.998f) ++partial;
                // píxel destino cuyo texel fuente era transparente pero se llenó
                const int tx = x / S, ty = y / S;
                if (t[ty * 8 + tx] == 0) ++filled;
            }
        check(partial > 0, "silueta: hay píxeles con alpha parcial");
        check(filled > 0, "silueta: el lado transparente recibe píxeles del opaco (suaviza)");
    }

    // ---- 4. Sin vecino = clamp al tile (v1) vs con vecino válido ----------
    {
        // Tile con borde vertical en x=3|4 ... y un vecino DERECHO que continúa
        // el índice 1: con vecino, el píxel (7,y) ve F=1 del vecino.
        const char* d[8] = { "22221111", "22221111", "22221111", "22221111",
                             "22221111", "22221111", "22221111", "22221111" };
        const char* n[8] = { "11111111", "11111111", "11111111", "11111111",
                             "11111111", "11111111", "11111111", "11111111" };
        uint8_t t[64], nt[64]; tile_from(d, t); tile_from(n, nt);
        xbr_ref::Tile T; T.idx = t;
        const uint32_t clampv = xbr_ref::fetch_screen(T, 8, 3, 9);
        check(clampv == 9, "fetch_screen sin vecino: devuelve `center` (clamp)");
        T.right.valid = true; T.right.idx = nt;
        check(xbr_ref::fetch_screen(T, 8, 3, 9) == 1, "fetch_screen con vecino: lee el tile vecino");
        T.right.hf = true;   // el vecino espejado sigue siendo 1 (uniforme)
        check(xbr_ref::fetch_screen(T, 9, 3, 9) == 1, "fetch_screen con vecino hflip: resuelve con SU flip");
    }

    // ---- 5. Flip del tile central: el resultado es el espejo --------------
    {
        const char* d[8] = { "11111111", "21111111", "22111111", "22211111",
                             "22221111", "22222111", "22222211", "22222221" };
        uint8_t t[64]; tile_from(d, t);
        xbr_ref::Tile A; A.idx = t;
        xbr_ref::Tile Bf; Bf.idx = t; Bf.hf = true;
        bool mirror = true;
        for (int y = 0; y < 8 * S && mirror; ++y)
            for (int x = 0; x < 8 * S; ++x) {
                const xbr_ref::Out a = xbr_ref::shade(A, x, y, S);
                // v_local ya viene espejado del vertex shader: el destino
                // (x, y) en espacio de TEXEL es el píxel de pantalla
                // 8S-1-x, así que la misma coordenada debe dar el mismo
                // resultado que el tile sin flip.
                const xbr_ref::Out b = xbr_ref::shade(Bf, x, y, S);
                if (a.discard != b.discard || a.idx != b.idx || a.blend != b.blend ||
                    std::abs(a.cov - b.cov) > 1e-5f) { mirror = false; break; }
            }
        check(mirror, "hflip del tile: misma imagen de pantalla espejada (mismo resultado por texel)");
    }

    // ---- 6. Intensidad k (#503): k=0 deja todo borde al 50 %; k monótona --
    {
        const char* d[8] = { "11111111", "21111111", "22111111", "22211111",
                             "22221111", "22222111", "22222211", "22222221" };
        uint8_t t[64]; tile_from(d, t);
        xbr_ref::Tile T; T.idx = t;
        bool half_only = true, mono = true, same_set = true;
        for (int y = 0; y < 8 * S; ++y)
            for (int x = 0; x < 8 * S; ++x) {
                const xbr_ref::Out a = xbr_ref::shade(T, x, y, S, 1.0f);
                const xbr_ref::Out m = xbr_ref::shade(T, x, y, S, 0.5f);
                const xbr_ref::Out z = xbr_ref::shade(T, x, y, S, 0.0f);
                if (a.blend != m.blend || a.blend != z.blend || a.idx != z.idx) same_set = false;
                if (!a.blend) continue;
                if (std::abs(z.cov - 0.5f) > 1e-5f) half_only = false;
                const float lo = std::min(a.cov, 0.5f), hi = std::max(a.cov, 0.5f);
                if (m.cov < lo - 1e-5f || m.cov > hi + 1e-5f) mono = false;
            }
        check(same_set, "k no cambia QUÉ píxeles son borde ni qué índice gana (solo cuánto)");
        check(half_only, "k = 0: toda cobertura de borde queda en 0,5 (pixel art redondeado)");
        check(mono, "k = 0,5: la cobertura queda entre 0,5 y la de k = 1");
    }

    std::printf("\n%d checks, %d fallos\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
