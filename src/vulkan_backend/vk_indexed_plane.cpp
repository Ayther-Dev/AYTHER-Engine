// ---------------------------------------------------------------------------
// VkIndexedPlane — pipeline indexado (R-2, ). Ver el header por el diseño.
//
// Deudas conocidas y a propósito (R-2 valida el mecanismo, no lo integra):
//   - un draw por celda (≈1.2k draws/plano) — batching por instancia es una
//     optimización de R-5, cuando el pase reemplace al blit.
//   - el pass asume el mismo contrato de layouts que VkSprite para poder
//     insertarse en la cadena de AytherRenderer sin barreras nuevas.
// ---------------------------------------------------------------------------
#include "vulkan_backend/vk_indexed_plane.h"
#include "log.h"
#include <ayther/engine/vulkan_interop.hpp>
#include "vulkan_backend/vk_render_target.h"
#include "ayther_file.h"
#include <vk_mem_alloc.h>   // real VMA API (header forward-declares the handle only)

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kIdxW = 512;   // 64 tiles por fila × 8 texels
constexpr uint32_t kIdxH = 256;   // 32 filas de tiles (2048 patrones)
constexpr uint32_t kPalW = 64;    // 4 líneas × 16 colores

std::vector<uint32_t> load_spv(const char* path) {
    FILE* f = ayther::file_open(path, "rb");
    if (!f) {
        ayther::log::write(ayther::log::Severity::Error,
            "vulkan.plane", "cannot_open_shader",
            "Cannot open shader: %s",
            path);
        return {};
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::rewind(f);
    if (sz <= 0 || (sz % 4) != 0) {
        ayther::log::write(ayther::log::Severity::Warning,
            "vulkan.plane", "bad_spir_v_size",
            "Bad SPIR-V size (%ld): %s",
            sz,
            path);
        std::fclose(f);
        return {};
    }
    std::vector<uint32_t> code(static_cast<size_t>(sz) / 4);
    std::fread(code.data(), 1, static_cast<size_t>(sz), f);
    std::fclose(f);
    return code;
}

VkShaderModule make_shader_module(const ayther::engine::VulkanContextView& ctx, const std::vector<uint32_t>& code) {
    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size() * sizeof(uint32_t);
    info.pCode    = code.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(ctx.device(), &info, nullptr, &mod);
    return mod;
}

// Imagen 2D device-local + staging host-visible persistentemente mapeado
// (molde de VkTexture::init, con formato parametrizado — VkTexture es BGRA8
// fijo y esta textura es R8_UINT).
bool make_image(const ayther::engine::VulkanContextView& ctx, uint32_t w, uint32_t h, VkFormat fmt,
                VkImage& image, VmaAllocation& alloc, VkImageView& view) {
    VkImageCreateInfo ii{};
    ii.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType     = VK_IMAGE_TYPE_2D;
    ii.format        = fmt;
    ii.extent        = { w, h, 1 };
    ii.mipLevels     = 1;
    ii.arrayLayers   = 1;
    ii.samples       = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ii.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(ctx.allocator(), &ii, &ai, &image, &alloc, nullptr) != VK_SUCCESS) {
        ayther::log::write(ayther::log::Severity::Error,
            "vulkan.plane", "vmacreateimage_x_failed",
            "vmaCreateImage %ux%u failed",
            w,
            h);
        return false;
    }
    VkImageViewCreateInfo vi{};
    vi.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image            = image;
    vi.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    vi.format           = fmt;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(ctx.device(), &vi, nullptr, &view) != VK_SUCCESS) {
        ayther::log::write(ayther::log::Severity::Error,
            "vulkan.plane", "vkcreateimageview_failed",
            "vkCreateImageView failed");
        return false;
    }
    return true;
}

bool make_staging(const ayther::engine::VulkanContextView& ctx, VkDeviceSize size,
                  VkBuffer& buf, VmaAllocation& alloc, void*& map) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size  = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo ai{};
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
               VMA_ALLOCATION_CREATE_MAPPED_BIT;
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(ctx.allocator(), &bi, &ai, &buf, &alloc, &info) != VK_SUCCESS) {
        ayther::log::write(ayther::log::Severity::Error,
            "vulkan.plane", "staging_b_failed",
            "staging (%zu B) failed",
            (size_t)size);
        return false;
    }
    map = info.pMappedData;
    return map != nullptr;
}

