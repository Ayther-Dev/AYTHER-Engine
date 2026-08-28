#version 450
// ---------------------------------------------------------------------------
// sprite_mult.frag — blend MULTIPLICATIVO del Acetato ().
//
// El pipeline que usa este shader mezcla con srcColor=DST_COLOR · dstColor=ZERO:
//     out = dst · src
// y este shader emite src = mix(1, c·tint, fuerza) — el mix hacia BLANCO por
// la fuerza (alpha del PNG × opacidad del quad). Eso da el multiply clásico
// con fuerza regulable:
//     out = dst · mix(1, tinted, a·op)
// fuerza 0 = identidad (blanco multiplica por 1) · fuerza 1 = multiply pleno.
// El mix hacia blanco NO se puede expresar con factores de blend fijos sin
// premultiplicar — por eso el frag propio; sprite.frag no se toca y los
// oráculos del camino clásico quedan byte-exactos por construcción.
//
// El tinte E1 cromático multiplica ANTES del mix (la mancha tintada por la
// CRAM sigue los fundidos del juego, ). El alpha de salida es 1: el
// pipeline conserva el alpha del offscreen (ZERO/ONE), como todos.
// ---------------------------------------------------------------------------

layout(location = 0) in  vec2  frag_uv;
layout(location = 1) in  vec3  frag_tint;
layout(location = 2) in  float frag_alpha;
layout(location = 0) out vec4  out_color;

layout(set = 0, binding = 0) uniform sampler2D sprite_tex;

void main() {
    vec4  c      = texture(sprite_tex, frag_uv);
    float fuerza = c.a * frag_alpha;
    out_color = vec4(mix(vec3(1.0), c.rgb * frag_tint, fuerza), 1.0);
}
