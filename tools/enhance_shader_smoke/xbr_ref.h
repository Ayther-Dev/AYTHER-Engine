#pragma once
// ---------------------------------------------------------------------------
// xbr_ref.h — REFERENCIA en CPU del filtro «Mejorar por software» (#493) que
// implementa indexed_plane.frag en la rama `pc.attr & 16`: xBR lv1/lv2/lv3
// sobre ÍNDICES de paleta + anti-muesca (#500) + cobertura analítica + silueta
// con alpha. Es el CONTRATO del shader escrito en C++: un cambio en el .frag
// que se aparte de estas reglas rompe el smoke (#504).
//
// Lo que NO defiende (declarado, como pide [[gpu-oracle-shader-vs-referencia]]):
//   · el valor exacto de fwidth(): acá w = sqrt(2)/escala (quad a escala entera)
//     (length de (1/escala, 1/escala)); la GPU lo deriva por diferencias finitas — por eso la tolerancia de
//     1-2 LSB en los píxeles de borde.
//   · el redondeo UNORM de la mezcla en paleta.
// Todo lo que es DISCRETO (qué índice gana, dónde hay borde, qué lado es
// opaco) tiene que coincidir exacto.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace xbr_ref {

/// Vecino de pantalla tal como viaja en el push constant (14 bits):
/// 0-10 patrón · 11 hflip · 12 vflip · 13 válido.
struct Neighbor {
    bool    valid = false;
    bool    hf = false, vf = false;
    const uint8_t* idx = nullptr;   ///< 64 índices, fila mayor, SIN flip (VRAM)
};

/// Tile bajo prueba + anillo 3×3 (orden del shader: nb0 = izq|der, nb1 =
/// arriba|abajo, nb2 = arriba-izq|arriba-der, nb3 = abajo-izq|abajo-der).
struct Tile {
    const uint8_t* idx = nullptr;   ///< 64 índices SIN flip
    bool hf = false, vf = false;    ///< attr bits 0/1 del tile central
    Neighbor left, right, up, down, ul, ur, dl, dr;
};

/// Resultado por píxel destino: índice base, índice de mezcla (0 = ninguno),
/// cobertura de la mezcla y alpha de silueta. `discard` = no se escribe.
struct Out {
    bool     discard   = true;
    uint8_t  idx       = 0;
    uint8_t  blend     = 0;
    float    cov       = 0.0f;
    float    alpha     = 1.0f;
};

inline uint32_t dI(uint32_t a, uint32_t b) { return a == b ? 0u : 1u; }

/// fetch_screen del shader: `sp` en coords de PANTALLA del tile actual
/// (∈ [-2,9]); fuera del tile resuelve contra el vecino con SUS flips; sin
/// vecino válido devuelve `center` (clamp v1).
inline uint32_t fetch_screen(const Tile& T, int sx, int sy, uint32_t center) {
    const int tx = sx < 0 ? -1 : (sx > 7 ? 1 : 0);
    const int ty = sy < 0 ? -1 : (sy > 7 ? 1 : 0);
    const uint8_t* base = T.idx;
    bool nhf = T.hf, nvf = T.vf;
    if (tx != 0 || ty != 0) {
        const Neighbor* nb = nullptr;
        if      (ty == 0) nb = tx < 0 ? &T.left : &T.right;
        else if (tx == 0) nb = ty < 0 ? &T.up   : &T.down;
        else if (ty <  0) nb = tx < 0 ? &T.ul   : &T.ur;
        else              nb = tx < 0 ? &T.dl   : &T.dr;
        if (!nb->valid) return center;
        base = nb->idx; nhf = nb->hf; nvf = nb->vf;
        sx -= tx * 8; sy -= ty * 8;
    }
    const int tX = nhf ? 7 - sx : sx, tY = nvf ? 7 - sy : sy;
    return base[tY * 8 + tX];
}

