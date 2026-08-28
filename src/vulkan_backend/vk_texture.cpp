#include <chrono>
#include "vulkan_backend/vk_texture.h"
#include "vulkan_backend/vk_context.h"
#include <vk_mem_alloc.h>   // real VMA API (header forward-declares the handle only)
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// init / shutdown
// ---------------------------------------------------------------------------
// Desglose de init (). La medición dijo primero que el 94% del upload vivía
// acá dentro, después que las tres asignaciones (imagen device-local con su
// cadena de mips, vista, staging host-visible MAPEADO) sumaban 10 ms de los
// ~725 que el tramo decía costar. Lo único que quedaba fuera de los relojes era
// el fprintf del final: era el 92%. Los tiempos quedan como guarda permanente —
// baratos, y es lo que delataría que volvió a entrar trabajo no cronometrado.
bool VkTexture::init(VkContext& ctx, uint32_t max_w, uint32_t max_h, bool mipmapped,
                     TexImageFormat::Value img_fmt) {
    // : un canal para los planos del video. El resto del archivo asumia 4
    // bytes por pixel en cuatro lugares distintos (image, view, staging, paso
    // de fila); ahora sale de `bpp_` y no de una constante repetida.
    bpp_ = (img_fmt == TexImageFormat::R8) ? 1u : 4u;
    const VkFormat vk_fmt = (img_fmt == TexImageFormat::R8)
                          ? VK_FORMAT_R8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
    const auto tA = std::chrono::steady_clock::now();
    width_  = max_w;
    height_ = max_h;
    mip_levels_ = 1;
    if (mipmapped) {
        uint32_t m = max_w > max_h ? max_w : max_h;
        while (m > 1) { ++mip_levels_; m >>= 1; }
    }

    // ---- GPU image (device-local, BGRA8) -----------------------------------
    VkImageCreateInfo img{};
    img.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img.imageType     = VK_IMAGE_TYPE_2D;
    img.format        = vk_fmt;
    img.extent        = { max_w, max_h, 1 };
    img.mipLevels     = mip_levels_;
    img.arrayLayers   = 1;
    img.samples       = VK_SAMPLE_COUNT_1_BIT;
    img.tiling        = VK_IMAGE_TILING_OPTIMAL;
    img.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_SAMPLED_BIT      |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // blit to swapchain
    img.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    img.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(ctx.allocator(), &img, &ai,
                       &image_, &image_alloc_, nullptr) != VK_SUCCESS) {
        std::fprintf(stderr, "[VkTexture] vmaCreateImage failed\n");
        return false;
    }

    const auto tB = std::chrono::steady_clock::now();

    // ---- Image view --------------------------------------------------------
    VkImageViewCreateInfo vi{};
    vi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image    = image_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = vk_fmt;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels_, 0, 1 };

    if (vkCreateImageView(ctx.device(), &vi, nullptr, &image_view_) != VK_SUCCESS) {
        std::fprintf(stderr, "[VkTexture] vkCreateImageView failed\n");
        return false;
    }

    const auto tC = std::chrono::steady_clock::now();

    // ---- Staging buffer (host-visible, persistently mapped) ----------------
    VkDeviceSize staging_size = static_cast<VkDeviceSize>(max_w) * max_h * bpp_;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size  = staging_size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo sai{};
    sai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;
    sai.usage = VMA_MEMORY_USAGE_AUTO;

    VmaAllocationInfo sinfo{};
    if (vmaCreateBuffer(ctx.allocator(), &bi, &sai,
                        &staging_buf_, &staging_alloc_, &sinfo) != VK_SUCCESS) {
        std::fprintf(stderr, "[VkTexture] vmaCreateBuffer (staging) failed\n");
        return false;
    }
    staging_map_ = sinfo.pMappedData;

    const auto tD = std::chrono::steady_clock::now();

    if (vk_verbose_logging())
        std::fprintf(stdout,
            "[VkTexture] Ready  %ux%u  staging=%zu bytes\n",
            max_w, max_h, static_cast<size_t>(staging_size));

    {
        const auto tE = std::chrono::steady_clock::now();
        using msd = std::chrono::duration<double, std::milli>;
        init_cost_.image_ms   = msd(tB - tA).count();
        init_cost_.view_ms    = msd(tC - tB).count();
        init_cost_.staging_ms = msd(tD - tC).count();
        init_cost_.log_ms     = msd(tE - tD).count();
    }
    return true;
}

