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
class  VkContext;

struct TileTexCache {
    ~TileTexCache() = default;

    struct Entry { VkTexture tex; bool valid = false; };
    std::unordered_map<std::string, Entry> map;

    /// Returns a ready VkTexture*, or nullptr if the asset is missing/invalid.
    /// `cmd` must be active (inside begin_frame/end_frame).
    VkTexture* get_or_load(const std::string& asset_path,
                           AyArchive*         pack,
                           VkContext&         ctx,
                           VkCommandBuffer    cmd);

    /// : llamar 1×/frame — libera el staging de los uploads con el fence ya
    /// garantizado (los tiles suben UNA vez; el staging retenido eran ~80 KB
    /// por tile HD × cientos de tiles). Entry* es estable (unordered_map es
    /// node-based y acá no hay evict individual — solo shutdown total).
    void pump(VkContext& ctx);

    void shutdown(VkContext& ctx);

private:
    struct StagingRelease { Entry* e; uint64_t due; };
    static constexpr uint64_t kStagingLingerPumps = 4;  // frames-in-flight + margen
    std::vector<StagingRelease> staging_release_;
    uint64_t                    tick_ = 0;
};
