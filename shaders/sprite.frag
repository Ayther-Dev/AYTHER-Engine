#version 450
// ---------------------------------------------------------------------------
// sprite.frag — samples the HD sprite texture.
//
// The alpha channel from the PNG is passed through unchanged.  Alpha blending
// (src_alpha / one_minus_src_alpha) is configured in the pipeline's
// VkPipelineColorBlendAttachmentState — the shader outputs the RGBA modulado
// por el tinte E1 CROMÁTICO: rgb *= frag_tint para que el HD siga los fades Y
// los cambios de color de la CRAM (un flash naranja tinta el HD; un tinte >1
// lo abrilanta y satura hacia el flash).
//
// El alpha de la textura se multiplica por `frag_alpha`, la OPACIDAD del quad:
// 1 = el alpha del PNG tal cual (el blend no cambia). Lo mueve el ATENUADO de
// capa () — lo que no se está autorando se compone al 75%.
// ---------------------------------------------------------------------------

layout(location = 0) in  vec2  frag_uv;
layout(location = 1) in  vec3  frag_tint;
layout(location = 2) in  float frag_alpha;
layout(location = 0) out vec4  out_color;

layout(set = 0, binding = 0) uniform sampler2D sprite_tex;

void main() {
    vec4 c = texture(sprite_tex, frag_uv);
    out_color = vec4(c.rgb * frag_tint, c.a * frag_alpha);   // E1 cromático + opacidad
}
