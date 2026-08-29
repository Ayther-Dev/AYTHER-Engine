// ---------------------------------------------------------------------------
// TileTexCache — see tile_tex_cache.h.
//
// This TU owns the single STB_IMAGE_IMPLEMENTATION *and*
// STB_IMAGE_WRITE_IMPLEMENTATION for the engine + the frontends (vk_sprite.cpp,
// metasprite_export.cpp and ayther_background_export.cpp use the declarations
// and link these symbols).
// ---------------------------------------------------------------------------
#include "vulkan_backend/tile_tex_cache.h"
#include "vulkan_backend/vk_context.h"
#include "ayther_core_ffi.h"   // ayther_pack_file_size / ayther_pack_read

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstdio>
#include <cstdint>
#include <vector>
#include <algorithm>           // std::swap

VkTexture* TileTexCache::get_or_load(const std::string& asset_path,
                                     AyArchive*         pack,
                                     VkContext&         ctx,
                                     VkCommandBuffer    cmd) {
    auto it = map.find(asset_path);
    if (it != map.end())
        return it->second.valid ? &it->second.tex : nullptr;

    Entry& entry = map[asset_path];
    entry.valid  = false;

    if (!pack) return nullptr;

    // ---- Read from pack ----
    int64_t file_sz = ayther_pack_file_size(pack, asset_path.c_str());
    if (file_sz <= 0) {
        std::fprintf(stderr, "[TileTexCache] not found: %s\n", asset_path.c_str());
        return nullptr;
    }
    std::vector<uint8_t> raw(static_cast<size_t>(file_sz));
    if (ayther_pack_read(pack, asset_path.c_str(), raw.data(), raw.size()) <= 0)
        return nullptr;

    // ---- Decode PNG ----
    int w, h, ch;
    uint8_t* pixels = stbi_load_from_memory(
        raw.data(), static_cast<int>(raw.size()), &w, &h, &ch, 4);
    if (!pixels) {
        std::fprintf(stderr, "[TileTexCache] decode failed: %s\n", asset_path.c_str());
        return nullptr;
    }

    // Swap R↔B: RGBA → BGRA (VK_FORMAT_B8G8R8A8_UNORM)
    for (int i = 0; i < w * h; ++i)
        std::swap(pixels[i * 4 + 0], pixels[i * 4 + 2]);

    // ---- Upload to GPU ----
    if (!entry.tex.init(ctx, static_cast<uint32_t>(w), static_cast<uint32_t>(h))) {
        stbi_image_free(pixels);
        std::fprintf(stderr, "[TileTexCache] VkTexture::init failed: %s\n", asset_path.c_str());
        return nullptr;
    }
    entry.tex.upload(ctx, cmd, pixels,
                     static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                     static_cast<size_t>(w * 4),
                     TexPixelFormat::Xrgb8888);  // already BGRA after swap
    stbi_image_free(pixels);

    entry.valid = true;
    // Tile = UN solo upload: staging a la lista diferida ().
    staging_release_.push_back({ &entry, tick_ + kStagingLingerPumps });
    if (vk_verbose_logging())   // una línea por asset = ~5 ms de consola ()
        std::fprintf(stdout, "[TileTexCache] loaded: %s (%dx%d)\n", asset_path.c_str(), w, h);
    return &entry.tex;
}

void TileTexCache::pump(VkContext& ctx) {
    ++tick_;
    if (staging_release_.empty()) return;
    size_t freed = 0;
    int    n     = 0;
    auto keep = staging_release_.begin();
    for (auto& r : staging_release_) {
        if (r.due > tick_) { *keep++ = r; continue; }
        freed += r.e->tex.release_staging(ctx);
        ++n;
    }
    staging_release_.erase(keep, staging_release_.end());
    if (freed && vk_verbose_logging())
        std::fprintf(stdout, "[TileTexCache] staging liberado: %d tex (+%zu KB)\n",
                     n, freed / 1024);
}

void TileTexCache::shutdown(VkContext& ctx) {
    (void)ctx;
    staging_release_.clear();   // : los Entry* dejan de existir
    map.clear();
}
