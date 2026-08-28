// ---------------------------------------------------------------------------
// vk_sprite.cpp — Alpha-blended HD sprite overlay.  Ayther v0.8.0
//
// ## Render-pass layout chain (per draw() call)
//
//   TRANSFER_DST_OPTIMAL
//     │  (explicit vkCmdPipelineBarrier before BeginRenderPass)
//     ▼
//   COLOR_ATTACHMENT_OPTIMAL
//     │  (render pass subpass — loadOp=LOAD, alpha blend, storeOp=STORE)
//     ▼
//   TRANSFER_DST_OPTIMAL          ← render pass finalLayout auto-transition
//     │  (VkPresent::finalize — unchanged)
//     ▼
//   PRESENT_SRC_KHR
//
// ## Descriptor layout
//   set 0, binding 0 = combined image sampler  (one set per unique asset)
//
// ## Push constants (vertex stage, 13 × float — matchear sprite.vert)
//   [0] x, [1] y, [2] w, [3] h        — destination rect in screen pixels
//   [4] scr_w, [5] scr_h              — swapchain dimensions for NDC conversion
//   [6] tr, [7] tg, [8] tb            — E1 cromático: tinte RGB (1.0 = neutro)
//   [9] u0, [10] v0, [11] uw, [12] vh — sub-rect UV del sheet (identidad = entera)
// ---------------------------------------------------------------------------

#include "vulkan_backend/vk_sprite.h"
#include "vulkan_backend/vk_context.h"
#include "vulkan_backend/vk_texture.h"
#include "ayther_animation.h"  // ayther::AnimHdFrame (draw_anim, C-S2)
#include "ayther_core_ffi.h"   // AytherSpriteSub, ayther_pack_*

// stb_image declaration only — STB_IMAGE_IMPLEMENTATION is in main.cpp.
#include <stb_image.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>   // SetThreadPriority (worker de decode, )
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <vector>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// The current swapchain VkImage is fetched directly from the cached
// VkSwapchain::images_ via swap.current_image() — no per-frame
// vkGetSwapchainImagesKHR calls needed.

static std::vector<uint32_t> load_spv(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "[VkSprite] Cannot open shader: %s\n", path);
        return {};
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::rewind(f);
    if (sz <= 0 || (sz % 4) != 0) {
        std::fprintf(stderr, "[VkSprite] Bad SPIR-V size (%ld) for: %s\n", sz, path);
        std::fclose(f);
        return {};
    }
    std::vector<uint32_t> code(static_cast<size_t>(sz) / 4);
    std::fread(code.data(), 1, static_cast<size_t>(sz), f);
    std::fclose(f);
    return code;
}

static VkShaderModule make_shader_module(VkContext& ctx,
                                          const std::vector<uint32_t>& code) {
    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size() * sizeof(uint32_t);
    info.pCode    = code.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(ctx.device(), &info, nullptr, &mod);
    return mod;
}

