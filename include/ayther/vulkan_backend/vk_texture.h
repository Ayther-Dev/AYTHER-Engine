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
        Gray8     = 4,  // : un byte por píxel, tal cual (planos Y/U/V del video)
    };
};

/// : formato del IMAGE en GPU, que es distinto del formato de ORIGEN.
/// Hasta acá toda VkTexture era BGRA8; los planos de luma y croma del video son
/// de UN canal, y subirlos como BGRA sería pagar cuatro bytes por píxel para
/// llevar uno — justo lo que  vino a sacar.
struct TexImageFormat {
    enum Value : uint32_t {
        Bgra8 = 0,   // VK_FORMAT_B8G8R8A8_UNORM (todo lo anterior)
        R8    = 1,   // VK_FORMAT_R8_UNORM       (planos del video)
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
    // mipmapped: genera la cadena de mips en upload() (vkCmdBlitImage LINEAR) —
    // para texturas de ASSETS que se minifican (un máster HD dibujado chico
    // aliasea sin mips). Las texturas por-frame (emu fb) quedan en false: su
    // upload es cada frame y no se minifican.
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

    /// : libera SOLO el staging (el image queda vivo). Para texturas de UN
    /// upload (assets: el staging quedaba retenido toda la vida de la textura
    /// — hasta ~10 MB por máster). Llamar cuando la copia grabada YA ejecutó
    /// en GPU (≥ frames-in-flight después del upload). Un upload posterior
    /// sería un bug (el staging no existe): se loguea y se ignora.
    /// Devuelve los bytes liberados (0 si ya no había staging).
    size_t release_staging(VkContext& ctx);

    bool is_ready() const { return image_ != VK_NULL_HANDLE; }

    VkImage     image()      const { return image_;      }
    VkImageView image_view() const { return image_view_; }
    uint32_t    width()      const { return width_;      }
    uint32_t    height()     const { return height_;     }

    /// Desglose de lo que costó el último init(), en ms (). Vive en la
    /// INSTANCIA y no en acumuladores de archivo porque init() tiene dos
    /// llamadores —VkSprite por asset, TileTexCache por tile— y unos
    /// acumuladores compartidos divididos por el contador de uno solo daban una
    /// cifra de control absurda: las partes sumaban MÁS que el todo (0,183
    /// contra 0,090 en la primera muestra). El llamador acumula lo suyo.
    struct InitCost {
        double image_ms   = 0.0;   // vmaCreateImage + cadena de mips
        double view_ms    = 0.0;   // vkCreateImageView
        double staging_ms = 0.0;   // buffer host-visible persistentemente mapeado
        double log_ms     = 0.0;   // el fprintf, que resultó ser el 92% del total
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
    /// : bytes por píxel del IMAGE (4 = BGRA8, 1 = R8). Gobierna el tamaño
    /// del staging y el paso de fila; estaba fijo en 4 por todo el archivo.
    uint32_t      bpp_        = 4;
};
