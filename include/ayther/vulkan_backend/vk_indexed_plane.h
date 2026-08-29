#pragma once
// ---------------------------------------------------------------------------
// VkIndexedPlane — the INDEXED pipeline of our own renderer (R-2).
//
// Instead of decoding tiles into RGBA textures, it uploads the RAW VDP STATE
// and resolves the colour in the fragment shader:
//   - VRAM (2048 4bpp patterns) → a 512×256 R8_UINT texture (64 tiles per row,
//     each tile 8×8 texels = its unpacked colour-index nibble).
//   - CRAM (64 packed colours R0-2/G3-5/B6-8) → a 64×1 RGBA8 texture, converted
//     with the SAME colour expansion as the core renderer (3 bits → level ×2 →
//     RGB565 → 888 by bit replication), so the result is comparable BIT FOR BIT
//     against the emulator framebuffer.
//   - Every 8×8 px quad carries pattern + palette line + flips through a push
//     constant; the shader maps index → colour. Index 0 is discarded (VDP
//     semantics: transparent).
//
// Why this way (from the epic): a palette fade no longer invalidates anything
// (64 texels change), and a per-element effect is one uniform per quad, not a
// new lane. Uploads are INCREMENTAL: a CPU shadow of VRAM/CRAM, and only the
// tiles/palette that changed travel to the GPU.
//
// Layout contract (identical to VkSprite): the render target must be in
// COLOR_ATTACHMENT_OPTIMAL on entry to draw_cells(); the pass leaves it in
// TRANSFER_DST_OPTIMAL. upload_*() are recorded OUTSIDE a render pass.
// Ordering contract: at least one upload_vram()+upload_cram() before the first
// draw_cells() (the images are born UNDEFINED; a draw without an upload is a
// no-op).
// ---------------------------------------------------------------------------
#include <vulkan/vulkan.h>
#include <cstddef>
#include <cstdint>

struct VmaAllocation_T;
using  VmaAllocation = VmaAllocation_T*;

class VkContext;
class VkRenderTarget;

class VkIndexedPlane {
public:
    VkIndexedPlane()  = default;
    ~VkIndexedPlane();

    VkIndexedPlane(const VkIndexedPlane&)            = delete;
    VkIndexedPlane& operator=(const VkIndexedPlane&) = delete;

    bool init(VkContext& ctx, const VkRenderTarget& target,
              const char* vert_spv_path, const char* frag_spv_path);
    /// R-5: the offscreen changed (resize) — recreates ONLY the framebuffer
    /// over the new view (pipeline, textures and shadows persist; the format
    /// does not change).
    bool rebuild(VkContext& ctx, const VkRenderTarget& target);
    void shutdown(VkContext& ctx);
    bool is_ready() const { return pipeline_ != VK_NULL_HANDLE; }

    /// Raw core VRAM (64 KB, the host word-swapped view — the same one
    /// video_ram() returns). It compares against the shadow per tile (32 bytes)
    /// and uploads ONLY the tiles that changed (one copy region per dirty
    /// tile).
    void upload_vram(VkContext& ctx, VkCommandBuffer cmd,
                     const uint8_t* vram, size_t size);

    /// Raw core CRAM (128 bytes, GPX layout 0000BBB0GGG0RRR0). If it changed,
    /// it re-converts the 64 colours and uploads the whole palette (256
    /// bytes).
    void upload_cram(VkContext& ctx, VkCommandBuffer cmd,
                     const uint8_t* cram, size_t size);