// Barrera de imagen simple (molde de ayther_renderer.cpp).
void image_barrier(VkCommandBuffer cmd, VkImage image,
                   VkImageLayout from, VkImageLayout to,
                   VkAccessFlags src_access, VkAccessFlags dst_access,
                   VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout           = from;
    b.newLayout           = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = image;
    b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b.srcAccessMask       = src_access;
    b.dstAccessMask       = dst_access;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace

// ---------------------------------------------------------------------------
// Conversión de color única — CPU y paleta GPU comparten ESTA función.
// ---------------------------------------------------------------------------
uint32_t VkIndexedPlane::genesis_color_rgba(uint16_t packed) {
    // CRAM del core: R=bits0-2, G=3-5, B=6-8 (empaquetada, sin huecos — el
    // gotcha gemelo de decode_plane_tile). Canal de 3 bits → nivel "normal"
    // ×2 (0..14, pixel_lut[1] del core) → RGB565 con la expansión de
    // MAKE_PIXEL → 888 replicando bits altos (la misma del tríptico del spike
    // R-1): así el pixel GPU es byte-idéntico al del framebuffer del emulador.
    const unsigned r4 = ((packed >> 0) & 7) << 1;
    const unsigned g4 = ((packed >> 3) & 7) << 1;
    const unsigned b4 = ((packed >> 6) & 7) << 1;
    const unsigned r5 = (r4 << 1) | (r4 >> 3);
    const unsigned g6 = (g4 << 2) | (g4 >> 2);
    const unsigned b5 = (b4 << 1) | (b4 >> 3);
    const unsigned r8 = (r5 << 3) | (r5 >> 2);
    const unsigned g8 = (g6 << 2) | (g6 >> 4);
    const unsigned b8 = (b5 << 3) | (b5 >> 2);
    return r8 | (g8 << 8) | (b8 << 16) | 0xFF000000u;
}

// ---------------------------------------------------------------------------
// init / shutdown
// ---------------------------------------------------------------------------
VkIndexedPlane::~VkIndexedPlane() {
    if (context_) shutdown(*context_);
}

bool VkIndexedPlane::init(const ayther::engine::VulkanContextView& ctx, const VkRenderTarget& target,
                          const char* vert_spv_path, const char* frag_spv_path) {
    context_ = &ctx;
    extent_ = target.extent();
    if (!create_images(ctx)) { shutdown(ctx); return false; }
    if (!create_pipeline(ctx, target.format(), vert_spv_path, frag_spv_path)) {
        shutdown(ctx);
        return false;
    }

    // Framebuffer sobre el view del render target (molde de VkSprite).
    VkFramebufferCreateInfo fi{};
    fi.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass      = render_pass_;
    fi.attachmentCount = 1;
    VkImageView att    = target.view();
    fi.pAttachments    = &att;
    fi.width           = extent_.width;
    fi.height          = extent_.height;
    fi.layers          = 1;
    if (vkCreateFramebuffer(ctx.device(), &fi, nullptr, &framebuffer_) != VK_SUCCESS) {
        ayther::log::write(ayther::log::Severity::Error,
            "vulkan.plane", "vkcreateframebuffer_failed",
            "vkCreateFramebuffer failed");
        shutdown(ctx);
        return false;
    }
    return true;
}

bool VkIndexedPlane::rebuild(const ayther::engine::VulkanContextView& ctx, const VkRenderTarget& target) {
    if (!is_ready()) return false;
    if (framebuffer_) {
        vkDestroyFramebuffer(ctx.device(), framebuffer_, nullptr);
        framebuffer_ = VK_NULL_HANDLE;
    }
    extent_ = target.extent();
    VkFramebufferCreateInfo fi{};
    fi.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass      = render_pass_;
    fi.attachmentCount = 1;
    VkImageView att    = target.view();
    fi.pAttachments    = &att;
    fi.width           = extent_.width;
    fi.height          = extent_.height;
    fi.layers          = 1;
    if (vkCreateFramebuffer(ctx.device(), &fi, nullptr, &framebuffer_) != VK_SUCCESS) {
        ayther::log::write(ayther::log::Severity::Error,
            "vulkan.plane", "rebuild_framebuffer_failed",
            "rebuild framebuffer failed");
        return false;
    }
    return true;
}

bool VkIndexedPlane::create_images(const ayther::engine::VulkanContextView& ctx) {
    if (!make_image(ctx, kIdxW, kIdxH, VK_FORMAT_R8_UINT,
                    idx_image_, idx_alloc_, idx_view_)) return false;
    if (!make_staging(ctx, kIdxW * kIdxH, idx_staging_, idx_staging_alloc_,
                      idx_staging_map_)) return false;
    if (!make_image(ctx, kPalW, 1, VK_FORMAT_R8G8B8A8_UNORM,
                    pal_image_, pal_alloc_, pal_view_)) return false;
    if (!make_staging(ctx, kPalW * 4, pal_staging_, pal_staging_alloc_,
                      pal_staging_map_)) return false;

    VkSamplerCreateInfo si{};
    si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter    = VK_FILTER_NEAREST;
    si.minFilter    = VK_FILTER_NEAREST;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(ctx.device(), &si, nullptr, &sampler_) != VK_SUCCESS) {
        ayther::log::write(ayther::log::Severity::Error,
            "vulkan.plane", "vkcreatesampler_failed",
            "vkCreateSampler failed");
        return false;
    }
    return true;
}