// ---------------------------------------------------------------------------
// VkSprite::create_render_pass
// ---------------------------------------------------------------------------
bool VkSprite::create_render_pass(VkContext& ctx, VkFormat fmt) {
    // Swapchain attachment — preserves background, alpha-blends sprites on top.
    // The final layout is TRANSFER_DST_OPTIMAL so VkPresent::finalize() works.
    VkAttachmentDescription att{};
    att.format         = fmt;
    att.samples        = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;        // keep emulator framebuffer
    att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;      // write blended result
    att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;   // after our barrier
    att.finalLayout    = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;        // auto-transition

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &color_ref;

    // dep_in  — ensure prior transfers are visible before colour writes.
    // dep_out — ensure colour writes are complete before next transfer.
    VkSubpassDependency deps[2]{};

    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    VkRenderPassCreateInfo rp_info{};
    rp_info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_info.attachmentCount = 1;
    rp_info.pAttachments    = &att;
    rp_info.subpassCount    = 1;
    rp_info.pSubpasses      = &subpass;
    rp_info.dependencyCount = 2;
    rp_info.pDependencies   = deps;

    if (vkCreateRenderPass(ctx.device(), &rp_info, nullptr, &render_pass_) != VK_SUCCESS) {
        std::fprintf(stderr, "[VkSprite] vkCreateRenderPass failed\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// VkSprite::create_pipeline
// ---------------------------------------------------------------------------
bool VkSprite::create_pipeline(VkContext& ctx,
                                const char* vert_spv_path,
                                const char* frag_spv_path,
                                uint32_t    sampler_count,
                                VkDescriptorSetLayout* out_layout,
                                VkPipelineLayout*      out_pipe_layout,
                                VkPipeline*            out_pipeline,
                                VkPipeline*            out_pipeline_add,
                                const PipelineBlendVariant* variants,
                                uint32_t variant_count) {
    auto vert_code = load_spv(vert_spv_path);
    auto frag_code = load_spv(frag_spv_path);
    if (vert_code.empty() || frag_code.empty()) return false;

    VkShaderModule vert_mod = make_shader_module(ctx, vert_code);
    VkShaderModule frag_mod = make_shader_module(ctx, frag_code);
    if (!vert_mod || !frag_mod) {
        if (vert_mod) vkDestroyShaderModule(ctx.device(), vert_mod, nullptr);
        if (frag_mod) vkDestroyShaderModule(ctx.device(), frag_mod, nullptr);
        std::fprintf(stderr, "[VkSprite] vkCreateShaderModule failed\n");
        return false;
    }

    // ---- Descriptor set layout: N combined image samplers -------------------
    // 1 para los sprites (la textura del asset); 3 para el video (): luma y
    // los dos cromas, que el fragment shader combina.
    if (sampler_count == 0 || sampler_count > 4) sampler_count = 1;
    VkDescriptorSetLayoutBinding sampler_binding[4]{};
    for (uint32_t i = 0; i < sampler_count; ++i) {
        sampler_binding[i].binding         = i;
        sampler_binding[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sampler_binding[i].descriptorCount = 1;
        sampler_binding[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo dsl_info{};
    dsl_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_info.bindingCount = sampler_count;
    dsl_info.pBindings    = sampler_binding;

    if (vkCreateDescriptorSetLayout(ctx.device(), &dsl_info, nullptr, out_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(ctx.device(), vert_mod, nullptr);
        vkDestroyShaderModule(ctx.device(), frag_mod, nullptr);
        return false;
    }

    // ---- Push constant range: 14 floats for the vertex shader ---------------
    // DEBE matchear struct PC (abajo) y el uniform PC de sprite.vert (memoria:
    // push constant = struct + range + shader, los tres iguales).
    VkPushConstantRange pc_range{};
    pc_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pc_range.offset     = 0;
    pc_range.size       = 14 * sizeof(float);  // x,y,w,h, scr_w,scr_h, tr,tg,tb, u0,v0,uw,vh, a

    VkPipelineLayoutCreateInfo pl_info{};
    pl_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_info.setLayoutCount         = 1;
    pl_info.pSetLayouts            = out_layout;
    pl_info.pushConstantRangeCount = 1;
    pl_info.pPushConstantRanges    = &pc_range;

    if (vkCreatePipelineLayout(ctx.device(), &pl_info, nullptr, out_pipe_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(ctx.device(), vert_mod, nullptr);
        vkDestroyShaderModule(ctx.device(), frag_mod, nullptr);
        return false;
    }

    // ---- Shader stages -------------------------------------------------------
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_mod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_mod;
    stages[1].pName  = "main";

    // ---- No vertex buffer — quad generated in vertex shader ------------------
    VkPipelineVertexInputStateCreateInfo vtx_input{};
    vtx_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_asm{};
    input_asm.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_asm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // ---- Dynamic viewport + scissor (survives resize) -----------------------
    VkPipelineViewportStateCreateInfo vp_state{};
    vp_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp_state.viewportCount = 1;
    vp_state.scissorCount  = 1;

    VkDynamicState dyn_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dyn_states;

    // ---- Rasterizer ---------------------------------------------------------
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = VK_CULL_MODE_NONE;     // no backface culling for 2-D quads
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // ---- Alpha blending: src_alpha / one_minus_src_alpha en COLOR; PRESERVA el
    //      alpha del destino (el offscreen). El emu deja el offscreen opaco (alpha=255)
    //      y la UI (ImGui) muestra el offscreen USANDO su alpha. Si escribiéramos el
    //      alpha de la fuente (srcAlpha=ONE/dstAlpha=ZERO), un overlay con alpha<1 —y
    //      sobre todo el COMPOSE de primer plano, que cubre toda la pantalla con alpha=0
    //      en los huecos— dejaría el offscreen TRANSPARENTE ahí → el viewport lo pinta
    //      NEGRO aunque el color HD esté debajo. Con dstAlpha=ONE el offscreen queda
    //      opaco y sólo se mezcla el color. (También evita halos por sprites HD con alpha.)
    VkPipelineColorBlendAttachmentState blend_att{};
    blend_att.blendEnable         = VK_TRUE;
    blend_att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_att.colorBlendOp        = VK_BLEND_OP_ADD;
    blend_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;   // no escribir el alpha de la fuente…
    blend_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;    // …conservar el alpha del offscreen (opaco)
    blend_att.alphaBlendOp        = VK_BLEND_OP_ADD;
    blend_att.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments    = &blend_att;

    // ---- Assemble pipeline --------------------------------------------------
    VkGraphicsPipelineCreateInfo gp_info{};
    gp_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp_info.stageCount          = 2;
    gp_info.pStages             = stages;
    gp_info.pVertexInputState   = &vtx_input;
    gp_info.pInputAssemblyState = &input_asm;
    gp_info.pViewportState      = &vp_state;
    gp_info.pRasterizationState = &raster;
    gp_info.pMultisampleState   = &ms;
    gp_info.pColorBlendState    = &blend;
    gp_info.pDynamicState       = &dyn;
    gp_info.layout              = *out_pipe_layout;
    gp_info.renderPass          = render_pass_;
    gp_info.subpass             = 0;

    VkResult res = vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE,
                                              1, &gp_info, nullptr, out_pipeline);

    // : variante ADITIVA (Acetatos) — mismo todo, sólo cambia el blend de
    // COLOR a src·a + dst (el alpha del destino se sigue preservando: el
    // offscreen queda opaco, mismo razonamiento que arriba). Si falla no aborta:
    // el aditivo cae al alpha normal y queda el motivo en el log.
    if (res == VK_SUCCESS && out_pipeline_add) {
        blend_att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        if (vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE,
                                      1, &gp_info, nullptr, out_pipeline_add) != VK_SUCCESS) {
            std::fprintf(stderr, "[VkSprite] pipeline aditivo no disponible: "
                                 "blend=1 va a dibujar con alpha normal\n");
            *out_pipeline_add = VK_NULL_HANDLE;
        }
    }

    // /: variantes con FRAG PROPIO (multiplicar, pantalla) — reusan
    // layout, vert y todo el estado; sólo cambian el fragment shader y los
    // factores de COLOR (el alpha del destino se sigue preservando, mismo
    // razonamiento de arriba). Degradación POR variante: sin su .spv, o si la
    // creación falla, su out queda NULL —ese modo dibuja con alpha normal— y
    // esta función no fracasa por eso.
    if (res == VK_SUCCESS && variants) {
        for (uint32_t vi = 0; vi < variant_count; ++vi) {
            const PipelineBlendVariant& bv = variants[vi];
            if (!bv.out) continue;
            *bv.out = VK_NULL_HANDLE;
            auto vfrag = load_spv(bv.frag_spv);
            VkShaderModule vfm = vfrag.empty() ? VK_NULL_HANDLE
                                               : make_shader_module(ctx, vfrag);
            if (!vfm) {
                std::fprintf(stderr, "[VkSprite] variante de blend no disponible "
                                     "(%s): ese modo va a dibujar con alpha "
                                     "normal\n", bv.frag_spv);
                continue;
            }
            stages[1].module = vfm;
            blend_att.srcColorBlendFactor = bv.src_color;
            blend_att.dstColorBlendFactor = bv.dst_color;
            blend_att.colorBlendOp        = bv.color_op;   //  (MIN/MAX ignoran factores)
            if (vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE,
                                          1, &gp_info, nullptr, bv.out) != VK_SUCCESS) {
                std::fprintf(stderr, "[VkSprite] pipeline de blend (%s) falló: "
                                     "ese modo va a dibujar con alpha normal\n",
                             bv.frag_spv);
                *bv.out = VK_NULL_HANDLE;
            }
            vkDestroyShaderModule(ctx.device(), vfm, nullptr);
        }
    }

    vkDestroyShaderModule(ctx.device(), vert_mod, nullptr);
    vkDestroyShaderModule(ctx.device(), frag_mod, nullptr);

    if (res != VK_SUCCESS) {
        std::fprintf(stderr, "[VkSprite] vkCreateGraphicsPipelines failed (%d)\n", res);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// VkSprite::create_sampler
// ---------------------------------------------------------------------------
bool VkSprite::create_sampler(VkContext& ctx) {
    // Trilinear: los assets se cargan MIPMAPPED (VkTexture genera la cadena) —
    // minificar un máster HD sin mips aliasea aunque el filtro sea LINEAR.
    VkSamplerCreateInfo si{};
    si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod       = 0.0f;
    si.maxLod       = VK_LOD_CLAMP_NONE;

    if (vkCreateSampler(ctx.device(), &si, nullptr, &sampler_) != VK_SUCCESS) {
        std::fprintf(stderr, "[VkSprite] vkCreateSampler failed\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// VkSprite::create_desc_pool
// ---------------------------------------------------------------------------
bool VkSprite::create_desc_pool(VkContext& ctx) {
    // CADENA de pools: cada eslabón aloja 128 assets ×2 sets (NEAREST + LINEAR;
    // el draw elige por-quad según minificación). El pool ÚNICO se agotaba en
    // proyectos reales (Golden Axe: 65 assets ×2 caras = 260 sets > 256) y las
    // texturas perdedoras del orden de carga quedaban negative-cacheadas toda
    // la sesión → poses INVISIBLES en sus ventanas de frames (Tyris f515-536).
    // alloc_desc_set() agrega eslabones bajo demanda; acá solo el primero.
    static constexpr uint32_t kAssetsPerPool = 128;

    VkDescriptorPoolSize pool_size{};
    pool_size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = kAssetsPerPool * 2;

    VkDescriptorPoolCreateInfo pi{};
    pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets       = kAssetsPerPool * 2;
    pi.poolSizeCount = 1;
    pi.pPoolSizes    = &pool_size;

    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(ctx.device(), &pi, nullptr, &pool) != VK_SUCCESS) {
        std::fprintf(stderr, "[VkSprite] vkCreateDescriptorPool failed\n");
        return false;
    }
    desc_pools_.push_back(pool);
    return true;
}

VkDescriptorSet VkSprite::alloc_desc_set(VkContext& ctx) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (desc_pools_.empty() && !create_desc_pool(ctx)) return VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = desc_pools_.back();
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &desc_layout_;
        VkDescriptorSet ds = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(ctx.device(), &ai, &ds) == VK_SUCCESS)
            return ds;
        // Eslabón agotado → crear el siguiente y reintentar (una vez).
        if (attempt == 0 && !create_desc_pool(ctx)) return VK_NULL_HANDLE;
    }
    return VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Vestuario: pools/sets de 2 samplers (asset + máscara). Cadena propia — los
// de `desc_pools_` se alocan con `desc_layout_` (1 binding) y sus tamaños
// están calibrados para eso. Misma política de crecimiento bajo demanda.
// ---------------------------------------------------------------------------
bool VkSprite::create_mask_pool(VkContext& ctx) {
    static constexpr uint32_t kSetsPerPool = 64;   // pares (pose, máscara) × filtro
    VkDescriptorPoolSize pool_size{};
    pool_size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = kSetsPerPool * 2;   // 2 bindings por set
    VkDescriptorPoolCreateInfo pi{};
    pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets       = kSetsPerPool;
    pi.poolSizeCount = 1;
    pi.pPoolSizes    = &pool_size;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(ctx.device(), &pi, nullptr, &pool) != VK_SUCCESS) {
        std::fprintf(stderr, "[VkSprite] mask descriptor pool failed\n");
        return false;
    }
    mask_pools_.push_back(pool);
    return true;
}

VkDescriptorSet VkSprite::alloc_mask_set(VkContext& ctx) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (mask_pools_.empty() && !create_mask_pool(ctx)) return VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = mask_pools_.back();
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &mask_layout_;
        VkDescriptorSet ds = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(ctx.device(), &ai, &ds) == VK_SUCCESS)
            return ds;
        if (attempt == 0 && !create_mask_pool(ctx)) return VK_NULL_HANDLE;
    }
    return VK_NULL_HANDLE;
}

// El set combinado se cachea por (asset_key, mask_key, filtro) y guarda los
// seq de ambas entradas: tras un evict/hot-reload el seq no coincide y se
// re-escribe sobre el MISMO set (evict ya esperó GPU idle — mismo contrato que
// la reutilización de sets de TexEntry).
VkDescriptorSet VkSprite::get_mask_set(VkContext& ctx, const std::string& asset_key,
                                       const std::string& mask_key, TexEntry& asset,
                                       TexEntry& mask, bool linear) {
    if (!asset.valid || !asset.tex || !mask.valid || !mask.tex)
        return VK_NULL_HANDLE;
    const std::string ck = asset_key + "|" + mask_key + (linear ? "|L" : "|N");
    MaskSet& ms = mask_set_cache_[ck];
    if (ms.ds != VK_NULL_HANDLE && ms.asset_seq == asset.seq &&
        ms.mask_seq == mask.seq)
        return ms.ds;
    if (ms.ds == VK_NULL_HANDLE) {
        ms.ds = alloc_mask_set(ctx);
        if (ms.ds == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    }
    // El MISMO filtro para asset y máscara (dims iguales ⇒ misma decisión de
    // minificación): un filtro distinto desalinearía el borde de la zona
    // enmascarada respecto del píxel del asset.
    const VkSampler smp = linear ? sampler_ : sampler_nn_;
    VkDescriptorImageInfo ii[2]{};
    VkWriteDescriptorSet  wr[2]{};
    VkTexture* texs[2] = { asset.tex, mask.tex };
    for (int i = 0; i < 2; ++i) {
        ii[i].sampler     = smp;
        ii[i].imageView   = texs[i]->image_view();
        ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        wr[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet          = ms.ds;
        wr[i].dstBinding      = static_cast<uint32_t>(i);
        wr[i].descriptorCount = 1;
        wr[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[i].pImageInfo      = &ii[i];
    }
    vkUpdateDescriptorSets(ctx.device(), 2, wr, 0, nullptr);
    ms.asset_seq = asset.seq;
    ms.mask_seq  = mask.seq;
    return ms.ds;
}

// ---------------------------------------------------------------------------
// VkSprite::create_framebuffers / destroy_framebuffers
// ---------------------------------------------------------------------------
bool VkSprite::create_framebuffer(VkContext& ctx, VkImageView view, uint32_t w, uint32_t h) {
    fb_w_ = w;
    fb_h_ = h;

    framebuffers_.resize(1, VK_NULL_HANDLE);
    VkFramebufferCreateInfo fi{};
    fi.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass      = render_pass_;
    fi.attachmentCount = 1;
    fi.pAttachments    = &view;
    fi.width           = fb_w_;
    fi.height          = fb_h_;
    fi.layers          = 1;
    if (vkCreateFramebuffer(ctx.device(), &fi, nullptr, &framebuffers_[0]) != VK_SUCCESS) {
        std::fprintf(stderr, "[VkSprite] vkCreateFramebuffer failed\n");
        return false;
    }
    return true;
}

void VkSprite::destroy_framebuffers(VkContext& ctx) {
    for (auto fb : framebuffers_) {
        if (fb != VK_NULL_HANDLE)
            vkDestroyFramebuffer(ctx.device(), fb, nullptr);
    }
    framebuffers_.clear();
    fb_w_ = fb_h_ = 0;
}

// ---------------------------------------------------------------------------
// VkSprite::init / rebuild / shutdown
// ---------------------------------------------------------------------------
bool VkSprite::init(VkContext& ctx, VkFormat fmt, uint32_t w, uint32_t h,
                    VkImageView target_view,
                    const char* vert_spv_path, const char* frag_spv_path) {
    if (!ctx.is_ready()) return false;

    if (!create_render_pass(ctx, fmt))             return false;
    {
        std::string vf(vert_spv_path);
        const size_t slash = vf.find_last_of("/\\");
        const std::string dir = slash == std::string::npos ? std::string()
                                                           : vf.substr(0, slash + 1);
        // /: MULTIPLICAR y PANTALLA — variantes del pipeline clásico
        // con frag propio (el mix hacia blanco/negro por la fuerza no sale con
        // factores fijos sin premultiplicar) y factores de COLOR fijos. La
        // degradación es por variante (sin su .spv ese modo cae a alpha).
        const std::string mult_frag   = dir + "sprite_mult.frag.spv";
        const std::string screen_frag = dir + "sprite_screen.frag.spv";
        // : sub/min/max REUSAN los dos frags — solo cambia el op.
        const PipelineBlendVariant blend_variants[] = {
            { mult_frag.c_str(),   VK_BLEND_FACTOR_DST_COLOR,
              VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,              &pipeline_mult_ },
            { screen_frag.c_str(), VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
              VK_BLEND_FACTOR_ONE,  VK_BLEND_OP_ADD,              &pipeline_screen_ },
            { screen_frag.c_str(), VK_BLEND_FACTOR_ONE,
              VK_BLEND_FACTOR_ONE,  VK_BLEND_OP_REVERSE_SUBTRACT, &pipeline_sub_ },
            { mult_frag.c_str(),   VK_BLEND_FACTOR_ONE,
              VK_BLEND_FACTOR_ONE,  VK_BLEND_OP_MIN,              &pipeline_min_ },
            { screen_frag.c_str(), VK_BLEND_FACTOR_ONE,
              VK_BLEND_FACTOR_ONE,  VK_BLEND_OP_MAX,              &pipeline_max_ },
        };
        if (!create_pipeline(ctx, vert_spv_path, frag_spv_path, 1,
                             &desc_layout_, &pipe_layout_, &pipeline_,
                             &pipeline_add_, blend_variants, 5)) return false;

    // : pipeline del VIDEO — mismo vertex shader y mismo push constant, tres
    // samplers y video.frag. Si el .spv no está (build viejo, o alguien movió el
    // shader) NO se aborta: el resto del renderer funciona y la Cinemática es lo
    // único que se queda sin dibujar, con motivo en el log. Abortar el init del
    // renderer entero por un video sería desproporcionado.
        const std::string video_frag = dir + "video.frag.spv";
        if (!create_pipeline(ctx, vert_spv_path, video_frag.c_str(), 3,
                             &video_layout_, &video_pipe_layout_, &video_pipeline_)) {
            std::fprintf(stderr, "[VkSprite] pipeline de video no disponible (%s): "
                                 "la Cinematica no se va a dibujar\n", video_frag.c_str());
            video_pipeline_ = VK_NULL_HANDLE;
        }
        // Vestuario: pipeline de 2 samplers (asset + máscara R8) con
        // sprite_mask.frag. Misma política de degradación que el video: sin el
        // .spv no se aborta — los quads con máscara caen al pipeline clásico
        // (tinte entero) con motivo en el log.
        const std::string mask_frag = dir + "sprite_mask.frag.spv";
        if (!create_pipeline(ctx, vert_spv_path, mask_frag.c_str(), 2,
                             &mask_layout_, &mask_pipe_layout_, &mask_pipeline_)) {
            std::fprintf(stderr, "[VkSprite] pipeline de Vestuario no disponible (%s): "
                                 "las mascaras de tinte no se van a aplicar\n",
                         mask_frag.c_str());
            mask_pipeline_ = VK_NULL_HANDLE;
        }
    }
    if (!create_sampler    (ctx))                  return false;
    if (!create_desc_pool  (ctx))                  return false;
    if (!create_framebuffer(ctx, target_view, w, h)) return false;

    // Sampler NEAREST: nació para el mask del compose (borrado en ) y hoy
    // lo usa el par de samplers del pipeline — nítido donde el bilineal
    // arruinaría un borde de pixel art.
    {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_NEAREST;
        si.minFilter    = VK_FILTER_NEAREST;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod       = 0.25f;
        if (vkCreateSampler(ctx.device(), &si, nullptr, &sampler_nn_) != VK_SUCCESS) {
            std::fprintf(stderr, "[VkSprite] nearest sampler failed\n");
            return false;
        }
        // Pool propio del VIDEO (): `clear_textures` resetea `pool_` en el
        // hotreload del pack y se llevaría puesto el set del video a mitad de
        // reproducción. (Estas structs las declaraba el pool del overlay del
        // compose, que se borró en  — el video las necesita igual.)
        VkDescriptorPoolSize ps{};
        ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ps.descriptorCount = 1;
        VkDescriptorPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets = 1;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(ctx.device(), &pi, nullptr, &video_pool_) != VK_SUCCESS) {
            std::fprintf(stderr, "[VkSprite] video descriptor pool failed\n");
            return false;
        }
    }

    std::fprintf(stdout, "[VkSprite] Sprite pipeline ready  (%ux%u)\n",
                 fb_w_, fb_h_);
    return true;
}

void VkSprite::rebuild(VkContext& ctx, uint32_t w, uint32_t h, VkImageView target_view) {
    if (!pipeline_) return;
    vkDeviceWaitIdle(ctx.device());
    destroy_framebuffers(ctx);
    create_framebuffer(ctx, target_view, w, h);
    std::fprintf(stdout, "[VkSprite] Rebuilt framebuffer  (%ux%u)\n",
                 fb_w_, fb_h_);
}

void VkSprite::shutdown(VkContext& ctx) {
    // : parar el worker de decode (aunque el ctx no este listo).
    if (decode_worker_.joinable()) {
        {
            std::lock_guard<std::mutex> lk(decode_mx_);
            decode_quit_ = true;
        }
        decode_cv_.notify_one();
        decode_worker_.join();
        decode_quit_ = false;
    }
    if (!ctx.is_ready()) return;
    vkDeviceWaitIdle(ctx.device());

    // Destroy all sprite textures.
    for (auto& [_, entry] : tex_cache_) {
        if (entry.valid && entry.tex) {
            entry.tex->shutdown(ctx);
            delete entry.tex;
        }
    }
    tex_cache_.clear();


    // Video de la Cinemática (draw_video, ) — tres planos desde .
    for (VkTexture*& t : video_tex_) {
        if (!t) continue;
        t->shutdown(ctx); delete t; t = nullptr;
    }
    video_ds_ = VK_NULL_HANDLE;   // liberado con su pool
    video_w_ = video_h_ = 0; video_seq_ = 0;
    if (video_pool_   != VK_NULL_HANDLE) { vkDestroyDescriptorPool(ctx.device(), video_pool_,   nullptr); video_pool_   = VK_NULL_HANDLE; }
    if (video_pipeline_    != VK_NULL_HANDLE) { vkDestroyPipeline          (ctx.device(), video_pipeline_,    nullptr); video_pipeline_    = VK_NULL_HANDLE; }
    if (video_pipe_layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout    (ctx.device(), video_pipe_layout_, nullptr); video_pipe_layout_ = VK_NULL_HANDLE; }
    if (video_layout_      != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(ctx.device(), video_layout_,     nullptr); video_layout_      = VK_NULL_HANDLE; }

    // Vestuario: sets combinados + pools + pipeline de 2 samplers.
    mask_set_cache_.clear();
    for (VkDescriptorPool pool : mask_pools_)
        vkDestroyDescriptorPool(ctx.device(), pool, nullptr);
    mask_pools_.clear();
    if (mask_pipeline_    != VK_NULL_HANDLE) { vkDestroyPipeline          (ctx.device(), mask_pipeline_,    nullptr); mask_pipeline_    = VK_NULL_HANDLE; }
    if (mask_pipe_layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout    (ctx.device(), mask_pipe_layout_, nullptr); mask_pipe_layout_ = VK_NULL_HANDLE; }
    if (mask_layout_      != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(ctx.device(), mask_layout_,     nullptr); mask_layout_      = VK_NULL_HANDLE; }

    // //: variantes de blend del pipeline de sprites.
    if (pipeline_mult_   != VK_NULL_HANDLE) { vkDestroyPipeline(ctx.device(), pipeline_mult_,   nullptr); pipeline_mult_   = VK_NULL_HANDLE; }
    if (pipeline_screen_ != VK_NULL_HANDLE) { vkDestroyPipeline(ctx.device(), pipeline_screen_, nullptr); pipeline_screen_ = VK_NULL_HANDLE; }
    if (pipeline_sub_    != VK_NULL_HANDLE) { vkDestroyPipeline(ctx.device(), pipeline_sub_,    nullptr); pipeline_sub_    = VK_NULL_HANDLE; }
    if (pipeline_min_    != VK_NULL_HANDLE) { vkDestroyPipeline(ctx.device(), pipeline_min_,    nullptr); pipeline_min_    = VK_NULL_HANDLE; }
    if (pipeline_max_    != VK_NULL_HANDLE) { vkDestroyPipeline(ctx.device(), pipeline_max_,    nullptr); pipeline_max_    = VK_NULL_HANDLE; }

    if (sampler_nn_   != VK_NULL_HANDLE) { vkDestroySampler        (ctx.device(), sampler_nn_,   nullptr); sampler_nn_   = VK_NULL_HANDLE; }

    destroy_framebuffers(ctx);

    for (VkDescriptorPool pool : desc_pools_)
        vkDestroyDescriptorPool(ctx.device(), pool, nullptr);
    desc_pools_.clear();
    if (sampler_     != VK_NULL_HANDLE) { vkDestroySampler            (ctx.device(), sampler_,     nullptr); sampler_     = VK_NULL_HANDLE; }
    if (pipeline_add_ != VK_NULL_HANDLE) { vkDestroyPipeline          (ctx.device(), pipeline_add_, nullptr); pipeline_add_ = VK_NULL_HANDLE; }
    if (pipeline_    != VK_NULL_HANDLE) { vkDestroyPipeline           (ctx.device(), pipeline_,    nullptr); pipeline_    = VK_NULL_HANDLE; }
    if (pipe_layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout     (ctx.device(), pipe_layout_, nullptr); pipe_layout_ = VK_NULL_HANDLE; }
    if (desc_layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(ctx.device(), desc_layout_, nullptr); desc_layout_ = VK_NULL_HANDLE; }
    if (render_pass_ != VK_NULL_HANDLE) { vkDestroyRenderPass         (ctx.device(), render_pass_, nullptr); render_pass_ = VK_NULL_HANDLE; }
}

// ---------------------------------------------------------------------------
// VkSprite::ensure_loaded — lookup del cache + ENCOLAR el decode asíncrono.
//
//  fase 2: el decode stbi de un máster 1440×1296 son decenas de ms; hecho
// acá (hilo de render, síncrono) era un pico > presupuesto de frame en la
// PRIMERA aparición de cada asset y de cada variante volteada → el backlog de
// audio (~46 ms) se vaciaba → crackle. Ahora este método solo mira el cache y,
// en un miss, lee los bytes y encola el decode en el worker; devuelve nullptr
// hasta que pump_uploads() suba la textura (1-3 frames después; con prewarm()
// al alimentar las poses, la textura ya está lista en la primera aparición).
// ---------------------------------------------------------------------------
VkSprite::TexEntry* VkSprite::ensure_loaded(VkContext& ctx, VkCommandBuffer cmd,
                                              const std::string& path,
                                              AyArchive* pack, uint8_t flip,
                                              bool mask) {
    (void)ctx; (void)cmd;   // el upload ocurre en pump_uploads()
    // Las variantes volteadas se cachean aparte (clave = path + sufijo de flip);
    // el asset se LEE del pack por `path`, sólo cambia la textura subida. La
    // máscara (Vestuario) lleva clave `#m…` propia: el mismo PNG puede estar
    // asignado como asset y como máscara, y además se decodifica R8.
    const std::string key = tex_key(path, flip, mask);
    auto it = tex_cache_.find(key);
    if (it != tex_cache_.end() && !it->second.stale) {
        if (it->second.pending) return nullptr;               // decode en vuelo
        return it->second.valid ? &it->second : nullptr;      // valid o negative-cache
    }
    enqueue_decode(key, path, pack, flip, mask);
    return nullptr;
}

// ---------------------------------------------------------------------------
// VkSprite::texture_state — R-8 (): consulta de solo lectura del cache.
// Misma clave que draw()/ensure_loaded (path + sufijo de flip); NO encola
// nada — una entrada ausente es «todavía no se pidió», no un miss a cargar.
// ---------------------------------------------------------------------------
VkSprite::TexState VkSprite::texture_state(const std::string& path,
                                           uint8_t flip) const {
    const std::string key = (flip & 3)
        ? path + "#" + static_cast<char>('0' + (flip & 3)) : path;
    auto it = tex_cache_.find(key);
    if (it == tex_cache_.end() || it->second.stale) return TexState::NotRequested;
    if (it->second.pending) return TexState::Pending;
    return it->second.valid ? TexState::Ready : TexState::Failed;
}

// ---------------------------------------------------------------------------
// VkSprite::enqueue_decode — bytes (pack/disco) en el hilo de render (rápido;
// además AyArchive no se toca desde el worker) + job al worker.
// ---------------------------------------------------------------------------
void VkSprite::enqueue_decode(const std::string& key, const std::string& path,
                              AyArchive* pack, uint8_t flip, bool mask_r8) {
    TexEntry& entry = tex_cache_[key];
    if (entry.pending && !entry.stale) return;   // ya en vuelo
    entry.stale = false;
    entry.valid = false;

    // ---- Read the PNG bytes: pack first (relative asset name), then a loose
    //      on-disk file (full path from the Lab's asset browser). Así la autoría
    //      en Editar PREVISUALIZA el reemplazo HD antes de construir un pack.
    std::vector<uint8_t> raw;
    if (pack) {
        int64_t file_sz = ayther_pack_file_size(pack, path.c_str());
        if (file_sz > 0) {
            raw.resize(static_cast<size_t>(file_sz));
            ayther_pack_read(pack, path.c_str(), raw.data(), raw.size());
        }
    }
    bool from_disk = false;
    if (raw.empty()) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (f) {
            std::fseek(f, 0, SEEK_END);
            long sz = std::ftell(f);
            std::rewind(f);
            if (sz > 0) {
                raw.resize(static_cast<size_t>(sz));
                if (std::fread(raw.data(), 1, raw.size(), f) != raw.size()) raw.clear();
            }
            std::fclose(f);
            from_disk = !raw.empty();
        }
    }
    // Hot-reload (poll_disk): registrar el mtime del archivo de DISCO junto a
    // sus bytes — se registra aunque el decode luego falle (PNG a medio
    // escribir), así el poll no re-evicta en loop un archivo corrupto: recién
    // reintenta cuando el artista lo vuelve a guardar (mtime nuevo).
    entry.disk_mtime = -1;
    if (from_disk) {
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(path, ec);
        if (!ec)
            entry.disk_mtime =
                static_cast<int64_t>(t.time_since_epoch().count());
    }
    if (raw.empty()) {
        // Negative-cache (entry !valid !pending): no reintentar cada frame.
        // Hot-reload: -2 = vigilar la APARICIÓN del archivo en poll_disk.
        entry.pending    = false;
        entry.disk_mtime = -2;
        std::fprintf(stderr, "[VkSprite] Asset not found (pack/disk): %s\n", path.c_str());
        return;
    }

    entry.pending = true;
    ++entry.seq;                                 // invalida resultados anteriores
    {
        std::lock_guard<std::mutex> lk(decode_mx_);
        decode_jobs_.push_back({ key, path, flip, mask_r8, decode_gen_,
                                 entry.seq, std::move(raw) });
        if (!decode_worker_.joinable())
            decode_worker_ = std::thread([this] { decode_loop(); });
    }
    decode_cv_.notify_one();
}

// ---------------------------------------------------------------------------
// VkSprite::decode_loop — worker: stbi + swap BGRA + pre-flip (la mitad CPU
// del viejo ensure_loaded síncrono). Sin llamadas Vulkan.
// ---------------------------------------------------------------------------
void VkSprite::decode_loop() {
    // Prioridad BAJA: en laptops (CPU power-limited) el decode competia con el
    // hilo de render/audio; el worker puede tardar mas sin que importe.
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    for (;;) {
        DecodeJob job;
        {
            std::unique_lock<std::mutex> lk(decode_mx_);
            decode_cv_.wait(lk, [&] { return decode_quit_ || !decode_jobs_.empty(); });
            if (decode_quit_) return;
            job = std::move(decode_jobs_.front());
            decode_jobs_.pop_front();
        }
        DecodeDone d;
        d.key = std::move(job.key);
        d.path = std::move(job.path);
        d.r8  = job.r8;
        d.gen = job.gen;
        d.seq = job.seq;

        if (job.r8) {
            // Vestuario: la máscara es escala de grises — 1 canal (stb ya
            // promedia el RGB si el PNG vino en color). Mismo pre-flip que el
            // asset: la máscara TIENE que cargar con la cara del asset o tiñe
            // el lado equivocado en las poses espejadas.
            int w = 0, h = 0, ch = 0;
            uint8_t* px = stbi_load_from_memory(
                job.raw.data(), static_cast<int>(job.raw.size()), &w, &h, &ch, 1);
            if (px) {
                if (job.flip & 1)
                    for (int y = 0; y < h; ++y) {
                        uint8_t* row = px + static_cast<size_t>(y) * w;
                        for (int x = 0; x < w / 2; ++x) std::swap(row[x], row[w - 1 - x]);
                    }
                if (job.flip & 2)
                    for (int y = 0; y < h / 2; ++y)
                        for (int x = 0; x < w; ++x)
                            std::swap(px[static_cast<size_t>(y) * w + x],
                                      px[static_cast<size_t>(h - 1 - y) * w + x]);
                d.w = w; d.h = h;
                d.bgra.assign(px, px + static_cast<size_t>(w) * h);
                stbi_image_free(px);
            }
            std::lock_guard<std::mutex> lk(decode_mx_);
            decode_done_.push_back(std::move(d));
            continue;
        }

        int w = 0, h = 0, ch = 0;
        uint8_t* pixels = stbi_load_from_memory(
            job.raw.data(), static_cast<int>(job.raw.size()), &w, &h, &ch, 4);
        if (pixels) {
            // Swap R↔B: RGBA → BGRA (VK_FORMAT_B8G8R8A8_UNORM), alpha preserved.
            for (int p = 0; p < w * h; ++p)
                std::swap(pixels[p * 4 + 0], pixels[p * 4 + 2]);
            // Pre-flip (Fase 2c): voltea la textura en CPU según `flip` (bit0 h,
            // bit1 v) — un tile de plano/pose volteado usa la misma asignación HD
            // sin tocar el shader.
            if (job.flip) {
                uint32_t* px = reinterpret_cast<uint32_t*>(pixels);   // BGRA = 4 B/px
                if (job.flip & 1)
                    for (int y = 0; y < h; ++y) {
                        uint32_t* row = px + static_cast<size_t>(y) * w;
                        for (int x = 0; x < w / 2; ++x) std::swap(row[x], row[w - 1 - x]);
                    }
                if (job.flip & 2)
                    for (int y = 0; y < h / 2; ++y)
                        for (int x = 0; x < w; ++x)
                            std::swap(px[static_cast<size_t>(y) * w + x],
                                      px[static_cast<size_t>(h - 1 - y) * w + x]);
            }
            d.w = w; d.h = h;
            d.bgra.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
            stbi_image_free(pixels);
        }
        std::lock_guard<std::mutex> lk(decode_mx_);
        decode_done_.push_back(std::move(d));
    }
}

// ---------------------------------------------------------------------------
// VkSprite::pump_uploads — sube al GPU los decodes terminados (acotado por
// frame). Llamar 1×/frame FUERA de un render pass, aunque no haya subs.
// ---------------------------------------------------------------------------
void VkSprite::pump_uploads(VkContext& ctx, VkCommandBuffer cmd, int max_uploads) {
    // ---- Staging diferido (): liberar lo vencido -----------------------
    ++pump_tick_;
    if (!staging_release_.empty()) {
        size_t freed = 0;
        int    n     = 0;
        auto keep = staging_release_.begin();
        for (auto& r : staging_release_) {
            if (r.due > pump_tick_) { *keep++ = std::move(r); continue; }
            auto it = tex_cache_.find(r.key);
            // seq distinto = evict/reload: la textura vigente es OTRA (su
            // propio release ya está encolado) — no tocarla.
            if (it != tex_cache_.end() && it->second.seq == r.seq &&
                it->second.tex) {
                freed += it->second.tex->release_staging(ctx);
                ++n;
            }
        }
        staging_release_.erase(keep, staging_release_.end());
        if (freed) {
            staging_freed_total_ += freed;
            if (vk_verbose_logging())
                std::fprintf(stdout,
                    "[VkSprite] staging liberado: %d tex (+%zu KB, total %zu KB)\n",
                    n, freed / 1024, staging_freed_total_ / 1024);
        }
    }

    std::vector<DecodeDone> ready;
    {
        std::lock_guard<std::mutex> lk(decode_mx_);
        while (!decode_done_.empty() && static_cast<int>(ready.size()) < max_uploads) {
            ready.push_back(std::move(decode_done_.back()));
            decode_done_.pop_back();
        }
    }
    for (DecodeDone& d : ready) {
        if (d.gen != decode_gen_) continue;              // clear_textures posterior
        auto it = tex_cache_.find(d.key);
        if (it == tex_cache_.end() || it->second.seq != d.seq) continue;   // evict/re-encolado
        TexEntry& entry = it->second;
        entry.pending = false;
        if (d.bgra.empty()) {                            // decode falló → negative-cache
            std::fprintf(stderr, "[VkSprite] PNG decode failed: %s\n", d.path.c_str());
            continue;
        }
        // MEDICIÓN DEL UPLOAD (). El presupuesto de 1 upload/frame se fijó
        // asumiendo que el staging memcpy de un master grande son 4-5 ms. Un
        // banco de memcpy suelto mide 25 GB/s —8 MB serían 0,3 ms—, así que o
        // el costo está en otra parte del camino o esa nota es de otra máquina.
        // Esto cronometra el camino REAL y completo, que es el único número que
        // sirve para decidir si un frame de video por frame de juego entra.
        const auto t_up = std::chrono::steady_clock::now();
        const bool up_ok =
            finish_upload(ctx, cmd, entry, d.path, d.w, d.h, d.bgra.data(), d.r8);
        const double up_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_up).count();
        if (up_ok) {
            const double mb = (double)d.bgra.size() / (1024.0 * 1024.0);
            if (up_ms > upload_ms_max_) upload_ms_max_ = up_ms;
            upload_ms_total_ += up_ms; ++upload_count_;
            upload_mb_total_ += mb;
            // Una línea POR ASSET cuesta ~5 ms contra la consola (): más que
            // el upload entero. Queda detrás de AYTHER_VK_VERBOSE; el agregado
            // de abajo conserva media/max, que era para lo que se leía.
            if (vk_verbose_logging())
                std::fprintf(stdout,
                    "[VkSprite] Loaded: %s  (%dx%d)  upload %.2f ms · %.2f MB · "
                    "%.1f GB/s  [max %.2f · media %.2f de %d]\n",
                    d.path.c_str(), d.w, d.h, up_ms, mb,
                    up_ms > 0.0 ? (mb / 1024.0) / (up_ms / 1000.0) : 0.0,
                    upload_ms_max_, upload_ms_total_ / (double)upload_count_,
                    upload_count_);
            // Partición acumulada: es el número que cerró el issue . Se
            // reporta en ms POR UPLOAD, no en porcentajes: los porcentajes de la
            // primera medición escondieron que las partes sumaban 10 ms de 725.
            // La cifra de control «suma X de Y» se queda como guarda permanente:
            // si vuelve a abrirse una brecha, es que algo no cronometrado entró
            // al camino caliente — que es exactamente cómo se encontró esto.
            if ((upload_count_ % 16) == 0) {
                const double n  = (double)upload_count_;
                const double it = up_img_ms_ + up_view_ms_ + up_stg_ms_ +
                                  up_log_ms_;
                std::fprintf(stdout,
                    "[VkSprite] %d uploads · %.1f MB · media %.2f ms · max %.2f ms\n"
                    "           ms/upload: imagen %.3f · vista %.3f · staging %.3f"
                    " · pixeles %.3f · descriptores %.3f · log %.3f\n"
                    "           control: init medido %.3f, partes suman %.3f\n",
                    upload_count_, upload_mb_total_,
                    upload_ms_total_ / n, upload_ms_max_,
                    up_img_ms_ / n, up_view_ms_ / n, up_stg_ms_ / n,
                    up_copy_ms_ / n, up_desc_ms_ / n, up_log_ms_ / n,
                    up_init_ms_ / n, it / n);
            }
            // Asset = UN solo upload: el staging se libera en diferido ().
            staging_release_.push_back(
                { d.key, entry.seq, pump_tick_ + kStagingLingerPumps });
        }
    }
}

// ---------------------------------------------------------------------------
// VkSprite::prewarm — encolar el decode ANTES de la primera aparición (al
// asignar/alimentar poses). Solo assets sueltos de disco: sin pack y con un
// path inexistente sería negative-cachear un nombre relativo de pack.
// ---------------------------------------------------------------------------
void VkSprite::prewarm(const std::string& path, AyArchive* pack, uint8_t flip,
                       bool mask) {
    if (path.empty()) return;
    // CACHE PRIMERO, stat DESPUÉS: el feed de poses llama prewarm por asset
    // POR FRAME (~50-70 llamadas) — con el fs::exists() antes del lookup eran
    // 50-70 stats de filesystem por frame (Documents puede ser OneDrive):
    // 100-200 ms/frame de "sesion" y el audio hambreado TODO el tiempo (frame
    // probe del usuario,  causa 8). El stat corre solo en un miss real
    // (una vez por asset por sesión).
    const std::string key = tex_key(path, flip, mask);
    auto it = tex_cache_.find(key);
    if (it != tex_cache_.end() && !it->second.stale) return;   // listo / en vuelo / negativo
    if (!pack) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return;
    }
    enqueue_decode(key, path, pack, flip, mask);
}

// ---------------------------------------------------------------------------
// VkSprite::finish_upload — textura + descriptores desde BGRA decodificado
// (la mitad GPU del viejo ensure_loaded síncrono).
// ---------------------------------------------------------------------------
bool VkSprite::finish_upload(VkContext& ctx, VkCommandBuffer cmd, TexEntry& entry,
                             const std::string& path, int w, int h,
                             const uint8_t* bgra, bool r8) {
    // DESGLOSE (). Los 3,5 ms de un upload no son el memcpy —eso es 0,31 ms
    // para 8 MB— y hay tres candidatos: crear la imagen con su cadena de mips,
    // la subida en sí, y los descriptores. Cronometrar los tres es lo que
    // decide cuál de las optimizaciones vale la pena; medir antes de suponer.
    const auto t0 = std::chrono::steady_clock::now();

    // ---- Allocate GPU texture (mipmapped: los assets se minifican) ---------
    // Vestuario (r8): la máscara sube R8/Gray8 — 1 byte por píxel, mismos mips
    // que el asset (se minifica con él).
    entry.tex = new VkTexture();
    if (!entry.tex->init(ctx, static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                         /*mipmapped=*/true,
                         r8 ? TexImageFormat::R8 : TexImageFormat::Bgra8)) {
        delete entry.tex;
        entry.tex = nullptr;
        return false;
    }
    const auto t1 = std::chrono::steady_clock::now();

    // Upload with Bgra8888 format — direct copy, alpha channel intact.
    entry.tex->upload(ctx, cmd,
                      bgra,
                      static_cast<uint32_t>(w),
                      static_cast<uint32_t>(h),
                      static_cast<size_t>(w) * (r8 ? 1 : 4),
                      r8 ? TexPixelFormat::Gray8 : TexPixelFormat::Bgra8888);
    const auto t2 = std::chrono::steady_clock::now();

    // La máscara NUNCA se dibuja sola: no gasta sets de 1 sampler — vive en
    // los sets combinados (get_mask_set), que se arman cuando el asset y ella
    // están listos.
    if (r8) {
        const auto t3r = std::chrono::steady_clock::now();
        using msr = std::chrono::duration<double, std::milli>;
        up_init_ms_ += msr(t1 - t0).count();
        up_copy_ms_ += msr(t2 - t1).count();
        (void)t3r;
        entry.valid = true;
        return true;
    }

    // ---- Allocate descriptor sets (reusados si la entrada fue evictada) ----
    // Dos por textura: NEAREST y LINEAR — el draw elige por-quad. La cadena de
    // pools crece bajo demanda; un fallo acá es un error real del driver.
    for (VkDescriptorSet* ds : { &entry.desc_set, &entry.desc_set_lin }) {
        if (*ds != VK_NULL_HANDLE) continue;
        *ds = alloc_desc_set(ctx);
        if (*ds == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[VkSprite] Descriptor set alloc failed: %s\n", path.c_str());
            entry.tex->shutdown(ctx);
            delete entry.tex;
            entry.tex = nullptr;
            return false;
        }
    }

    // ---- Write descriptors --------------------------------------------------
    // desc_set = NEAREST: pixel-art AMPLIADO (snapshots de pose; el LINEAR los
    // difuminaba — el reloj "borroso"). desc_set_lin = LINEAR: arte HD MINIFICADO
    // (un máster 1296² dibujado a ~72px con NEAREST se ve pixelado/aliaseado).
    const VkSampler samplers[2] = { sampler_nn_, sampler_ };
    VkDescriptorSet dsts[2]     = { entry.desc_set, entry.desc_set_lin };
    for (int i = 0; i < 2; ++i) {
        VkDescriptorImageInfo img_info{};
        img_info.sampler     = samplers[i];
        img_info.imageView   = entry.tex->image_view();
        img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = dsts[i];
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &img_info;

        vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);
    }

    // Cierre del desglose (): init de la imagen (con su cadena de mips) ·
    // subida de píxeles · descriptores. El desglose FINO de init se lee de la
    // textura recién creada, no de acumuladores compartidos: init() lo llama
    // también TileTexCache, y sumar sus tiempos acá dentro dividiéndolos por el
    // contador de assets hacía que las partes sumaran más que el todo.
    {
        const auto t3 = std::chrono::steady_clock::now();
        using ms = std::chrono::duration<double, std::milli>;
        up_init_ms_ += ms(t1 - t0).count();
        up_copy_ms_ += ms(t2 - t1).count();
        up_desc_ms_ += ms(t3 - t2).count();

        const VkTexture::InitCost& ic = entry.tex->last_init_cost();
        up_img_ms_ += ic.image_ms;
        up_view_ms_+= ic.view_ms;
        up_stg_ms_ += ic.staging_ms;
        up_log_ms_ += ic.log_ms;
    }

    entry.valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// VkSprite::draw
// ---------------------------------------------------------------------------
void VkSprite::draw(VkContext& ctx, VkCommandBuffer cmd, VkImage target_image,
                    const AytherSpriteSub* subs, uint32_t count,
                    AyArchive* pack,
                    uint32_t emu_w, uint32_t emu_h,
                    const uint8_t* flips, const uint8_t* tint, const uint8_t* slots,
                    const uint8_t* alphas, uint8_t blend) {
    if (!pipeline_ || count == 0) return;
    if (framebuffers_.empty()) return;

    auto flip_of = [&](uint32_t i) -> uint8_t { return flips ? (flips[i] & 3) : 0; };
    auto key_of  = [&](uint32_t i) -> std::string {
        const uint8_t f = flip_of(i);
        return f ? std::string(subs[i].asset_path) + "#" + static_cast<char>('0' + f)
                 : std::string(subs[i].asset_path);
    };

    // ---- Phase 1: Pre-load any missing textures (outside render pass) -------
    //      Corre con o sin pack: ensure_loaded cae al disco para assets sueltos
    //      (autoría en Editar sin pack construido). Los que no resuelven quedan
    //      inválidos en el cache y se saltean en la fase de dibujo (sin placeholder).
    for (uint32_t i = 0; i < count; ++i) {
        ensure_loaded(ctx, cmd, subs[i].asset_path, pack, flip_of(i));
        // Vestuario: precargar la máscara con la MISMA cara que el asset. Si
        // todavía no está lista, ese quad cae al pipeline clásico este frame
        // (mismo pop-in que los assets; una máscara ilegible queda en
        // negative-cache y el quad se tinta entero, con log una vez).
        if (subs[i].mask_path[0] != '\0' && mask_pipeline_ != VK_NULL_HANDLE)
            ensure_loaded(ctx, cmd, subs[i].mask_path, pack, flip_of(i), true);
    }

    // ---- Phase 2: Barrier TRANSFER_DST → COLOR_ATTACHMENT_OPTIMAL ----------
    VkImageMemoryBarrier to_color{};
    to_color.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_color.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_color.newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_color.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.image               = target_image;
    to_color.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    to_color.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_color.dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &to_color);

    // ---- Phase 3: Begin render pass ----------------------------------------
    VkClearValue clear_val{};  // unused (loadOp = LOAD)

    VkRenderPassBeginInfo rp_begin{};
    rp_begin.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass      = render_pass_;
    rp_begin.framebuffer     = framebuffers_[0];
    rp_begin.renderArea      = { {0, 0}, { fb_w_, fb_h_ } };
    rp_begin.clearValueCount = 1;
    rp_begin.pClearValues    = &clear_val;

    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    // ---- Dynamic viewport + scissor ----------------------------------------
    VkViewport vp{};
    vp.x        = 0.0f;
    vp.y        = 0.0f;
    vp.width    = static_cast<float>(fb_w_);
    vp.height   = static_cast<float>(fb_h_);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor{ {0, 0}, { fb_w_, fb_h_ } };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // ---- Bind pipeline -----------------------------------------------------
    // Blend por VARIANTE de pipeline: 1 aditivo () · 2 multiplicar () ·
    // 3 pantalla () · 4 sustractivo · 5 oscurecer · 6 aclarar (). Una
    // variante ausente (su .spv faltó en init) cae al alpha normal — el motivo
    // ya quedó en el log del init.
    VkPipeline blend_pipe = pipeline_;
    if      (blend == 1 && pipeline_add_    != VK_NULL_HANDLE) blend_pipe = pipeline_add_;
    else if (blend == 2 && pipeline_mult_   != VK_NULL_HANDLE) blend_pipe = pipeline_mult_;
    else if (blend == 3 && pipeline_screen_ != VK_NULL_HANDLE) blend_pipe = pipeline_screen_;
    else if (blend == 4 && pipeline_sub_    != VK_NULL_HANDLE) blend_pipe = pipeline_sub_;
    else if (blend == 5 && pipeline_min_    != VK_NULL_HANDLE) blend_pipe = pipeline_min_;
    else if (blend == 6 && pipeline_max_    != VK_NULL_HANDLE) blend_pipe = pipeline_max_;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blend_pipe);

    // ---- Pixel scale factors (emulator → screen) ---------------------------
    const float sx    = static_cast<float>(fb_w_) / static_cast<float>(emu_w);
    const float sy    = static_cast<float>(fb_h_) / static_cast<float>(emu_h);
    const float scr_w = static_cast<float>(fb_w_);
    const float scr_h = static_cast<float>(fb_h_);

    // ---- Draw sprites — grouped by asset_path to minimise descriptor rebinds --
    //
    // Stable-sort a local index array by asset_path so all sprites sharing a
    // texture are consecutive.  Within each group the original SAT order is
    // preserved (stable_sort).  The descriptor set is bound only once per
    // unique texture: N sprites with M unique assets → M vkCmdBindDescriptorSets
    // instead of the previous N (worst-case N == M, best-case M == 1).
    //
    // vkCmdPushConstants and vkCmdDraw are still emitted once per sprite — each
    // sprite has a distinct screen position that must be encoded in push constants.
    // tint (E1 cromático: fundido + cambio de color, Q2.6) + uv identidad
    // (0,0,1,1) = textura entera (el sub-rect es de draw_anim). El layout DEBE
    // matchear sprite.vert (13 floats).
    struct PC { float x, y, w, h, scr_w, scr_h, tr, tg, tb, u0, v0, uw, vh, a; };
    auto tint_of = [&](uint32_t i, int c) -> float {
        // Q2.6 (64 = 1.0), por el ATENUADO de capa: el dim es del compositor y
        // los quads HD también componen — sin esto la capa enfocada se nota
        // sólo en lo que NO tiene asset (justo al revés de lo que se autora).
        return (tint ? (tint[i * 3 + c] / 64.0f) : 1.0f) * dim_;
    };
    auto alpha_of = [&](uint32_t i) -> float {
        // Opacidad del quad: por sub si el productor la manda (tiles de plano,
        // que mezclan capas en UNA llamada), si no la de set_dim. Oscurecer sin
        // bajar la opacidad no alcanza: el HD atenuado seguía TAPANDO la capa
        // enfocada que tiene detrás.
        return alphas ? (alphas[i] / 255.0f) : dim_a_;
    };

    std::vector<uint32_t> order(count);
    std::iota(order.begin(), order.end(), 0u);
    if (slots) {
        // C8: z-order SAT — dibujar por slot DESCENDENTE (frontmost = slot menor =
        // último en el painter's = encima) para respetar la oclusión entre HD
        // superpuestos. Empate por asset_path para agrupar binds donde el slot no
        // decide. Se pierde algo de batching vs el orden por textura, pero el
        // conteo de subs es chico (≤80) y la CORRECCIÓN del z-order manda.
        std::stable_sort(order.begin(), order.end(),
            [&](uint32_t a, uint32_t b) {
                if (slots[a] != slots[b]) return slots[a] > slots[b];
                return std::strcmp(subs[a].asset_path, subs[b].asset_path) < 0;
            });
    } else {
        // Sin slots (tiles de plano): orden por asset_path = batching de descriptores.
        std::stable_sort(order.begin(), order.end(),
            [&](uint32_t a, uint32_t b) {
                return std::strcmp(subs[a].asset_path, subs[b].asset_path) < 0;
            });
    }

    VkDescriptorSet last_ds = VK_NULL_HANDLE;
    // Vestuario: el pipeline es POR QUAD (clásico o de máscara) — se trackea
    // como el descriptor para no re-bindear de más. El bound inicial es el del
    // vkCmdBindPipeline de arriba.
    const VkPipeline classic_pipe = blend_pipe;
    VkPipeline last_pipe = classic_pipe;
    for (uint32_t idx : order) {
        const AytherSpriteSub& s = subs[idx];

        auto it = tex_cache_.find(key_of(idx));   // variante volteada cacheada aparte
        if (it == tex_cache_.end() || !it->second.valid) continue;
        TexEntry& entry = it->second;

        // Push constants: destination rect in screen pixels + swapchain size.
        // w_px/h_px = tamaño EXACTO del bbox (los de pose no son múltiplos de
        // tile; truncar a tiles achataba el HD). 0 = productor tile-exacto.
        const float dw = s.w_px ? static_cast<float>(s.w_px) : s.w_tiles * 8.0f;
        const float dh = s.h_px ? static_cast<float>(s.h_px) : s.h_tiles * 8.0f;

        // Sampler por-quad: MINIFICAR (textura más grande que el quad en pantalla,
        // p.ej. un máster HD 1296² a ~72px de juego) → LINEAR, o el asset se ve
        // pixelado. Ampliar/1:1 (snapshots pixel-art) → NEAREST, nítido.
        const bool minified = entry.tex &&
            (static_cast<float>(entry.tex->width())  > dw * sx * 1.25f ||
             static_cast<float>(entry.tex->height()) > dh * sy * 1.25f);
        VkDescriptorSet ds = (minified && entry.desc_set_lin != VK_NULL_HANDLE)
                                       ? entry.desc_set_lin : entry.desc_set;

        // Vestuario: si el sub trae máscara y su textura está lista, este quad
        // va por el pipeline de 2 samplers con el set combinado; si no (decode
        // en vuelo o máscara ilegible), cae al clásico este frame — tinte
        // entero, el mismo comportamiento de siempre.
        VkPipeline       pipe   = classic_pipe;
        VkPipelineLayout layout = pipe_layout_;
        if (s.mask_path[0] != '\0' && mask_pipeline_ != VK_NULL_HANDLE &&
            blend == 0) {
            const std::string mkey = tex_key(s.mask_path, flip_of(idx), true);
            auto mit = tex_cache_.find(mkey);
            if (mit != tex_cache_.end() && mit->second.valid) {
                const VkDescriptorSet mds = get_mask_set(
                    ctx, key_of(idx), mkey, entry, mit->second, minified);
                if (mds != VK_NULL_HANDLE) {
                    ds     = mds;
                    pipe   = mask_pipeline_;
                    layout = mask_pipe_layout_;
                }
            }
        }
        if (pipe != last_pipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            last_pipe = pipe;
            last_ds   = VK_NULL_HANDLE;   // el bind del set es por-layout
        }

        // Bind only when the descriptor changes — skips redundant rebinds for
        // sprites sharing the same asset_path (and scaling direction).
        if (ds != last_ds) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    layout, 0, 1, &ds, 0, nullptr);
            last_ds = ds;
        }
        // Sub-rect UV del sub (: quads por grupo de paleta de una pose
        // mixta recortan su porción del asset). uw/vh == 0 = productor que no
        // llena los campos (Modo 3, planos, buffers zero-init) → identidad.
        const bool has_uv = s.uw > 0.0f && s.vh > 0.0f;
        const PC pc{
            //  fase 0: el centrado del ensanchado, en espacio emu y antes
            // de escalar. Con off_x_emu = 0 (el default) es el mismo píxel.
            (s.screen_x + wide_off_x_) * sx,
            s.screen_y * sy,
            dw * sx,
            dh * sy,
            scr_w,
            scr_h,
            tint_of(idx, 0), tint_of(idx, 1), tint_of(idx, 2),
            has_uv ? s.u0 : 0.0f, has_uv ? s.v0 : 0.0f,
            has_uv ? s.uw : 1.0f, has_uv ? s.vh : 1.0f,
            alpha_of(idx),
        };
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(PC), &pc);

        // Draw one sprite quad (6 vertices, no vertex buffer).
        vkCmdDraw(cmd, 6, 1, 0, 0);
    }

    // ---- End render pass ---------------------------------------------------
    // The render pass finalLayout (TRANSFER_DST_OPTIMAL) is applied
    // automatically — VkPresent::finalize() works unchanged.
    vkCmdEndRenderPass(cmd);
}