/// Un píxel DESTINO del quad escalado ×`scale`: (dx, dy) ∈ [0, 8·scale).
/// `v_local` = centro del píxel destino en texels del tile (igual que la
/// interpolación del quad: local = (d + 0.5) / scale).
/// `k` (#503): intensidad 0..1 del suavizado — cov' = mix(0.5, cov, k).
/// k = 1 vector limpio (v6) · k → 0 pixel art apenas redondeado (cada
/// borde queda al 50 %).
inline Out shade(const Tile& T, int dx, int dy, int scale, float k = 1.0f) {
    Out o;
    const float lx = (dx + 0.5f) / scale, ly = (dy + 0.5f) / scale;
    // |fwidth(v_local)| del shader: v_local recorre 0..8 en 8·scale px por
    // eje => fwidth = (1/scale, 1/scale) y length() = sqrt(2)/scale.
    const float frag_w = 1.41421356f / scale;
    const int   tx = std::clamp((int)lx, 0, 7), ty = std::clamp((int)ly, 0, 7);
    uint32_t idx = T.idx[ty * 8 + tx];

    uint32_t blend_with = 0; float blend_cov = 0.0f, edge_alpha = 1.0f;
    {
        const bool hf = T.hf, vf = T.vf;
        const int sX = hf ? 7 - tx : tx, sY = vf ? 7 - ty : ty;
        float fx = lx - std::floor(lx), fy = ly - std::floor(ly);
        if (hf) fx = 1.0f - fx;
        if (vf) fy = 1.0f - fy;
        const int sgx = fx < 0.5f ? -1 : 1, sgy = fy < 0.5f ? -1 : 1;
        auto PX = [&](int i, int j) {
            return fetch_screen(T, sX + i * sgx, sY + j * sgy, idx);
        };
        const uint32_t E = idx;
        const uint32_t B = PX(0,-1), D = PX(-1,0), F = PX(1,0), H = PX(0,1);
        const uint32_t C = PX(1,-1), G = PX(-1,1), I = PX(1,1);
        const uint32_t F4 = PX(2,0), I4 = PX(2,1), H5 = PX(0,2), I5 = PX(1,2);
        const uint32_t G0 = PX(-2,1), D0 = PX(-2,0), C1 = PX(1,-2), B1 = PX(0,-2);
        const uint32_t e = dI(E,C) + dI(E,G) + dI(I,H5) + dI(I,F4) + 4u * dI(H,F);
        const uint32_t i = dI(H,D) + dI(H,I5) + dI(F,I4) + dI(F,B) + 4u * dI(E,I);
        const bool thin = (D != E && F != E) || (B != E && H != E);
        if (e < i && !thin) {
            const uint32_t nw = (dI(E,F) <= dI(E,H)) ? F : H;
            if (nw != E) {
                const uint32_t ke = dI(F,G), ki = dI(H,C);
                const bool ex2 = (E != C) && (B != C);
                const bool ex3 = (E != G) && (D != G);
                const bool left = (2u * ke <= ki) && ex3;
                const bool up   = (ke >= 2u * ki) && ex2;
                const uint32_t ke3 = dI(F,G0), ki3 = dI(H,C1);
                const bool left3 = left && (2u * ke3 <= ki3) && (E != G0) && (D0 != G0);
                const bool up3   = up   && (ke3 >= 2u * ki3) && (E != C1) && (B1 != C1);
                const float px = sgx < 0 ? 1.0f - fx : fx, py = sgy < 0 ? 1.0f - fy : fy;
                const float w  = frag_w;
                const float d1 = (px + py - 1.5f) / 1.41421356f;
                const float dl = left3 ? (px / 3.0f + py - 0.83333333f) / 1.05409255f
                                       : (0.5f * px + py - 1.0f) / 1.11803399f;
                const float du = up3   ? (px + py / 3.0f - 0.83333333f) / 1.05409255f
                                       : (px + 0.5f * py - 1.0f) / 1.11803399f;
                float dd = d1;
                if (left && up) dd = std::max(dl, du);
                else if (left)  dd = dl;
                else if (up)    dd = du;
                const float cov = 0.5f + (std::clamp(0.5f + dd / w, 0.0f, 1.0f) - 0.5f) * k;
                if (E == 0u || nw == 0u) {
                    const float a = (E == 0u) ? cov : 1.0f - cov;
                    if (E == 0u) idx = nw;
                    edge_alpha = a;
                    if (a <= 0.002f) { o.discard = true; return o; }
                } else {
                    blend_with = nw; blend_cov = cov;
                }
            }
        }
    }
    if (idx == 0u) { o.discard = true; return o; }
    o.discard = false;
    o.idx   = (uint8_t)idx;
    o.blend = (uint8_t)blend_with;
    o.cov   = blend_cov;
    o.alpha = edge_alpha;
    return o;
}

/// Color esperado (RGB 0-255) con la paleta `pal[16][3]`, y alpha 0-255.
/// Mezcla lineal en 8 bits como el mix() del shader sobre UNORM.
inline void resolve(const Out& o, const uint8_t (*pal)[3], uint8_t rgba[4]) {
    if (o.discard) { rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0; return; }
    for (int c = 0; c < 3; ++c) {
        float v = pal[o.idx][c];
        if (o.blend) v = v + (pal[o.blend][c] - v) * o.cov;
        rgba[c] = (uint8_t)std::lround(std::clamp(v, 0.0f, 255.0f));
    }
    rgba[3] = (uint8_t)std::lround(std::clamp(o.alpha * 255.0f, 0.0f, 255.0f));
}

}  // namespace xbr_ref
