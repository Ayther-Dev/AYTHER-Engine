#version 450
// ---------------------------------------------------------------------------
// indexed_plane.vert — quad de un tile de plano del pipeline INDEXADO (R-2).
//
// Igual que sprite.vert, sin vertex buffer: 2 triángulos desde gl_VertexIndex.
// El quad es SIEMPRE 8×8 px de canvas (un tile del VDP); el push constant
// lleva el destino, el patrón y los atributos. Los flips se aplican acá
// espejando la coordenada local (el frag hace floor() sobre el interpolado:
// para hflip u va de 8→0 y floor cae exactamente en 7-c — sin off-by-one).
//
// Push constants — MUST match struct PC + pc_range en vk_indexed_plane.cpp:
//   x, y                 — esquina sup-izq del tile en px del canvas (float)
//   w, h                 — tamaño del quad en px del canvas (R-5: el compose
//                          escala 8×8 nativos al canvas — p.ej. ×6 en 1920)
//   canvas_w, canvas_h   — dimensiones del canvas para el mapeo a NDC (float)
//   tile                 — índice de patrón 0..2047 (uint)
//   attr                 — bit0 hflip · bit1 vflip · bits 2-3 línea de paleta ·
//                          bit4 = EPX/Scale2x sobre índices (, sólo frag)
//   fx                   — R-6: tinte Q2.6 r|g<<8|b<<16 (64=neutro) + opacidad<<24
//   flat_rgba            — R-6: silueta — color plano AABBGGRR donde idx≠0
//                          (0 = modo normal). El PC es VERTEX|FRAGMENT: el
//                          frag lee fx/flat directo del mismo bloque.
//   checker              — R-8: bit0 = modo checker · bit1 = par de colores ·
//                          bits 8-19/20-31 = offset local del quad dentro del
//                          ELEMENTO en px de emu (ancla el patrón al elemento)
//   nb0..nb3             —  v2/v3: los 8 tiles vecinos del xBR (sólo frag)
// ---------------------------------------------------------------------------

layout(push_constant) uniform PC {
    float x;
    float y;
    float w;
    float h;
    float canvas_w;
    float canvas_h;
    uint  tile;
    uint  attr;
    uint  fx;
    uint  flat_rgba;
    uint  checker;
    uint  nb0;        //  v2: vecinos izq | der<<14 (14 bits c/u, sólo frag)
    uint  nb1;        //  v2: vecinos arriba | abajo<<14
    uint  nb2;        //  v3: diagonales arriba-izq | arriba-der<<14
    uint  nb3;        //  v3: diagonales abajo-izq | abajo-der<<14
} pc;

layout(location = 0)      out vec2  v_local;  // 0..8 dentro del tile, ya flipeado
layout(location = 1) flat out ivec2 v_base;   // texel base del tile en u_index
layout(location = 2) flat out int   v_pal;    // línea de paleta 0..3

// Dos triángulos CCW formando el quad unitario (mismo layout que sprite.vert).
const vec2 kQuad[6] = vec2[6](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
    vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
);

void main() {
    vec2 uv    = kQuad[gl_VertexIndex];
    vec2 local = uv * 8.0;
    if ((pc.attr & 1u) != 0u) local.x = 8.0 - local.x;
    if ((pc.attr & 2u) != 0u) local.y = 8.0 - local.y;
    v_local = local;

    // La textura de índices es 512×256: 64 tiles por fila, 8×8 texels cada uno.
    v_base = ivec2(int(pc.tile % 64u) * 8, int(pc.tile / 64u) * 8);
    v_pal  = int((pc.attr >> 2) & 3u);

    float px = pc.x + uv.x * pc.w;
    float py = pc.y + uv.y * pc.h;
    gl_Position = vec4((px / pc.canvas_w) * 2.0 - 1.0,
                       (py / pc.canvas_h) * 2.0 - 1.0, 0.0, 1.0);
}
