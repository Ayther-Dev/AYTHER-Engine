#version 450
// ---------------------------------------------------------------------------
// video.frag — I420 → RGB para el frame de la Cinemática ().
//
// POR QUÉ ACÁ Y NO EN CPU. La conversión I420→BGRA en CPU era el gasto MAYOR de
// todo el reproductor: medido 12,5 de los 20,3 ms por frame en 2304×2016 (62%),
// más 17,7 MB de buffer que eran la residencia entera del clip. Acá sale gratis
// —el sampleo ya se paga— y de paso se suben 1,5 bytes por píxel en vez de 4.
//
// LA FÓRMULA ES UN CONTRATO. Tiene que dar EXACTAMENTE lo mismo que
// `ayther::video_i420_to_bgra_px` (`include/ayther/ayther_video.h`), que es su
// especificación ejecutable: BT.601 limited range con los mismos coeficientes
// enteros. tools/video_shader_smoke compara los dos por píxel; si se separan,
// el video cambia de color y nadie se entera hasta verlo.
//
// POR QUÉ `floor(x/256)` Y NO `x >> 8`. En GLSL el corrimiento a derecha de un
// entero con signo NEGATIVO es implementation-defined (§5.9), y acá el numerador
// es negativo con frecuencia — un píxel oscuro con croma bajo lo consigue. La
// división entera hacia abajo es lo que el `>>8` de la referencia hace en C++, y
// es exacta en float: el numerador queda dentro de ±190.000, muy por debajo de
// los 24 bits de mantisa, así que no hay redondeo que romper la igualdad.
//
// EL CROMA CAMBIÓ DE VECINO A BILINEAL. La versión de CPU replicaba croma
// (`x >> 1`); acá lo sampleá el hardware con el sampler LINEAR de la textura, que
// interpola. Es MEJOR (transiciones de color más suaves) pero es una diferencia
// real: la igualdad por píxel con la referencia sólo vale con croma constante o
// a escala de croma, y así la verifica el oráculo.
// ---------------------------------------------------------------------------

layout(location = 0) in  vec2  frag_uv;
layout(location = 1) in  vec3  frag_tint;
layout(location = 2) in  float frag_alpha;
layout(location = 0) out vec4  out_color;

layout(set = 0, binding = 0) uniform sampler2D plane_y;
layout(set = 0, binding = 1) uniform sampler2D plane_u;
layout(set = 0, binding = 2) uniform sampler2D plane_v;

// x/256 redondeando hacia abajo — el `>> 8` de la referencia, sin depender del
// comportamiento indefinido del corrimiento con signo.
int shr8(int x) { return int(floor(float(x) / 256.0)); }

void main() {
    // R8_UNORM entrega [0,1]; el +0.5 antes de truncar recupera el entero exacto
    // (255 valores discretos, sin ambigüedad de redondeo).
    int Y = int(texture(plane_y, frag_uv).r * 255.0 + 0.5);
    int U = int(texture(plane_u, frag_uv).r * 255.0 + 0.5);
    int V = int(texture(plane_v, frag_uv).r * 255.0 + 0.5);

    int C = Y - 16;
    int D = U - 128;
    int E = V - 128;

    int r = shr8(298 * C + 409 * E + 128);
    int g = shr8(298 * C - 100 * D - 208 * E + 128);
    int b = shr8(298 * C + 516 * D + 128);

    vec3 rgb = vec3(float(clamp(r, 0, 255)),
                    float(clamp(g, 0, 255)),
                    float(clamp(b, 0, 255))) / 255.0;

    // El tinte cromático y la opacidad son los del atenuado de capa, igual que
    // en sprite.frag: el video es contenido de la escena y se atenúa como el
    // resto cuando se está autorando otra capa. El video es OPACO, así que el
    // alpha sale de frag_alpha y no de una textura.
    out_color = vec4(rgb * frag_tint, frag_alpha);
}