bool VkIndexedPlane::create_pipeline(const ayther::engine::VulkanContextView& ctx, VkFormat fmt,
                                     const char* vert_spv_path,
                                     const char* frag_spv_path) {
    // ---- Render pass: contrato idéntico a VkSprite (LOAD; entra en
    //      COLOR_ATTACHMENT_OPTIMAL, sale en TRANSFER_DST_OPTIMAL) ------------
    VkAttachmentDescription att{};
    att.format         = fmt;
    att.samples        = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.finalLayout    = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &color_ref;

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
        ayther::log::write(ayther::log::Severity::Error,
            "vulkan.plane", "vkcreaterenderpass_failed",
            "vkCreateRenderPass failed");
        return false;
    }

    auto vert_code = load_spv(vert_spv_path);
    auto frag_code = load_spv(frag_spv_path);
    if (vert_code.empty() || frag_code.empty()) return false;
    VkShaderModule vert_mod = make_shader_module(ctx, vert_code);
    VkShaderModule frag_mod = make_shader_module(ctx, frag_code);
    if (!vert_mod || !frag_mod) {
        if (vert_mod) vkDestroyShaderModule(ctx.device(), vert_mod, nullptr);
        if (frag_mod) vkDestroyShaderModule(ctx.device(), frag_mod, nullptr);
        ayther::log::write(ayther::log::Severity::Error,
            "vulkan.plane", "vkcreateshadermodule_failed",
            "vkCreateShaderModule failed");
        return false;
    }

    // ---- Descriptor set layout: binding 0 = índices, binding 1 = paleta ------
    VkDescriptorSetLayoutBinding bindings[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dsl_info{};
    dsl_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_info.bindingCount = 2;
    dsl_info.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(ctx.device(), &dsl_info, nullptr, &desc_layout_)
            != VK_SUCCESS) {
        vkDestroyShaderModule(ctx.device(), vert_mod, nullptr);
        vkDestroyShaderModule(ctx.device(), frag_mod, nullptr);
        return false;
    }

    // ---- Push constants: 6 floats + 5 uints, VERTEX|FRAGMENT (R-6: el frag
    //      lee fx/flat_rgba del mismo bloque; R-8 suma checker). DEBE matchear
    //      struct PC (draw_cells) y el uniform PC de ambos shaders — o falla
    //      mudo. --------------------------------------------------------------
    VkPushConstantRange pc_range{};
    pc_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc_range.offset     = 0;
    pc_range.size       = 6 * sizeof(float) + 9 * sizeof(uint32_t);   // 60 B ( v3: +nb0..nb3)

    VkPipelineLayoutCreateInfo pl_info{};
    pl_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_info.setLayoutCount         = 1;
    pl_info.pSetLayouts            = &desc_layout_;
    pl_info.pushConstantRangeCount = 1;
    pl_info.pPushConstantRanges    = &pc_range;
    if (vkCreatePipelineLayout(ctx.device(), &pl_info, nullptr, &pipe_layout_)
            != VK_SUCCESS) {
        vkDestroyShaderModule(ctx.device(), vert_mod, nullptr);
        vkDestroyShaderModule(ctx.device(), frag_mod, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_mod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_mod;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vtx_input{};
    vtx_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_asm{};
    input_asm.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_asm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp_state{};
    vp_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp_state.viewportCount = 1;
    vp_state.scissorCount  = 1;
    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dyn_states;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = VK_CULL_MODE_NONE;
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // ---- Blending R-6: srcAlpha/1-srcAlpha en COLOR (la opacidad por
    //      elemento); el alpha del offscreen se PRESERVA opaco (ZERO/ONE,
    //      mismo criterio que VkSprite). Con opacidad 255 el resultado es
    //      src*1 + dst*0 = src EXACTO — la validación bit a bit del compose
    //      sigue valiendo (scene_compose_smoke lo demuestra en cada corrida).
    //      La transparencia del índice 0 sigue siendo discard, no alpha. -----
    VkPipelineColorBlendAttachmentState blend_att{};
    blend_att.blendEnable         = VK_TRUE;
    blend_att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_att.colorBlendOp        = VK_BLEND_OP_ADD;
    blend_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_att.alphaBlendOp        = VK_BLEND_OP_ADD;
    blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments    = &blend_att;

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
    gp_info.layout              = pipe_layout_;
    gp_info.renderPass          = render_pass_;
    gp_info.subpass             = 0;
    VkResult res = vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE,
                                             1, &gp_info, nullptr, &pipeline_);
    vkDestroyShaderModule(ctx.device(), vert_mod, nullptr);
    vkDestroyShaderModule(ctx.device(), frag_mod, nullptr);
    if (res != VK_SUCCESS) {
        ayther::log::write(ayther::log::Severity::Error,
            "vulkan.plane", "vkcreategraphicspipelines_failed",
            "vkCreateGraphicsPipelines failed (%d)",
            res);
        return false;
    }

    // ---- Descriptor set único (las dos texturas nunca cambian de identidad) --
    VkDescriptorPoolSize pool_size{};
    pool_size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 2;
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets       = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes    = &pool_size;
    if (vkCreateDescriptorPool(ctx.device(), &dpi, nullptr, &desc_pool_) != VK_SUCCESS)
        return false;
    VkDescriptorSetAllocateInfo dsa{};
    dsa.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsa.descriptorPool     = desc_pool_;
    dsa.descriptorSetCount = 1;
    dsa.pSetLayouts        = &desc_layout_;
    if (vkAllocateDescriptorSets(ctx.device(), &dsa, &desc_set_) != VK_SUCCESS)
        return false;

    VkDescriptorImageInfo infos[2]{};
    infos[0].sampler     = sampler_;
    infos[0].imageView   = idx_view_;
    infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infos[1].sampler     = sampler_;
    infos[1].imageView   = pal_view_;
    infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = desc_set_;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo      = &infos[i];
    }
    vkUpdateDescriptorSets(ctx.device(), 2, writes, 0, nullptr);
    return true;
}

