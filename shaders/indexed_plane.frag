#version 450
// ---------------------------------------------------------------------------
// indexed_plane.frag — lookup índice → color del pipeline INDEXADO (R-2).
//
// u_index: VRAM desempaquetada a nibbles (R8_UINT, 512×256 — 64 tiles/fila).
// u_pal  : CRAM como paleta (64×1 RGBA8, ya convertida a 888 con la MISMA
//          expansión que el renderer del core → comparable bit a bit).
//
// El índice 0 es transparente en el VDP (deja ver lo de atrás): discard, sin
// blending — cada pixel escrito es EXACTAMENTE el color de la paleta.
// texelFetch en ambas texturas: sin filtrado, sin normalizar coordenadas.
// ---------------------------------------------------------------------------

layout(push_constant) uniform PC {
    float x;
    float y;
    float w;
    float h;
    float canvas_w;
    float canvas_h;
    uint  tile;
    uint  attr;       // 0-1 flips · 2-3 paleta · 4 enhance · 5-12 k ()
    uint  fx;         // R-6: tinte Q2.6 r|g<<8|b<<16 + opacidad<<24
    uint  flat_rgba;  // R-6: silueta AABBGGRR (0 = normal)
    uint  checker;    // R-8: bit0 on · bit1 alerta · bits 2-3 capa ·
                      //      bits 8-19/20-31 offset local
    uint  nb0;        //  v2: vecinos de PANTALLA izq | der<<14 — 14 bits:
                      //      0-10 patrón · 11 hflip · 12 vflip · 13 válido
    uint  nb1;        //  v2: arriba | abajo<<14
    uint  nb2;        //  v3: arriba-izq | arriba-der<<14
    uint  nb3;        //  v3: abajo-izq | abajo-der<<14
} pc;

layout(set = 0, binding = 0) uniform usampler2D u_index;
layout(set = 0, binding = 1) uniform sampler2D  u_pal;

layout(location = 0)      in vec2  v_local;
layout(location = 1) flat in ivec2 v_base;
layout(location = 2) flat in int   v_pal;

layout(location = 0) out vec4 o_color;

//  v2/v3: índice del píxel en coordenadas de PANTALLA del tile actual,
// sp ∈ [-2,9] en cada eje (alcance de xBR). Fuera del tile se resuelve contra
// el tile vecino que vino en el push constant (anillo 3×3) con SUS flips —
// así un tile espejado al lado de uno normal sigue siendo adyacente píxel a
// píxel. Sin vecino válido se clampea al tile actual (seam en el borde del
// ELEMENTO; dentro del elemento no hay seams).
uint fetch_screen(ivec2 sp, bool hf, bool vf, uint center) {
    int tx = (sp.x < 0) ? -1 : (sp.x > 7 ? 1 : 0);
    int ty = (sp.y < 0) ? -1 : (sp.y > 7 ? 1 : 0);
    ivec2 base = v_base;
    bool  nhf = hf, nvf = vf;
    if (tx != 0 || ty != 0) {
        uint nb;
        if      (ty == 0) nb = (tx < 0) ? (pc.nb0 & 0x3FFFu) : ((pc.nb0 >> 14) & 0x3FFFu);
        else if (tx == 0) nb = (ty < 0) ? (pc.nb1 & 0x3FFFu) : ((pc.nb1 >> 14) & 0x3FFFu);
        else if (ty <  0) nb = (tx < 0) ? (pc.nb2 & 0x3FFFu) : ((pc.nb2 >> 14) & 0x3FFFu);
        else              nb = (tx < 0) ? (pc.nb3 & 0x3FFFu) : ((pc.nb3 >> 14) & 0x3FFFu);
        if ((nb & 0x2000u) == 0u) return center;   // sin vecino: clamp (v1)
        uint pat = nb & 0x7FFu;
        base = ivec2(int(pat % 64u) * 8, int(pat / 64u) * 8);
        nhf  = (nb & 0x800u)  != 0u;
        nvf  = (nb & 0x1000u) != 0u;
        sp  -= ivec2(tx, ty) * 8;
    }
    ivec2 tt = ivec2(nhf ? 7 - sp.x : sp.x, nvf ? 7 - sp.y : sp.y);
    return texelFetch(u_index, base + tt, 0).r;
}