void VkTexture::shutdown(VkContext& ctx) {
    if (image_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(ctx.device(), image_view_, nullptr);
        image_view_ = VK_NULL_HANDLE;
    }
    if (image_ != VK_NULL_HANDLE) {
        vmaDestroyImage(ctx.allocator(), image_, image_alloc_);
        image_       = VK_NULL_HANDLE;
        image_alloc_ = nullptr;
    }
    if (staging_buf_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(ctx.allocator(), staging_buf_, staging_alloc_);
        staging_buf_   = VK_NULL_HANDLE;
        staging_alloc_ = nullptr;
        staging_map_   = nullptr;
    }
}

size_t VkTexture::release_staging(VkContext& ctx) {
    if (staging_buf_ == VK_NULL_HANDLE) return 0;
    vmaDestroyBuffer(ctx.allocator(), staging_buf_, staging_alloc_);
    staging_buf_   = VK_NULL_HANDLE;
    staging_alloc_ = nullptr;
    staging_map_   = nullptr;
    return static_cast<size_t>(width_) * height_ * bpp_;
}

// ---------------------------------------------------------------------------
// upload
// ---------------------------------------------------------------------------
// Convert a line of RGB565 pixels to BGRA8 in the staging buffer.
static void convert_rgb565_to_bgra8(const uint8_t* src, uint8_t* dst,
                                     uint32_t w, size_t src_pitch) {
    for (uint32_t y = 0; y < 1; ++y) {   // called per-row
        const uint16_t* s = reinterpret_cast<const uint16_t*>(src);
        uint8_t*        d = dst;
        for (uint32_t x = 0; x < w; ++x) {
            uint16_t px = s[x];
            uint8_t r5 = (px >> 11) & 0x1F;
            uint8_t g6 = (px >>  5) & 0x3F;
            uint8_t b5 =  px        & 0x1F;
            d[0] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));  // B
            d[1] = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));  // G
            d[2] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));  // R
            d[3] = 0xFF;                                           // A
            d += 4;
        }
    }
}

static void convert_xrgb8888_to_bgra8(const uint8_t* src, uint8_t* dst,
                                        uint32_t w) {
    // XRGB8888: byte order is X-R-G-B in little-endian → 0xXXRRGGBB
    // BGRA8:    byte order B-G-R-A
    for (uint32_t x = 0; x < w; ++x) {
        uint32_t px;
        std::memcpy(&px, src + x * 4, 4);
        dst[0] = (px      ) & 0xFF;  // B
        dst[1] = (px >>  8) & 0xFF;  // G
        dst[2] = (px >> 16) & 0xFF;  // R
        dst[3] = 0xFF;               // X byte → opaque alpha
        dst += 4;
    }
}

/// Bgra8888: data is already in BGRA layout (alpha preserved).
/// Used for HD sprite PNGs decoded by stb_image (RGBA → R↔B swapped to BGRA).
static void copy_bgra8888(const uint8_t* src, uint8_t* dst, uint32_t w) {
    std::memcpy(dst, src, static_cast<size_t>(w) * 4);
}