void VkIndexedPlane::shutdown(const ayther::engine::VulkanContextView& ctx) {
    VkDevice dev = ctx.device();
    if (framebuffer_) { vkDestroyFramebuffer(dev, framebuffer_, nullptr); framebuffer_ = VK_NULL_HANDLE; }
    if (desc_pool_)   { vkDestroyDescriptorPool(dev, desc_pool_, nullptr); desc_pool_ = VK_NULL_HANDLE; desc_set_ = VK_NULL_HANDLE; }
    if (pipeline_)    { vkDestroyPipeline(dev, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (pipe_layout_) { vkDestroyPipelineLayout(dev, pipe_layout_, nullptr); pipe_layout_ = VK_NULL_HANDLE; }
    if (desc_layout_) { vkDestroyDescriptorSetLayout(dev, desc_layout_, nullptr); desc_layout_ = VK_NULL_HANDLE; }
    if (render_pass_) { vkDestroyRenderPass(dev, render_pass_, nullptr); render_pass_ = VK_NULL_HANDLE; }
    if (sampler_)     { vkDestroySampler(dev, sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
    if (idx_view_)    { vkDestroyImageView(dev, idx_view_, nullptr); idx_view_ = VK_NULL_HANDLE; }
    if (idx_image_)   { vmaDestroyImage(ctx.allocator(), idx_image_, idx_alloc_); idx_image_ = VK_NULL_HANDLE; idx_alloc_ = nullptr; }
    if (idx_staging_) { vmaDestroyBuffer(ctx.allocator(), idx_staging_, idx_staging_alloc_); idx_staging_ = VK_NULL_HANDLE; idx_staging_alloc_ = nullptr; idx_staging_map_ = nullptr; }
    if (pal_view_)    { vkDestroyImageView(dev, pal_view_, nullptr); pal_view_ = VK_NULL_HANDLE; }
    if (pal_image_)   { vmaDestroyImage(ctx.allocator(), pal_image_, pal_alloc_); pal_image_ = VK_NULL_HANDLE; pal_alloc_ = nullptr; }
    if (pal_staging_) { vmaDestroyBuffer(ctx.allocator(), pal_staging_, pal_staging_alloc_); pal_staging_ = VK_NULL_HANDLE; pal_staging_alloc_ = nullptr; pal_staging_map_ = nullptr; }
    idx_uploaded_ = pal_uploaded_ = vram_seen_ = cram_seen_ = false;
    context_ = nullptr;
}

// ---------------------------------------------------------------------------
// Subidas incrementales
// ---------------------------------------------------------------------------
void VkIndexedPlane::upload_vram(const ayther::engine::VulkanContextView& ctx, VkCommandBuffer cmd,
                                 const uint8_t* vram, size_t size) {
    if (!vram || size < sizeof(vram_shadow_) || !idx_staging_map_) return;

    // Tiles sucios contra el shadow (32 bytes crudos por patrón — el
    // word-swap del host no cruza el bloque del tile, así que comparar los
    // bytes crudos detecta exactamente los patrones que cambiaron).
    std::vector<VkBufferImageCopy> regions;
    regions.reserve(64);
    uint8_t* stage = static_cast<uint8_t*>(idx_staging_map_);
    for (uint32_t t = 0; t < 2048; ++t) {
        const uint8_t* raw = vram + t * 32;
        if (vram_seen_ && std::memcmp(raw, vram_shadow_ + t * 32, 32) == 0) continue;

        // Desempaquetar el tile a nibbles en su posición del staging (layout
        // = la textura completa, bufferRowLength kIdxW). Vista de bus: off^1.
        const uint32_t bx = (t % 64) * 8, by = (t / 64) * 8;
        for (uint32_t row = 0; row < 8; ++row) {
            uint8_t* dst = stage + (by + row) * kIdxW + bx;
            for (uint32_t bi = 0; bi < 4; ++bi) {
                const uint32_t off  = t * 32 + row * 4 + bi;
                const uint8_t  byte = vram[off ^ 1u];
                dst[bi * 2 + 0] = byte >> 4;      // pixel par: nibble alto
                dst[bi * 2 + 1] = byte & 0x0F;    // pixel impar: nibble bajo
            }
        }
        VkBufferImageCopy r{};
        r.bufferOffset      = by * kIdxW + bx;
        r.bufferRowLength   = kIdxW;
        r.bufferImageHeight = kIdxH;
        r.imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r.imageOffset       = { (int32_t)bx, (int32_t)by, 0 };
        r.imageExtent       = { 8, 8, 1 };
        regions.push_back(r);
    }
    if (regions.empty()) return;
    std::memcpy(vram_shadow_, vram, sizeof(vram_shadow_));
    vram_seen_ = true;
    vmaFlushAllocation(ctx.allocator(), idx_staging_alloc_, 0, VK_WHOLE_SIZE);

    image_barrier(cmd, idx_image_,
        idx_uploaded_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        idx_uploaded_ ? VK_ACCESS_SHADER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        idx_uploaded_ ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(cmd, idx_staging_, idx_image_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           (uint32_t)regions.size(), regions.data());
    image_barrier(cmd, idx_image_,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    idx_uploaded_ = true;
}

void VkIndexedPlane::upload_cram(const ayther::engine::VulkanContextView& ctx, VkCommandBuffer cmd,
                                 const uint8_t* cram, size_t size) {
    if (!cram || size < sizeof(cram_shadow_) || !pal_staging_map_) return;
    if (cram_seen_ && std::memcmp(cram, cram_shadow_, sizeof(cram_shadow_)) == 0) return;
    std::memcpy(cram_shadow_, cram, sizeof(cram_shadow_));
    cram_seen_ = true;

    uint32_t* stage = static_cast<uint32_t*>(pal_staging_map_);
    for (uint32_t i = 0; i < kPalW; ++i) {
        const uint16_t v = (uint16_t)(cram[i * 2] | (cram[i * 2 + 1] << 8));
        stage[i] = genesis_color_rgba(v);
    }
    vmaFlushAllocation(ctx.allocator(), pal_staging_alloc_, 0, VK_WHOLE_SIZE);

    image_barrier(cmd, pal_image_,
        pal_uploaded_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        pal_uploaded_ ? VK_ACCESS_SHADER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        pal_uploaded_ ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy r{};
    r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.imageExtent      = { kPalW, 1, 1 };
    vkCmdCopyBufferToImage(cmd, pal_staging_, pal_image_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    image_barrier(cmd, pal_image_,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    pal_uploaded_ = true;
}

// ---------------------------------------------------------------------------
// draw_cells
// ---------------------------------------------------------------------------
void VkIndexedPlane::draw_cells(const ayther::engine::VulkanContextView& ctx, VkCommandBuffer cmd,
                                const CellQuad* cells, size_t count,
                                uint32_t canvas_w, uint32_t canvas_h,
                                int32_t scissor_x) {
    (void)ctx;
    if (!is_ready() || !idx_uploaded_ || !pal_uploaded_ || !cells || !count) return;

    VkRenderPassBeginInfo rp{};
    rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass        = render_pass_;
    rp.framebuffer       = framebuffer_;
    rp.renderArea.extent = extent_;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout_,
                            0, 1, &desc_set_, 0, nullptr);

    VkViewport vp{ 0.f, 0.f, (float)extent_.width, (float)extent_.height, 0.f, 1.f };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    if (scissor_x < 0) scissor_x = 0;
    if ((uint32_t)scissor_x > extent_.width) scissor_x = (int32_t)extent_.width;
    VkRect2D sc{ { scissor_x, 0 }, { extent_.width - (uint32_t)scissor_x, extent_.height } };
    vkCmdSetScissor(cmd, 0, 1, &sc);

    // DEBE matchear pc_range (create_pipeline) y el uniform PC de los shaders.
    struct PC { float x, y, w, h, canvas_w, canvas_h;
                uint32_t tile, attr, fx, flat_rgba, checker, nb0, nb1, nb2, nb3; };
    for (size_t i = 0; i < count; ++i) {
        const CellQuad& c = cells[i];
        PC pc{ c.x, c.y, c.w, c.h, (float)canvas_w, (float)canvas_h,
               (uint32_t)(c.pattern & 0x7FF),
               // attr: bits 0-1 flips · 2-3 paleta · 4 = EPX () ·
               //       5-12 = intensidad k 0..255 ()
               (uint32_t)((c.flips & 3u) | ((c.pal & 3u) << 2) |
                          (c.enhance ? 16u : 0u) |
                          ((uint32_t)c.enhance_k << 5)),
               c.fx, c.flat_rgba, c.checker, c.nb0, c.nb1, c.nb2, c.nb3 };
        vkCmdPushConstants(cmd, pipe_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(PC), &pc);
        vkCmdDraw(cmd, 6, 1, 0, 0);
    }
    vkCmdEndRenderPass(cmd);
}