// Distancia entre índices: iguales = 0, distintos = 1. Sobre ÍNDICES (no
// color) el filtro ve el dibujo real y no el resultado de un fundido.
uint dI(uint a, uint b) { return (a == b) ? 0u : 1u; }

void main() {
    // Ancho del fragmento en texels (v4): las derivadas van ANTES de cualquier
    // rama divergente (dentro de un `if` no uniforme son indefinidas).
    float frag_w = max(length(fwidth(v_local)), 1e-4);
    ivec2 t   = clamp(ivec2(v_local), ivec2(0), ivec2(7));
    uint  idx = texelFetch(u_index, v_base + t, 0).r;

    //  (runtime_enhancement): «Mejorar por software» — EPX/Scale2x sobre
    // ÍNDICES de paleta (igualdad de índice, no de color: así el filtro ve la
    // silueta real del dibujo y no el resultado de un fundido). Por píxel
    // destino: (1) cuadrante por la fracción de v_local (el quad escala 8×8
    // nativos al canvas, p.ej. ×6), (2) centro + 4 vecinos clampeados AL TILE
    // (v1: el seam de 8 px en el borde está documentado en el spec; los
    // vecinos inter-tile son el plan v2 vía push constants), (3) regla de
    // Scale2x: si los dos vecinos del cuadrante coinciden y no hay mayoría en
    // contra (arriba≠abajo y izq≠der), gana ese índice; si no, el centro.
    // El índice elegido pasa por el MISMO discard: la silueta también se
    // suaviza. Sin el bit la rama no se toca → camino byte-idéntico.
    //  v3: xBR nivel 1 sobre ÍNDICES, en espacio de PANTALLA. Por
    // cuadrante del píxel destino (sx, sy = ±1) se evalúa la esquina
    // correspondiente del 2xBR clásico (Hyllian), con el vecindario
    // reflejado por los signos:
    //        A1 B1 C1
    //     A0 A  B  C  C4
    //     D0 D  E  F  F4
    //     G0 G  H  I  I4
    //        G5 H5 I5
    //   e = d(E,C)+d(E,G)+d(I,H5)+d(I,F4)+4·d(H,F)   (borde por la diagonal)
    //   i = d(H,D)+d(H,I5)+d(F,I4)+d(F,B)+4·d(E,I)   (no hay borde)
    //   e < i → borde: el sub-píxel toma F o H (el más parecido a E).
    // Salida: si E y el elegido son ambos opacos, 50 % de mezcla (el
    // anti-alias del xBR); si uno es transparente, gana el elegido en duro
    // (así la silueta también se suaviza y el discard sigue valiendo).
    uint  blend_with = 0u;   // 0 = sin borde
    float blend_cov  = 0.0;  // v4: cobertura del vecino en este fragmento
    float edge_alpha = 1.0;  // v6: silueta anti-aliasada (alpha = cobertura del lado opaco)
    if ((pc.attr & 16u) != 0u) {
        bool  hf = (pc.attr & 1u) != 0u, vf = (pc.attr & 2u) != 0u;
        ivec2 s  = ivec2(hf ? 7 - t.x : t.x, vf ? 7 - t.y : t.y);
        vec2  f  = fract(v_local);
        if (hf) f.x = 1.0 - f.x;
        if (vf) f.y = 1.0 - f.y;
        ivec2 sg = ivec2(f.x < 0.5 ? -1 : 1, f.y < 0.5 ? -1 : 1);   // cuadrante
        #define PX(i, j) fetch_screen(s + ivec2((i) * sg.x, (j) * sg.y), hf, vf, idx)
        uint E  = idx;
        uint B  = PX( 0, -1), D  = PX(-1,  0), F  = PX( 1,  0), H  = PX( 0,  1);
        uint C  = PX( 1, -1), G  = PX(-1,  1), I  = PX( 1,  1);
        uint F4 = PX( 2,  0), I4 = PX( 2,  1), H5 = PX( 0,  2), I5 = PX( 1,  2);
        uint G0 = PX(-2,  1), D0 = PX(-2,  0), C1 = PX( 1, -2), B1 = PX( 0, -2);   // v5: lv3
        #undef PX
        uint e = dI(E, C) + dI(E, G) + dI(I, H5) + dI(I, F4) + 4u * dI(H, F);
        uint i = dI(H, D) + dI(H, I5) + dI(F, I4) + dI(F, B) + 4u * dI(E, I);
        //  — anti-muesca: si E es una LÍNEA de 1 px (sus dos vecinos
        // opuestos son ambos distintos de él, en X o en Y) no se dispara el
        // borde — xBR la muescaría en las diagonales (el contorno blanco del
        // logo SEGA). La línea queda íntegra aunque sin suavizar; un píxel de
        // un trazo grueso nunca cumple la condición (tiene un vecino igual en
        // cada eje), así que el resto del filtro no cambia.
        bool thin = (D != E && F != E) || (B != E && H != E);
        if (e < i && !thin) {               // borde: el sub-píxel toma F o H
            uint nw = (dI(E, F) <= dI(E, H)) ? F : H;   // el más parecido a E
            if (nw != E) {
                // v4 — xBR nivel 2: pendientes 2:1. ke/ki miden si el borde
                // continúa horizontal (F==G) o vertical (H==C); ex2/ex3 evitan
                // los falsos positivos del lv2 original (Hyllian, coef. 2).
                uint ke = dI(F, G), ki = dI(H, C);
                bool ex2 = (E != C) && (B != C);
                bool ex3 = (E != G) && (D != G);
                bool left = (2u * ke <= ki) && ex3;   // borde tendido en X
                bool up   = (ke >= 2u * ki) && ex2;   // borde tendido en Y
                // v5 — xBR nivel 3: pendientes 3:1. El borde sigue UN píxel
                // más (F==G0 hacia la izquierda, H==C1 hacia arriba) con sus
                // propias restricciones (Hyllian xbr-lv3). Sólo cuenta si el
                // nivel 2 ya disparó en esa dirección.
                uint ke3 = dI(F, G0), ki3 = dI(H, C1);
                bool left3 = left && (2u * ke3 <= ki3) && (E != G0) && (D0 != G0);
                bool up3   = up   && (ke3 >= 2u * ki3) && (E != C1) && (B1 != C1);
                // v4 — COBERTURA ANALÍTICA: el borde es una recta en el píxel
                // fuente (origen arriba-izq de E, esquina del cuadrante en
                // (1,1)); cada fragmento mezcla según su distancia a ella, así
                // a ×6 el borde es continuo y no dos tonos.
                //   lv1    : (0.5,1)-(1,0.5)   x + y = 1.5
                //   left   : (0,1)-(1,0.5)     0.5x + y = 1
                //   up     : (1,0)-(0.5,1)     x + 0.5y = 1
                //   left3  : por (1,0.5) con pendiente 1/3   x/3 + y = 5/6
                //   up3    : por (0.5,1) con pendiente 3     x + y/3 = 5/6
                // Todas pasan por el punto medio del borde que cortan, así los
                // niveles se encadenan sin saltos. Con «left» y «up» a la vez
                // vale la unión de los dos cortes.
                vec2  p  = vec2(sg.x < 0 ? 1.0 - f.x : f.x, sg.y < 0 ? 1.0 - f.y : f.y);
                float w  = frag_w;                          // ancho del fragmento
                float d1 = (p.x + p.y - 1.5) / 1.41421356;
                float dl = left3 ? (p.x / 3.0 + p.y - 0.83333333) / 1.05409255
                                 : (0.5 * p.x + p.y - 1.0) / 1.11803399;
                float du = up3   ? (p.x + p.y / 3.0 - 0.83333333) / 1.05409255
                                 : (p.x + 0.5 * p.y - 1.0) / 1.11803399;
                float dd = d1;
                if (left && up)  dd = max(dl, du);
                else if (left)   dd = dl;
                else if (up)     dd = du;
                float cov = clamp(0.5 + dd / w, 0.0, 1.0);
                //  — INTENSIDAD k (0..1): cov' = mix(0.5, cov, k). k = 1 es
                // el vector limpio de v6; k → 0 deja cada borde al 50 % (pixel
                // art apenas redondeado). Misma fórmula que xbr_ref.h.
                float k = float((pc.attr >> 5) & 0xFFu) / 255.0;
                cov = 0.5 + (cov - 0.5) * k;
                if (E == 0u || nw == 0u) {
                    // v6 — SILUETA anti-aliasada: el lado opaco se dibuja con
                    // alpha = su cobertura (el pipeline blendea SRC_ALPHA, el
                    // mismo camino que la opacidad de R-6). Si el opaco es E,
                    // su cobertura es 1-cov; si es el vecino, cov. Con alpha 0
                    // se descarta (mismo efecto que el índice 0).
                    float a = (E == 0u) ? cov : 1.0 - cov;
                    if (E == 0u) idx = nw;
                    edge_alpha = a;
                    if (a <= 0.002) discard;
                } else {                              // interior: anti-alias analítico
                    blend_with = nw; blend_cov = cov;
                }
            }
        }
    }
    if (idx == 0u) discard;   // semántica del VDP: el color 0 no se dibuja

    // R-8: modo UV checker — TABLERO DE AJEDREZ anclado al ELEMENTO, con el
    // índice alfanumérico de cada casilla en el centro (formato de la
    // referencia que pasó el usuario, 2026-08-06). Respeta la silueta (el
    // discard de arriba) y el flip se DESHACE: el ancla es la posición en
    // pantalla, no el texel.
    //
    // CASILLA = 1 TILE del VDP (8 px). El offset local del quad dentro del
    // elemento viene en el push constant, así que fila/columna salen de
    // dividirlo por 8: un tile suelto es siempre A00 —eso ES la señal de que
    // está solo— y un elemento grande despliega su grilla completa.
    //
    // PALETA: la del UV checker de Valle, muestreada del PNG de referencia.
    // Cada CAPA lleva su propio par de 2 colores —así se lee de qué plano es lo
    // que falta sin buscar dónde está— y los 8 colores alcanzan justo para las
    // 4 capas. El estado «el asset NO cargó» pisa el par de la capa con uno de
    // ALERTA (rosa+negro, que no es ninguno de los cuatro): cuando un asset no
    // carga lo urgente es eso, y la capa se sigue viendo por dónde está.
    if ((pc.checker & 1u) != 0u) {
        //                      Plano B     Plano A     Window      Sprites     ALERTA
        const vec3 kDark[5] = vec3[5](
            vec3( 11.0,  93.0, 121.0),   // teal
            vec3(207.0,  44.0, 101.0),   // rosa
            vec3(127.0, 127.0, 127.0),   // gris
            vec3( 40.0,  40.0,  40.0),   // negro
            vec3(207.0,  44.0, 101.0));  // rosa
        const vec3 kLite[5] = vec3[5](
            vec3( 94.0, 214.0, 194.0),   // turquesa
            vec3(252.0, 143.0, 116.0),   // salmón
            vec3(214.0, 214.0, 214.0),   // gris claro
            vec3(254.0, 237.0, 170.0),   // amarillo
            vec3( 40.0,  40.0,  40.0));  // negro
        // Fuente 5×7 empaquetada: bit(fila*5 + col), col 0 = izquierda. Son 35
        // bits, así que no entra en un uint: x = bits 0-31, y = 32-34.
        // Orden: 0-9 y luego A-Z (índice 10 = 'A').
        const uvec2 kFont[36] = uvec2[36](
            uvec2(0xA33AE62Eu,0x3u), uvec2(0x884210C4u,0x3u), uvec2(0xC444422Eu,0x7u),   // 012
            uvec2(0xA304111Fu,0x3u), uvec2(0x11F4A988u,0x2u), uvec2(0xA3083C3Fu,0x3u),   // 345
            uvec2(0xA317844Cu,0x3u), uvec2(0x8422221Fu,0x0u), uvec2(0xA317462Eu,0x3u),   // 678
            uvec2(0x910F462Eu,0x1u), uvec2(0x631FC62Eu,0x4u), uvec2(0xE317C62Fu,0x3u),   // 9AB
            uvec2(0xA210862Eu,0x3u), uvec2(0xD318C527u,0x1u), uvec2(0xC217843Fu,0x7u),   // CDE
            uvec2(0x4217843Fu,0x0u), uvec2(0xA31E862Eu,0x7u), uvec2(0x631FC631u,0x4u),   // FGH
            uvec2(0x8842108Eu,0x3u), uvec2(0x9284211Cu,0x1u), uvec2(0x52519531u,0x4u),   // IJK
            uvec2(0xC2108421u,0x7u), uvec2(0x631AD771u,0x4u), uvec2(0x631CD671u,0x4u),   // LMN
            uvec2(0xA318C62Eu,0x3u), uvec2(0x4217C62Fu,0x0u), uvec2(0x9358C62Eu,0x5u),   // OPQ
            uvec2(0x5257C62Fu,0x4u), uvec2(0xE107043Eu,0x3u), uvec2(0x0842109Fu,0x1u),   // RST
            uvec2(0xA318C631u,0x3u), uvec2(0x1518C631u,0x1u), uvec2(0x775AC631u,0x4u),   // UVW
            uvec2(0x62A22A31u,0x4u), uvec2(0x08422A31u,0x1u), uvec2(0xC222221Fu,0x7u)    // XYZ
        );
        vec2 s = v_local;
        if ((pc.attr & 1u) != 0u) s.x = 8.0 - s.x;
        if ((pc.attr & 2u) != 0u) s.y = 8.0 - s.y;
        int lx  = int((pc.checker >>  8) & 0xFFFu);
        int ly  = int((pc.checker >> 20) & 0xFFFu);
        int col = lx / 8;                  // columna de la casilla en el elemento
        int row = ly / 8;                  // fila
        // El ÍNDICE es del elemento (arriba), pero la PARIDAD del tablero va
        // por posición en PANTALLA: en los planos casi todo lo no autorado son
        // celdas SUELTAS —cada una su propio elemento, fila 0 columna 0— y con
        // la paridad del elemento salían todas del mismo color: el tablero de
        // ajedrez desaparecía justo donde más se usa. `pc.x`/`pc.w` son la
        // posición y el ancho del quad en px de pantalla, así que su cociente
        // es la casilla en la grilla de la pantalla (redondeado: con hscroll
        // fino el plano no cae en múltiplos de 8).
        int sc  = int(floor(pc.x / max(pc.w, 1.0) + 0.5));
        int sr  = int(floor(pc.y / max(pc.h, 1.0) + 0.5));
        bool alt  = ((sc + sr) & 1) != 0;
        uint slot = ((pc.checker & 2u) != 0u) ? 4u : ((pc.checker >> 2) & 3u);
        vec3 c = (alt ? kDark[slot] : kLite[slot]) / 255.0;

        // ÍNDICE en el centro: letra(s) de fila + 2 dígitos de columna (D09).
        // Sólo si la casilla se ve grande: `pc.w` es el ancho del quad en px de
        // PANTALLA, así que a 1× (8 px) el texto sería ilegible y ensuciaría el
        // color plano — aparece recién cuando hay resolución para leerlo.
        //
        // El texto NO respeta la grilla de píxeles del emulador: es una ayuda de
        // autoría, no pixel art del juego. Se evalúa en el espacio CONTINUO de
        // la casilla y se antialiasa con 3×3 muestras por fragmento, así que a
        // más resolución se ve más nítido en vez de más pixelado.
        if (pc.w >= 24.0) {
            // Fila >25 pasa a doble letra (AA, AB…), estilo Excel: un Cuadro de
            // pantalla completa tiene 28 filas de tiles y A-Z no alcanza.
            int  ng   = (row < 26) ? 3 : 4;
            // Glifos: [letra(s)] + 2 dígitos.
            int g0 = (row < 26) ? (10 + row) : (10 + (row / 26) - 1);
            int g1 = (row < 26) ? ((col / 10) % 10) : (10 + (row % 26));
            int g2 = (row < 26) ? (col % 10) : ((col / 10) % 10);
            int g3 = (col % 10);
            // Caja centrada: ancho = ng*5 + (ng-1) unidades de fuente, alto 7.
            float cw   = float(6 * ng - 1);
            float uw   = 0.80 / cw;                  // unidad, en fracción de casilla
            float tw   = uw * cw;
            float th   = uw * 7.0;
            vec2  p    = clamp(s / 8.0, vec2(0.0), vec2(0.999));
            vec2  t0   = (p - vec2(0.5 - tw * 0.5, 0.5 - th * 0.5)) / uw;
            // Tamaño del fragmento en unidades de fuente → paso del supersample.
            vec2  fw   = max(fwidth(t0), vec2(1e-4));
            float cov  = 0.0;
            for (int sy = 0; sy < 3; ++sy)
                for (int sx = 0; sx < 3; ++sx) {
                    vec2 t = t0 + (vec2(float(sx), float(sy)) - 1.0) * fw / 3.0;
                    if (t.x < 0.0 || t.y < 0.0 || t.y >= 7.0 || t.x >= cw) continue;
                    int cx  = int(t.x);
                    int gi  = cx / 6;              // 5 de glifo + 1 de gap
                    int inx = cx - gi * 6;
                    if (inx >= 5 || gi >= ng) continue;
                    int  gl   = (gi == 0) ? g0 : (gi == 1) ? g1 : (gi == 2) ? g2 : g3;
                    uvec2 bits = kFont[gl];
                    int  bit  = int(t.y) * 5 + inx;
                    uint word = (bit < 32) ? bits.x : bits.y;
                    uint sh   = uint(bit < 32 ? bit : bit - 32);
                    if ((word & (1u << sh)) != 0u) cov += 1.0;
                }
            cov /= 9.0;
            if (cov > 0.0) {
                // Tinta por CONTRASTE con la casilla, como la referencia:
                // negro sobre claro, blanco sobre oscuro.
                float lum = dot(c, vec3(0.299, 0.587, 0.114));
                c = mix(c, (lum > 0.5) ? vec3(0.0) : vec3(1.0), cov);
            }
        }
        o_color = vec4(c, 1.0);
        return;
    }

    // R-6: silueta de autoría — color plano donde el elemento es opaco.
    if (pc.flat_rgba != 0u) {
        o_color = vec4(float((pc.flat_rgba >>  0) & 0xFFu) / 255.0,
                       float((pc.flat_rgba >>  8) & 0xFFu) / 255.0,
                       float((pc.flat_rgba >> 16) & 0xFFu) / 255.0,
                       float((pc.flat_rgba >> 24) & 0xFFu) / 255.0);
        return;
    }

    vec4 c = texelFetch(u_pal, ivec2(v_pal * 16 + int(idx), 0), 0);
    if (blend_with != 0u)   //  v3/v4: anti-alias analítico del borde xBR
        c = mix(c, texelFetch(u_pal, ivec2(v_pal * 16 + int(blend_with), 0), 0), blend_cov);
    // R-6: tinte multiplicativo Q2.6 (64 = neutro — con fx neutro y opacidad
    // 255 el resultado es BYTE-IDÉNTICO al camino sin efectos) + opacidad.
    vec3 tint = vec3(float((pc.fx >>  0) & 0xFFu),
                     float((pc.fx >>  8) & 0xFFu),
                     float((pc.fx >> 16) & 0xFFu)) / 64.0;
    o_color = vec4(clamp(c.rgb * tint, 0.0, 1.0),
                   float((pc.fx >> 24) & 0xFFu) / 255.0 * edge_alpha);
}
