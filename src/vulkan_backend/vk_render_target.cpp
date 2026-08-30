// ---------------------------------------------------------------------------
// VkRenderTarget — offscreen color image (R3). See vk_render_target.h.
// ---------------------------------------------------------------------------
#include "vulkan_backend/vk_render_target.h"
#include "log.h"
#include "vulkan_backend/vk_context.h"
#include <vk_mem_alloc.h>   // real VMA API (header forward-declares the handle)
#include <cstdio>

VkRenderTarget::~VkRenderTarget() { release(); }

bool VkRenderTarget::init(VkContext& ctx, uint32_t width, uint32_t height,
                          VkFormat format) {
    if (width == 0 || height == 0) {
        ayther::log::write(ayther::log::Severity::Warning,
            "vulkan.target", "init_zero_extent",
            "init: zero extent");
        return false;
    }
    release();
    device_ = ctx.device();
    allocator_ = ctx.allocator();
    format_ = format;
    extent_ = { width, height };

    // ---- Color image (device-local) ----------------------------------------
    VkImageCreateInfo img{};
    img.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img.imageType     = VK_IMAGE_TYPE_2D;
    img.format        = format;
    img.extent        = { width, height, 1 };
    img.mipLevels     = 1;
    img.arrayLayers   = 1;
    img.samples       = VK_SAMPLE_COUNT_1_BIT;
    img.tiling        = VK_IMAGE_TILING_OPTIMAL;
    img.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |  // sprite/postprocess
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT     |  // emu/tile blits
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT     |  // blit to swapchain
                        VK_IMAGE_USAGE_SAMPLED_BIT;            // Lab viewport
    img.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    img.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(ctx.allocator(), &img, &ai, &image_, &alloc_, nullptr)
            != VK_SUCCESS) {
        ayther::log::write(ayther::log::Severity::Error,
            "vulkan.target", "vmacreateimage_failed",
            "vmaCreateImage failed");
        return false;
    }

    // ---- View ---------------------------------------------------------------
    VkImageViewCreateInfo vi{};
    vi.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image            = image_;
    vi.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    vi.format           = format;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    if (vkCreateImageView(ctx.device(), &vi, nullptr, &view_) != VK_SUCCESS) {
        ayther::log::write(ayther::log::Severity::Error,
            "vulkan.target", "vkcreateimageview_failed",
            "vkCreateImageView failed");
        return false;
    }

    // ---- Sampler (LINEAR, clamp) — for ImGui / shader sampling --------------
    VkSamplerCreateInfo si{};
    si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod       = 1.0f;

    if (vkCreateSampler(ctx.device(), &si, nullptr, &sampler_) != VK_SUCCESS) {
        ayther::log::write(ayther::log::Severity::Error,
            "vulkan.target", "vkcreatesampler_failed",
            "vkCreateSampler failed");
        return false;
    }

    ayther::log::write(ayther::log::Severity::Info,
        "vulkan.target", "ready_x_fmt",
        "Ready  %ux%u  fmt=%d",
        width,
        height,
        static_cast<int>(format));
    return true;
}

bool VkRenderTarget::resize(VkContext& ctx, uint32_t width, uint32_t height) {
    shutdown(ctx);
    return init(ctx, width, height, format_ == VK_FORMAT_UNDEFINED
                                        ? VK_FORMAT_B8G8R8A8_UNORM : format_);
}

void VkRenderTarget::shutdown(VkContext& ctx) {
    (void)ctx;
    release();
}

void VkRenderTarget::release() noexcept {
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    if (view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, view_, nullptr);
        view_ = VK_NULL_HANDLE;
    }
    if (image_ != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator_, image_, alloc_);
        image_ = VK_NULL_HANDLE;
        alloc_ = nullptr;
    }
    device_ = VK_NULL_HANDLE;
    allocator_ = nullptr;
}