// ---------------------------------------------------------------------------
// VkSprite::draw_anim — Animaciones C-S2: frames HD de un SHEET en fase.
// Igual que draw() pero cada quad muestrea un SUB-RECT (UV) del sheet y el
// destino es un rect FLOAT (sub-píxel, para el tween geométrico Nivel 1).
// Mismo pipeline/render pass: el sub-rect viaja en los push constants.
// ---------------------------------------------------------------------------
void VkSprite::draw_anim(VkContext& ctx, VkCommandBuffer cmd, VkImage target_image,
                         const ayther::AnimHdFrame* frames, uint32_t count,
                         AyArchive* pack,
                         uint32_t emu_w, uint32_t emu_h) {
    if (!pipeline_ || count == 0) return;
    if (framebuffers_.empty()) return;

    // ---- Fase 1: precargar los sheets que falten (fuera del render pass) ----
    for (uint32_t i = 0; i < count; ++i)
        ensure_loaded(ctx, cmd, frames[i].asset, pack);

    // ---- Fase 2/3: mismo barrier + render pass + pipeline que draw() --------
    VkImageMemoryBarrier to_color{};
    to_color.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_color.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_color.newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_color.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.image               = target_image;
    to_color.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    to_color.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_color.dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &to_color);

    VkClearValue clear_val{};  // unused (loadOp = LOAD)
    VkRenderPassBeginInfo rp_begin{};
    rp_begin.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass      = render_pass_;
    rp_begin.framebuffer     = framebuffers_[0];
    rp_begin.renderArea      = { {0, 0}, { fb_w_, fb_h_ } };
    rp_begin.clearValueCount = 1;
    rp_begin.pClearValues    = &clear_val;
    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{};
    vp.width    = static_cast<float>(fb_w_);
    vp.height   = static_cast<float>(fb_h_);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D scissor{ {0, 0}, { fb_w_, fb_h_ } };
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    const float sx    = static_cast<float>(fb_w_) / static_cast<float>(emu_w);
    const float sy    = static_cast<float>(fb_h_) / static_cast<float>(emu_h);
    const float scr_w = static_cast<float>(fb_w_);
    const float scr_h = static_cast<float>(fb_h_);

    struct PC { float x, y, w, h, scr_w, scr_h, tr, tg, tb, u0, v0, uw, vh, a; };

    // Agrupar por sheet (mismo esquema que draw(): un bind por textura única).
    std::vector<uint32_t> order(count);
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(),
        [&](uint32_t a, uint32_t b) {
            return std::strcmp(frames[a].asset, frames[b].asset) < 0;
        });

    VkDescriptorSet last_ds = VK_NULL_HANDLE;
    for (uint32_t idx : order) {
        const ayther::AnimHdFrame& f = frames[idx];

        auto it = tex_cache_.find(f.asset);
        if (it == tex_cache_.end() || !it->second.valid) continue;
        TexEntry& entry = it->second;

        // Sub-rect del sheet en UV normalizado (src_w <= 0 → textura entera).
        const float tw = static_cast<float>(entry.tex->width());
        const float th = static_cast<float>(entry.tex->height());
        float u0 = 0.0f, v0 = 0.0f, uw = 1.0f, vh = 1.0f;
        if (f.src_w > 0.0f && f.src_h > 0.0f && tw > 0.0f && th > 0.0f) {
            u0 = f.src_x / tw;  v0 = f.src_y / th;
            uw = f.src_w / tw;  vh = f.src_h / th;
        }

        // Sampler por-quad (igual que draw()): la FUENTE efectiva es el sub-rect —
        // minificar → LINEAR (sin pixelado); ampliar/1:1 → NEAREST (nítido).
        const float src_w = (f.src_w > 0.0f) ? f.src_w : tw;
        const float src_h = (f.src_h > 0.0f) ? f.src_h : th;
        const bool minified = src_w > f.dst_w * sx * 1.25f || src_h > f.dst_h * sy * 1.25f;
        const VkDescriptorSet ds = (minified && entry.desc_set_lin != VK_NULL_HANDLE)
                                       ? entry.desc_set_lin : entry.desc_set;
        if (ds != last_ds) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipe_layout_, 0, 1, &ds, 0, nullptr);
            last_ds = ds;
        }

        const PC pc{
            f.dst_x * sx, f.dst_y * sy,
            f.dst_w * sx, f.dst_h * sy,
            scr_w, scr_h,
            dim_, dim_, dim_,   // tinte neutro (el E1 es de la lane de sprites)
            u0, v0, uw, vh,
            dim_a_,             // opacidad del atenuado de capa ()
        };
        vkCmdPushConstants(cmd, pipe_layout_, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(PC), &pc);
        vkCmdDraw(cmd, 6, 1, 0, 0);
    }

    vkCmdEndRenderPass(cmd);
}

