#pragma once
// ---------------------------------------------------------------------------
// VkRenderTarget — the motor's offscreen color image (R3).
//
// The HD frame is rendered here instead of straight to the swapchain, so each
// frontend can decide what to do with it:
//   - ayther_play blits it to its swapchain (present to window);
//   - ayther_lab samples it in an ImGui viewport panel.
//
// Usage flags cover every consumer in one image:
//   COLOR_ATTACHMENT  — the VkSprite / VkPostProcess graphics passes
//   TRANSFER_DST      — the emu-frame + HD-tile vkCmdBlitImage blits
//   TRANSFER_SRC      — the frontend blit to the swapchain
//   SAMPLED           — the Lab viewport (ImGui) / shader sampling
//
// Owned by AytherRenderer and recreated on resize. The target captures the
// device/allocator handles it needs, so destruction is automatic; shutdown()
// remains available for deterministic early release.
// ---------------------------------------------------------------------------
#include <vulkan/vulkan.h>
#include <cstdint>

// VMA handle is a pointer typedef — forward-declare so this PUBLIC header keeps
// <vk_mem_alloc.h> out of the frontend (matches vk_texture.h).
struct VmaAllocation_T;
using  VmaAllocation = VmaAllocation_T*;
struct VmaAllocator_T;
using  VmaAllocator = VmaAllocator_T*;

class VkContext;

class VkRenderTarget {
public:
    VkRenderTarget()  = default;
    ~VkRenderTarget();

    VkRenderTarget(const VkRenderTarget&)            = delete;
    VkRenderTarget& operator=(const VkRenderTarget&) = delete;

    // Create the offscreen color image + view + sampler at width × height.
    // The default format is the UNORM color target the sprite/postprocess passes
    // already assume, and is blit-compatible with the swapchain.
    bool init(VkContext& ctx, uint32_t width, uint32_t height,
              VkFormat format = VK_FORMAT_B8G8R8A8_UNORM);

    // Recreate at a new size (window / viewport resize): shutdown() + init().
    bool resize(VkContext& ctx, uint32_t width, uint32_t height);

    // Destroy all resources. Safe on a partially-initialized / empty target.
    void shutdown(VkContext& ctx);

    bool is_ready() const { return image_ != VK_NULL_HANDLE; }

    // ---- Accessors ---------------------------------------------------------
    VkImage     image()   const { return image_;   }
    VkImageView view()    const { return view_;    }   // SAMPLED + attachment
    VkSampler   sampler() const { return sampler_; }   // LINEAR, for sampling
    VkFormat    format()  const { return format_;  }
    VkExtent2D  extent()  const { return extent_;  }

private:
    void release() noexcept;

    VkDevice       device_    = VK_NULL_HANDLE;
    VmaAllocator   allocator_ = nullptr;
    VkImage       image_   = VK_NULL_HANDLE;
    VmaAllocation alloc_   = nullptr;
    VkImageView   view_    = VK_NULL_HANDLE;
    VkSampler     sampler_ = VK_NULL_HANDLE;
    VkFormat      format_  = VK_FORMAT_UNDEFINED;
    VkExtent2D    extent_  = {};
};
