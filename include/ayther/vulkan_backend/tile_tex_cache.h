#pragma once
// ---------------------------------------------------------------------------
// TileTexCache — lazy-load + cache HD tile textures from an .ay pack.
//
// First access per asset_path:  read bytes from pack → decode PNG (stb_image) →
// swap R↔B (RGBA→BGRA) → VkTexture init()+upload(). Subsequent accesses return
// the cached VkTexture directly. Owned by AytherRenderer.
// ---------------------------------------------------------------------------
#include "vulkan_backend/vk_texture.h"

#include <unordered_map>
#include <string>
#include <vector>

struct AyArchive;   // opaque (ayther_core_ffi.h)


struct TileTexCache {
    ~TileTexCache() = default;

    struct Entry { VkTexture tex; bool valid = false; };
    std::unordered_map<std::string, Entry> map;

    /// Returns a ready VkTexture*, or nullptr if the asset is missing/invalid.
    /// `cmd` must be active (inside begin_frame/end_frame).
    VkTexture* get_or_load(const std::string& asset_path,
                           AyArchive*         pack,
                           const ayther::engine::VulkanContextView&         ctx,
                           VkCommandBuffer    cmd);

    /// Call 1×/frame — releases the staging memory of uploads whose fence is
    /// already guaranteed (tiles upload ONCE; the retained staging was ~80 KB
    /// per HD tile × hundreds of tiles). Entry* is stable (unordered_map is
    /// node-based and there is no individual eviction here — only full
    /// shutdown).
    void pump(const ayther::engine::VulkanContextView& ctx);

    void shutdown(const ayther::engine::VulkanContextView& ctx);

private:
    struct StagingRelease { Entry* e; uint64_t due; };
    static constexpr uint64_t kStagingLingerPumps = 4;  // frames-in-flight + margin
    std::vector<StagingRelease> staging_release_;
    uint64_t                    tick_ = 0;
};