// ---------------------------------------------------------------------------
// VkSprite::draw_video — el frame vigente del video de la Cinemática ().
//   Ver la nota del header: textura y descriptor PROPIOS, opaco a pantalla
//   completa, LINEAR, y re-subida sólo cuando el contenido cambió.
// ---------------------------------------------------------------------------
void VkSprite::draw_video(VkContext& ctx, VkCommandBuffer cmd, VkImage target_image,
                          const uint8_t* y, uint32_t y_stride,
                          const uint8_t* u, uint32_t u_stride,
                          const uint8_t* v, uint32_t v_stride,
                          uint32_t w, uint32_t h, uint64_t seq) {
    if (!video_pipeline_ || framebuffers_.empty() || !y || !u || !v ||
        w == 0 || h == 0) return;

    // Croma a la mitad en los dos ejes (I420). El redondeo hacia arriba importa
    // en dimensiones impares: un plano de croma de w/2 se quedaria corto.
    const uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    const uint32_t pw[3] = { w, cw, cw };
    const uint32_t ph[3] = { h, ch, ch };
    const uint8_t* src[3] = { y, u, v };
    const uint32_t pitch[3] = { y_stride, u_stride, v_stride };

    // (Re)crear solo si cambian las dimensiones — o sea, al cambiar de clip.
    // vkDeviceWaitIdle ANTES de destruir: las texturas viejas pueden estar
    // referenciadas por command buffers en vuelo. (El viejo draw_overlay omitía
    // esta espera y era un defecto latente suyo; se fue con .)
    if (!video_tex_[0] || video_w_ != w || video_h_ != h) {
        if (video_tex_[0]) vkDeviceWaitIdle(ctx.device());
        for (int i = 0; i < 3; ++i) {
            if (!video_tex_[i]) continue;
            video_tex_[i]->shutdown(ctx);
            delete video_tex_[i];
            video_tex_[i] = nullptr;
        }
        for (int i = 0; i < 3; ++i) {
            video_tex_[i] = new VkTexture();
            // mipmapped=false: el video se dibuja ~1:1 o AMPLIADO, nunca
            // minificado, y la cadena de mips es justo el costo fijo por textura
            // que el presupuesto de 1 upload/frame existe para acotar.
            if (!video_tex_[i]->init(ctx, pw[i], ph[i], /*mipmapped=*/false,
                                     TexImageFormat::R8)) {
                for (int k = 0; k <= i; ++k) {
                    if (!video_tex_[k]) continue;
                    video_tex_[k]->shutdown(ctx);
                    delete video_tex_[k];
                    video_tex_[k] = nullptr;
                }
                return;
            }
        }
        video_w_ = w; video_h_ = h;
        video_seq_ = 0;               // fuerza la primera subida sobre las nuevas
        video_ds_  = VK_NULL_HANDLE;  // el set apuntaba a las vistas viejas
    }
    if (video_ds_ == VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = video_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &video_layout_;
        if (vkAllocateDescriptorSets(ctx.device(), &ai, &video_ds_) != VK_SUCCESS) {
            video_ds_ = VK_NULL_HANDLE; return;
        }
        video_seq_ = 0;   // set nuevo: hay que escribirlo aunque el frame no cambie
    }

    // Solo subir si el contenido cambio. En pausa —o en cualquier re-render de
    // UI sobre la misma FrameView— esto ahorra las tres copias, que son de lejos
    // lo mas caro de esta funcion. Son 1,5 bytes por pixel contra los 4 del BGRA
    // de antes de .
    if (seq != video_seq_) {
        VkDescriptorImageInfo ii[3]{};
        VkWriteDescriptorSet  wr[3]{};
        for (int i = 0; i < 3; ++i) {
            video_tex_[i]->upload(ctx, cmd, src[i], pw[i], ph[i], pitch[i],
                                  TexPixelFormat::Gray8);
            ii[i].sampler     = sampler_;   // LINEAR: el video casi siempre se amplia
            ii[i].imageView   = video_tex_[i]->image_view();
            ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            wr[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[i].dstSet          = video_ds_;
            wr[i].dstBinding      = uint32_t(i);
            wr[i].descriptorCount = 1;
            wr[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr[i].pImageInfo      = &ii[i];
        }
        vkUpdateDescriptorSets(ctx.device(), 3, wr, 0, nullptr);
        video_seq_ = seq;
    }

    // Barrier TRANSFER_DST → COLOR_ATTACHMENT (igual que draw()).
    VkImageMemoryBarrier to_color{};
    to_color.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_color.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_color.newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_color.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.image               = target_image;
    to_color.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    to_color.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_color.dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &to_color);

    VkClearValue clr{};
    VkRenderPassBeginInfo rp{};
    rp.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass      = render_pass_;
    rp.framebuffer     = framebuffers_[0];
    rp.renderArea      = { {0, 0}, { fb_w_, fb_h_ } };
    rp.clearValueCount = 1;
    rp.pClearValues    = &clr;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{}; vp.width = static_cast<float>(fb_w_); vp.height = static_cast<float>(fb_h_);
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D scissor{ {0, 0}, { fb_w_, fb_h_ } };
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, video_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            video_pipe_layout_, 0, 1, &video_ds_, 0, nullptr);

    // UV identidad, igual que el compose: el shader hace rgb *= frag_tint y sin
    // eso el video se pinta negro. 14 floats. El tinte/opacidad SÍ son los del
    // atenuado de capa (dim_/dim_a_, neutros = 1.0): el video es contenido de la
    // escena —la Cinemática reemplaza el fondo— y tiene que atenuarse como el
    // resto cuando se está autorando otra capa.
    struct PC { float x, y, w, h, scr_w, scr_h, tr, tg, tb, u0, v0, uw, vh, a; };
    const PC pc{ 0.0f, 0.0f, static_cast<float>(fb_w_), static_cast<float>(fb_h_),
                 static_cast<float>(fb_w_), static_cast<float>(fb_h_),
                 dim_, dim_, dim_, 0.0f, 0.0f, 1.0f, 1.0f, dim_a_ };
    vkCmdPushConstants(cmd, video_pipe_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(PC), &pc);
    vkCmdDraw(cmd, 6, 1, 0, 0);

    vkCmdEndRenderPass(cmd);
}