    /// One plane tile on screen: destination in canvas px + identity.
    /// R-5: quad w/h in canvas px — the compose scales the native 8×8 to the
    /// canvas (e.g. ×6 at 1920); the default of 8 keeps 1:1 usage.
    struct CellQuad {
        float    x = 0, y = 0;   ///< top-left corner in canvas px
        float    w = 8, h = 8;   ///< quad size in canvas px
        uint16_t pattern = 0;    ///< pattern index 0..2047
        uint8_t  pal     = 0;    ///< palette line 0..3
        uint8_t  flips   = 0;    ///< bit0 hflip · bit1 vflip
        /// R-6: per-quad effects. fx = Q2.6 tint r|g<<8|b<<16 (64 = neutral) +
        /// opacity<<24 (255 = opaque); the default is EXACTLY the path without
        /// effects (byte-identical — validated by scene_compose).
        /// flat_rgba ≠ 0 = silhouette mode: flat colour AABBGGRR where idx≠0.
        uint32_t fx       = 0xFF404040u;
        uint32_t flat_rgba = 0;
        /// R-8: UV checker mode — bit0 = on · bit1 = colour pair (0 amber
        /// "never authored" · 1 magenta "the asset did not load") ·
        /// bits 8-19/20-31 = local offset of the quad within the ELEMENT in emu
        /// px (anchors the pattern to the element, not to the screen).
        /// 0 = normal.
        uint32_t checker  = 0;
        /// runtime_enhancement: 1 = the quad is drawn with the EPX/Scale2x
        /// upscaler over palette INDICES (bit 4 of `attr` in the push
        /// constant). 0 = normal path, byte-identical to the usual one.
        uint8_t  enhance  = 0;
        /// Smoothing strength 0..255 (bits 5-12 of `attr`); 255 = clean vector.
        /// Only counts with enhance = 1.
        uint8_t  enhance_k = 255;
        /// v2/v3: the quad's 8 NEIGHBOURING TILES for the upscaler — in SCREEN
        /// space, 14 bits each: bits 0-10 pattern · 11 hflip · 12 vflip ·
        /// 13 valid. nb0 = left | right<<14 · nb1 = up | down<<14 ·
        /// nb2 = up-left | up-right<<14 · nb3 = down-left | down-right<<14.
        /// With the 3×3 ring the shader reaches any pixel at distance 2 (what
        /// xBR asks for). 0 = no neighbour (it clamps to the tile). They are
        /// only filled in with `enhance`.
        uint32_t nb0 = 0, nb1 = 0, nb2 = 0, nb3 = 0;
        static uint32_t neighbor(uint16_t pattern, uint8_t flips) {
            return (uint32_t)(pattern & 0x7FF) | ((uint32_t)(flips & 3) << 11) | (1u << 13);
        }
    };

    /// Draws the quads (one draw + push constant per cell — the model of the
    /// epic: one effect per element = one uniform per quad). scissor_x crops
    /// columns from the left (the VDP left-column blanking, reg 0 bit 5).
    void draw_cells(VkContext& ctx, VkCommandBuffer cmd,
                    const CellQuad* cells, size_t count,
                    uint32_t canvas_w, uint32_t canvas_h, int32_t scissor_x = 0);

    /// The SINGLE colour conversion (packed CRAM → RGBA8) used by the uploaded
    /// palette and by any CPU comparator (the smoke test). Same as the core:
    /// 3-bit channel → normal level ×2 (0..14) → RGB565 → 888 by bit
    /// replication. Returns 0xAABBGGRR (R in the low byte — RGBA memory
    /// order).
    static uint32_t genesis_color_rgba(uint16_t packed);

private:
    VkContext* context_ = nullptr;
    bool create_images(VkContext& ctx);
    bool create_pipeline(VkContext& ctx, VkFormat fmt,
                         const char* vert_spv_path, const char* frag_spv_path);

    // Index texture (512×256 R8_UINT) + persistent staging.
    VkImage       idx_image_ = VK_NULL_HANDLE;
    VkImageView   idx_view_  = VK_NULL_HANDLE;
    VmaAllocation idx_alloc_ = nullptr;
    VkBuffer      idx_staging_       = VK_NULL_HANDLE;
    VmaAllocation idx_staging_alloc_ = nullptr;
    void*         idx_staging_map_   = nullptr;
    bool          idx_uploaded_ = false;   ///< first upload done (layout defined)

    // Palette (64×1 RGBA8) + persistent staging.
    VkImage       pal_image_ = VK_NULL_HANDLE;
    VkImageView   pal_view_  = VK_NULL_HANDLE;
    VmaAllocation pal_alloc_ = nullptr;
    VkBuffer      pal_staging_       = VK_NULL_HANDLE;
    VmaAllocation pal_staging_alloc_ = nullptr;
    void*         pal_staging_map_   = nullptr;
    bool          pal_uploaded_ = false;

    // CPU shadows for the incremental upload.
    uint8_t vram_shadow_[0x10000] = {};
    uint8_t cram_shadow_[0x80]    = {};
    bool    vram_seen_ = false;    ///< false = first upload → everything dirty
    bool    cram_seen_ = false;

    // Pipeline.
    VkRenderPass          render_pass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout      pipe_layout_ = VK_NULL_HANDLE;
    VkPipeline            pipeline_    = VK_NULL_HANDLE;
    VkDescriptorPool      desc_pool_   = VK_NULL_HANDLE;
    VkDescriptorSet       desc_set_    = VK_NULL_HANDLE;
    VkSampler             sampler_     = VK_NULL_HANDLE;   // NEAREST (texelFetch requires it anyway)
    VkFramebuffer         framebuffer_ = VK_NULL_HANDLE;
    VkExtent2D            extent_      = {};
};
