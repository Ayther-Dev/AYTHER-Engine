#version 450
// ---------------------------------------------------------------------------
// sprite_screen.frag — blend PANTALLA (screen) del Acetato ().
//
// El pipeline que usa este shader mezcla con srcColor=ONE_MINUS_DST_COLOR ·
// dstColor=ONE:
//     out = src·(1−dst) + dst
// y este shader emite src = c·tint·fuerza — premultiplicado hacia NEGRO por
// la fuerza (alpha del PNG × opacidad del quad). La identidad es exacta:
//     dst + f·c·(1−dst) = mix(dst, screen(dst,c), f)
// así que la fuerza interpola el efecto sin fórmula aparte. fuerza 0 =
// identidad (negro no aclara nada) · fuerza 1 = screen pleno — satura suave
// donde el aditivo () revienta a blanco.
//
// El tinte E1 cromático multiplica antes (). El alpha de salida es 1: el
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
    out_color = vec4(c.rgb * frag_tint * fuerza, 1.0);
}