// ---------------------------------------------------------------------------
// VkSprite::clear_textures
// ---------------------------------------------------------------------------
// Free all cached sprite textures and reset the descriptor pool so all
// allocated descriptor sets are reclaimed.  The pipeline, render pass,
// framebuffers, sampler, and descriptor set layout are left intact — no
// need to re-call init() after a pack hotreload.
//
// PRECONDITION: caller must have called vkDeviceWaitIdle() before invoking
// this function so that no submitted command buffer references the textures
// that are about to be freed.
// ---------------------------------------------------------------------------
void VkSprite::clear_textures(VkContext& ctx) {
    {   // : invalidar decodes en vuelo (los resultados con gen vieja se tiran)
        std::lock_guard<std::mutex> lk(decode_mx_);
        decode_jobs_.clear();
        decode_done_.clear();
        ++decode_gen_;
    }
    for (auto& [_, entry] : tex_cache_) {
        if (entry.valid && entry.tex) {
            entry.tex->shutdown(ctx);
            delete entry.tex;
        }
    }
    tex_cache_.clear();
    staging_release_.clear();   // : sin cache no queda staging que liberar

    // Reset the pools to reclaim all allocated descriptor sets.
    // The pools were created WITHOUT VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT
    // so vkFreeDescriptorSets is not available — full reset is the only option.
    // La cadena se conserva (capacidad ya creada); solo se vacían los sets.
    for (VkDescriptorPool pool : desc_pools_)
        vkResetDescriptorPool(ctx.device(), pool, 0);
    // Vestuario: los sets combinados apuntaban a texturas recién destruidas.
    mask_set_cache_.clear();
    for (VkDescriptorPool pool : mask_pools_)
        vkResetDescriptorPool(ctx.device(), pool, 0);

    std::fprintf(stdout, "[VkSprite] Texture cache cleared (pack hotreload)\n");
}

