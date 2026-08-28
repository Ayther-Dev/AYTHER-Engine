#version 450
// ---------------------------------------------------------------------------
// sprite_mask.frag — sprite.frag + Vestuario: la máscara de TINTE del asset
// BASE de una pose (binding 1, R8) modula CUÁNTO del tinte E1 cromático llega
// a cada píxel.
//
//   blanco (1) → tinte completo, igual que sprite.frag — la ropa sigue el
//                palette swap del juego.
//   negro  (0) → sólo la LUMA del tinte — la piel/cara acompaña los fundidos
//                de pantalla (que bajan el brillo de toda la paleta) pero no
//                cambia de matiz. Congelar también el brillo dejaba la cara a
//                plena luz sobre una pantalla fundida a negro (decisión de
//                producto 2026-08-18).
//
// La máscara se muestrea con el MISMO frag_uv que el asset: los sub-rects de
// los quads por grupo de paleta () la recortan igual, y el pre-flip va en
// la textura (la máscara se carga volteada con la misma cara que el asset).
//
// Sólo lo usan los quads CON máscara (pipeline de 2 samplers); el resto sigue
// por sprite.frag, que queda byte-idéntico para los oráculos GPU.
// ---------------------------------------------------------------------------

layout(location = 0) in  vec2  frag_uv;
layout(location = 1) in  vec3  frag_tint;
layout(location = 2) in  float frag_alpha;
layout(location = 0) out vec4  out_color;

layout(set = 0, binding = 0) uniform sampler2D sprite_tex;
layout(set = 0, binding = 1) uniform sampler2D mask_tex;   // R8: 0 = sólo luma · 1 = tinte completo

void main() {
    vec4  c = texture(sprite_tex, frag_uv);
    float m = texture(mask_tex, frag_uv).r;
    // Luma Rec.601 del tinte (mismos pesos que el peak-hold del motor): lo que
    // el fundido oscurece, lo oscurece PAREJO; el matiz sólo pasa por la ropa.
    float l = dot(frag_tint, vec3(0.299, 0.587, 0.114));
    out_color = vec4(c.rgb * mix(vec3(l), frag_tint, m), c.a * frag_alpha);
}
