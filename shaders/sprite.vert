#version 450
// ---------------------------------------------------------------------------
// sprite.vert — screen-space quad generator for HD sprite overlays.
//
// No vertex buffer.  The vertex shader synthesises a 2-triangle quad directly
// from gl_VertexIndex (0-5), using push constants for the destination rect.
//
// Push constants (all floats — MUST match struct PC + pc_range in vk_sprite.cpp):
//   x, y          — top-left corner in *screen* pixels
//   w, h          — width/height in screen pixels
//   scr_w, scr_h  — swapchain dimensions for NDC conversion
//   tr, tg, tb    — E1 cromático: tinte RGB (1 = neutro; sigue fades Y cambios
//                   de color de la CRAM; >1 = flash más brillante, satura)
//   u0, v0        — texture sub-rect origin (normalized UV; 0,0 = whole texture)
//   uw, vh        — texture sub-rect size   (normalized UV; 1,1 = whole texture)
//   a             — OPACIDAD del quad (1 = opaco). La usa el ATENUADO de capa
//                   (): la capa que no se está autorando se compone al 75%
//                   además de oscurecerse, así se ve a través lo que se enfoca.
//
// El sub-rect UV permite muestrear un FRAME de un sheet (Animaciones C-S2,
// draw_anim); los paths de textura entera empujan la identidad (0,0,1,1).
//
// NDC mapping:  pixel(0,0) == NDC(-1,-1),  pixel(scr_w,scr_h) == NDC(+1,+1).
// ---------------------------------------------------------------------------

layout(push_constant) uniform PC {
    float x;      // sprite top-left X in screen pixels
    float y;      // sprite top-left Y in screen pixels
    float w;      // sprite width  in screen pixels
    float h;      // sprite height in screen pixels
    float scr_w;  // swapchain framebuffer width
    float scr_h;  // swapchain framebuffer height
    float tr;     // E1 cromático: tinte R (1 = neutro)
    float tg;     // E1 cromático: tinte G
    float tb;     // E1 cromático: tinte B
    float u0;     // sub-rect UV origin X (normalized)
    float v0;     // sub-rect UV origin Y (normalized)
    float uw;     // sub-rect UV width    (normalized)
    float vh;     // sub-rect UV height   (normalized)
    float a;      // opacidad del quad (1 = opaco; atenuado de capa )
} pc;

layout(location = 0) out vec2  frag_uv;
layout(location = 1) out vec3  frag_tint;
layout(location = 2) out float frag_alpha;

// Two CCW triangles forming a unit quad.
// Vertex layout (screen-space local [0,1]×[0,1]):
//   Tri 0: TL(0) TR(1) BL(2)
//   Tri 1: TR(3) BR(4) BL(5)
const vec2 kQuad[6] = vec2[6](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
    vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
);

void main() {
    vec2 uv = kQuad[gl_VertexIndex];
    frag_uv   = vec2(pc.u0 + uv.x * pc.uw, pc.v0 + uv.y * pc.vh);
    frag_tint  = vec3(pc.tr, pc.tg, pc.tb);   // E1: al fragment (tinta el HD)
    frag_alpha = pc.a;                        // atenuado de capa ()

    // Map local [0,1] → screen pixel position.
    float px = pc.x + uv.x * pc.w;
    float py = pc.y + uv.y * pc.h;

    // Convert screen pixel → NDC.
    float nx = (px / pc.scr_w) * 2.0 - 1.0;
    float ny = (py / pc.scr_h) * 2.0 - 1.0;

    gl_Position = vec4(nx, ny, 0.0, 1.0);
}