// ---------------------------------------------------------------------------
// VkSprite::poll_disk — hot-reload de autoría (2026-07-24): el artista guarda
// un PNG nuevo en graphics/ FUERA del Lab y el juego seguía mostrando el
// viejo (cache por-path sin invalidación externa). Stat de los assets con
// origen DISCO: mtime distinto → evict (el próximo draw recarga); entrada
// negative-cacheada (-2) cuyo archivo ahora existe → evict (levanta el
// negative-cache). Llamar a baja cadencia (~1×/s).
// ---------------------------------------------------------------------------
void VkSprite::poll_disk(VkContext& ctx) {
    std::vector<std::string> dirty;
    for (const auto& [key, e] : tex_cache_) {
        if (e.pending || e.stale || e.disk_mtime == -1) continue;
        // Clave de variante volteada = "path#N"; de máscara (Vestuario) =
        // "path#m" o "path#mN" → stat sobre el path base en todos los casos.
        std::string path = key;
        const size_t len = key.size();
        if (len > 3 && key[len - 3] == '#' && key[len - 2] == 'm')
            path = key.substr(0, len - 3);                       // "path#mN"
        else if (len > 2 && key[len - 2] == '#' && key[len - 1] == 'm')
            path = key.substr(0, len - 2);                       // "path#m"
        else if (len > 2 && key[len - 2] == '#')
            path = key.substr(0, len - 2);                       // caras del asset
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(path, ec);
        if (e.disk_mtime == -2) {
            if (!ec) dirty.push_back(path);          // el archivo APARECIÓ
        } else if (!ec &&
                   static_cast<int64_t>(t.time_since_epoch().count()) !=
                       e.disk_mtime) {
            dirty.push_back(path);                   // reescrito en disco
        }
        // stat fallido sobre una entrada válida (archivo borrado): conservar
        // la textura cargada — evictar dejaría el sub sin nada que dibujar.
    }
    std::sort(dirty.begin(), dirty.end());
    dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
    for (const auto& p : dirty) {
        std::fprintf(stdout, "[VkSprite] Asset cambiado en disco -> reload: %s\n",
                     p.c_str());
        evict(ctx, p);
    }
}