void VkTexture::upload(VkContext& ctx, VkCommandBuffer cmd,
                       const void* pixels, uint32_t w, uint32_t h,
                       size_t pitch, TexPixelFormat::Value fmt) {
    if (!pixels || w == 0 || h == 0) return;
    if (!staging_map_) {
        // release_staging() ya corrió: esta textura era de UN upload ().
        if (image_ != VK_NULL_HANDLE)
            std::fprintf(stderr,
                "[VkTexture] upload tras release_staging IGNORADO (%ux%u)\n",
                width_, height_);
        return;
    }

    // Clamp to our max dimensions
    w = (w < width_)  ? w : width_;
    h = (h < height_) ? h : height_;

    // --- Convert and copy into staging buffer --------------------------------
    const uint8_t* src_row = static_cast<const uint8_t*>(pixels);
    uint8_t*       dst_row = static_cast<uint8_t*>(staging_map_);

    for (uint32_t y = 0; y < h; ++y) {
        switch (fmt) {
            case TexPixelFormat::Rgb565:
                convert_rgb565_to_bgra8(src_row, dst_row, w, pitch);
                break;
            case TexPixelFormat::Xrgb8888:
                convert_xrgb8888_to_bgra8(src_row, dst_row, w);
                break;
            case TexPixelFormat::Rgb1555:
                // Treat as Rgb565 (close enough for emulator display)
                convert_rgb565_to_bgra8(src_row, dst_row, w, pitch);
                break;
            case TexPixelFormat::Bgra8888:
                // Already BGRA — direct copy, preserves alpha channel.
                copy_bgra8888(src_row, dst_row, w);
                break;
            case TexPixelFormat::Gray8:
                // : un byte por pixel, tal cual. Es el camino de los planos
                // Y/U/V del video; el image tiene que haberse creado R8.
                std::memcpy(dst_row, src_row, w);
                break;
        }
        src_row += pitch;
        dst_row += static_cast<size_t>(width_) * bpp_;
    }
    vmaFlushAllocation(ctx.allocator(), staging_alloc_, 0, VK_WHOLE_SIZE);

    // --- Pipeline barrier: UNDEFINED → TRANSFER_DST (TODOS los niveles) -----
    VkImageMemoryBarrier barrier_to_dst{};
    barrier_to_dst.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier_to_dst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier_to_dst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier_to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier_to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier_to_dst.image               = image_;
    barrier_to_dst.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels_, 0, 1 };
    barrier_to_dst.srcAccessMask       = 0;
    barrier_to_dst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr,
        1, &barrier_to_dst);

    // --- Buffer → image copy (nivel 0) ---------------------------------------
    VkBufferImageCopy region{};
    region.bufferOffset      = 0;
    region.bufferRowLength   = width_;   // staging row pitch in pixels
    region.bufferImageHeight = 0;
    region.imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageOffset       = { 0, 0, 0 };
    region.imageExtent       = { w, h, 1 };

    vkCmdCopyBufferToImage(cmd, staging_buf_, image_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // --- Cadena de mips: blit LINEAR nivel i-1 → i (assets minificados) ------
    // Dims por nivel derivadas de la IMAGEN (los assets se suben completos:
    // w==width_). Cada nivel pasa DST→SRC antes de ser fuente del siguiente.
    int32_t mw = static_cast<int32_t>(width_), mh = static_cast<int32_t>(height_);
    for (uint32_t i = 1; i < mip_levels_; ++i) {
        VkImageMemoryBarrier src_b{};
        src_b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        src_b.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        src_b.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        src_b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        src_b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        src_b.image               = image_;
        src_b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 1, 0, 1 };
        src_b.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        src_b.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &src_b);

        const int32_t nw = mw > 1 ? mw / 2 : 1, nh = mh > 1 ? mh / 2 : 1;
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1 };
        blit.srcOffsets[1]  = { mw, mh, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1 };
        blit.dstOffsets[1]  = { nw, nh, 1 };
        vkCmdBlitImage(cmd,
            image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR);
        mw = nw; mh = nh;
    }

    // --- Pipeline barrier: → SHADER_READ_ONLY (todos los niveles) ------------
    // Con mips: niveles 0..n-2 quedaron en TRANSFER_SRC y el último en DST →
    // dos barriers. Sin mips: una sola (DST → READ), como siempre.
    if (mip_levels_ > 1) {
        VkImageMemoryBarrier reads[2]{};
        for (auto& b : reads) {
            b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image               = image_;
            b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        }
        reads[0].oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        reads[0].srcAccessMask    = VK_ACCESS_TRANSFER_READ_BIT;
        reads[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels_ - 1, 0, 1 };
        reads[1].oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        reads[1].srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
        reads[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip_levels_ - 1, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr,
            2, reads);
        return;
    }
    VkImageMemoryBarrier barrier_to_read{};
    barrier_to_read.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier_to_read.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier_to_read.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier_to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier_to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier_to_read.image               = image_;
    barrier_to_read.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier_to_read.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier_to_read.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr,
        1, &barrier_to_read);
}
