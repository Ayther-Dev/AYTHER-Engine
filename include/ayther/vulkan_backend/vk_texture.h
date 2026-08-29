#pragma once
// ---------------------------------------------------------------------------
// VkTexture — a single GPU image + staging buffer for CPU→GPU upload.
//
// Used to upload the emulator's software framebuffer (RGB565 / XRGB8888)
// every frame.  A persistent staging buffer avoids per-frame allocation.
//
// Usage:
//   init(ctx, width, height)        — once
//   upload(ctx, cmd, pixels, ...)   — inside a command buffer (begin..end)
//   shutdown(ctx)                    — optional early release
//
// The image layout at the end of upload() is
//   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL (suitable for blit src).
// ---------------------------------------------------------------------------
#include <vulkan/vulkan.h>
#include <cstddef>
#include <cstdint>

// Forward-declare the VMA handle (a pointer typedef) so this PUBLIC header does
// not pull in <vk_mem_alloc.h>; VMA stays private to the engine .cpp TUs
// (matches vk_context.h's treatment of VmaAllocator).
struct VmaAllocation_T;
using  VmaAllocation = VmaAllocation_T*;
struct VmaAllocator_T;
using  VmaAllocator = VmaAllocator_T*;

class VkContext;  // forward

struct TexPixelFormat {
    enum Value : uint32_t {
        Rgb1555   = 0,  // RETRO_PIXEL_FORMAT_0RGB1555
        Xrgb8888  = 1,  // RETRO_PIXEL_FORMAT_XRGB8888
        Rgb565    = 2,  // RETRO_PIXEL_FORMAT_RGB565   (Genesis default)
        Bgra8888  = 3,  // pre-swapped BGRA (sprite PNGs after R↔B swap); alpha preserved
        Gray8     = 4,  // one byte per pixel, as-is (video Y/U/V planes)
    };
};

/// Format of the IMAGE on the GPU, which differs from the SOURCE format.
/// Until now every VkTexture was BGRA8; the luma and chroma planes of video are
/// SINGLE-channel, and uploading them as BGRA would mean paying four bytes per
/// pixel to carry one.
struct TexImageFormat {
    enum Value : uint32_t {
        Bgra8 = 0,   // VK_FORMAT_B8G8R8A8_UNORM (everything before this)
        R8    = 1,   // VK_FORMAT_R8_UNORM       (video planes)
    };
};

class VkTexture {
public:
    VkTexture()  = default;
    ~VkTexture();

    VkTexture(const VkTexture&)            = delete;
    VkTexture& operator=(const VkTexture&) = delete;

    // Create the GPU image (BGRA8 internally) and a host-visible staging buffer
    // large enough for max_w × max_h × 4 bytes.
    // mipmapped: generates the mip chain in upload() (vkCmdBlitImage LINEAR) —
    // for ASSET textures that get minified (an HD master drawn small aliases
    // without mips). Per-frame textures (the emu framebuffer) stay false: they
    // upload every frame and are never minified.
    bool init(VkContext& ctx, uint32_t max_w, uint32_t max_h, bool mipmapped = false,
              TexImageFormat::Value img_fmt = TexImageFormat::Bgra8);

    // Upload 'pixels' (in the given retro pixel format) into the staging buffer
    // and record a pipeline barrier + buffer-to-image copy into 'cmd'.
    // 'cmd' must be in the recording state.
    // The image will be in SHADER_READ_ONLY_OPTIMAL after the call.
    void upload(VkContext& ctx, VkCommandBuffer cmd,
                const void* pixels, uint32_t w, uint32_t h, size_t pitch,
                TexPixelFormat::Value fmt);

    void shutdown(VkContext& ctx);

    /// Releases ONLY the staging buffer (the image stays alive). For textures
    /// with a SINGLE upload (assets: the staging used to be retained for the
    /// whole lifetime of the texture — up to ~10 MB per master). Call it once
    /// the recorded copy HAS executed on the GPU (≥ frames-in-flight after the
    /// upload). A later upload would be a bug (the staging no longer exists):
    /// it is logged and ignored.
    /// Returns the bytes released (0 if there was no staging left).
    size_t release_staging(VkContext& ctx);

    bool is_ready() const { return image_ != VK_NULL_HANDLE; }

    VkImage     image()      const { return image_;      }
    VkImageView image_view() const { return image_view_; }
    uint32_t    width()      const { return width_;      }
    uint32_t    height()     const { return height_;     }

    /// Breakdown of what the last init() cost, in ms. It lives on the
    /// INSTANCE and not in file-level accumulators because init() has two
    /// callers —VkSprite per asset, TileTexCache per tile— and shared
    /// accumulators divided by the counter of only one of them produced an
    /// absurd control figure: the parts summed to MORE than the whole (0.183
    /// against 0.090 in the first sample). Each caller accumulates its own.
    struct InitCost {
        double image_ms   = 0.0;   // vmaCreateImage + mip chain
        double view_ms    = 0.0;   // vkCreateImageView
        double staging_ms = 0.0;   // persistently mapped host-visible buffer
        double log_ms     = 0.0;   // the fprintf, which turned out to be 92% of the total
    };
    const InitCost& last_init_cost() const { return init_cost_; }

private:
    void release() noexcept;

    VkDevice       device_    = VK_NULL_HANDLE;
    VmaAllocator   allocator_ = nullptr;
    InitCost      init_cost_{};

    VkImage       image_       = VK_NULL_HANDLE;
    VkImageView   image_view_  = VK_NULL_HANDLE;
    VmaAllocation image_alloc_ = nullptr;

    VkBuffer      staging_buf_  = VK_NULL_HANDLE;
    VmaAllocation staging_alloc_= nullptr;
    void*         staging_map_  = nullptr;  // persistently mapped

    uint32_t      width_      = 0;
    uint32_t      height_     = 0;
    uint32_t      mip_levels_ = 1;
    /// Bytes per pixel of the IMAGE (4 = BGRA8, 1 = R8). It governs the
    /// staging size and the row stride; it used to be hard-coded to 4
    /// throughout the file.
    uint32_t      bpp_        = 4;
};