// ---------------------------------------------------------------------------
// VkSprite::evict
// ---------------------------------------------------------------------------
// Saca `path` (y sus variantes volteadas `path..3`) del cache para que el
// próximo draw lo recargue del pack/disco — para assets reescritos EN VIVO (el
// snapshot de una pose regenerado durante el armado en Posar). La entrada queda
// `stale` conservando su descriptor set: el pool no tiene FREE_DESCRIPTOR_SET_BIT
// (no hay free individual) y re-escribir el set al recargar es válido. Un evict
// de un path que había FALLADO al cargar también levanta el negative-cache (el
// PNG puede existir recién ahora). GPU idle sólo si se destruye una textura que
// pudo quedar referenciada por el frame anterior.
void VkSprite::evict(VkContext& ctx, const std::string& path) {
    bool waited = false;
    for (int f = 0; f < 8; ++f) {
        // f 0-3 = asset y sus caras; f 4-7 = las claves de MÁSCARA (Vestuario,
        // `#m`/`#mN`) del mismo path. El ++seq invalida los sets combinados
        // (get_mask_set los re-escribe al ver el seq nuevo).
        const std::string key = f >= 4
            ? tex_key(path, static_cast<uint8_t>(f - 4), true)
            : tex_key(path, static_cast<uint8_t>(f), false);
        auto it = tex_cache_.find(key);
        if (it == tex_cache_.end()) continue;
        TexEntry& e = it->second;
        if (e.tex) {
            if (!waited) { vkDeviceWaitIdle(ctx.device()); waited = true; }
            e.tex->shutdown(ctx);
            delete e.tex;
            e.tex = nullptr;
        }
        e.valid = false;
        e.stale = true;
        e.pending = false;   // : un decode en vuelo queda invalidado por seq
        ++e.seq;
    }
}
