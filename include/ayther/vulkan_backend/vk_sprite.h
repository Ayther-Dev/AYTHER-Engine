#pragma once
// ---------------------------------------------------------------------------
// vk_sprite.h — Alpha-blended HD sprite overlay pipeline.  Ayther v0.8.0
//
// Renders HD sprite textures (loaded from a .ay pack) over the emulator
// framebuffer with standard src-alpha blending.
//
// ## Integration into the frame loop
//
//   // swap must be in TRANSFER_DST_OPTIMAL on entry.
//   sprite_pipeline.draw(ctx, swap, subs, n_subs, pack, emu_w, emu_h);
//   // swap is back in TRANSFER_DST_OPTIMAL on return.
//   VkPresent::finalize(ctx, swap);     // unchanged
//
// ## Lifecycle
//
//   sprite_pipeline.init(ctx, swap)          // once after swapchain creation
//   sprite_pipeline.rebuild(ctx, swap)       // on SDL_EVENT_WINDOW_RESIZED
//   sprite_pipeline.shutdown(ctx)            // before vkDeviceWaitIdle / shutdown
//
// ## Render-pass layout path (one barrier pair per draw() call)
//
//   TRANSFER_DST  →  [explicit barrier]  →  COLOR_ATTACHMENT
//                →  [render pass, loadOp=LOAD, alpha-blend subpass]
//                →  [finalLayout auto-transition]  →  TRANSFER_DST
//
//   VkPresent::finalize() then transitions TRANSFER_DST → PRESENT_SRC_KHR
//   as usual — no callers need to be changed.
// ---------------------------------------------------------------------------
#include <vulkan/vulkan.h>
#include <ayther/engine/vulkan_interop.hpp>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>


class VkTexture;
struct AyArchive;
struct AytherSpriteSub;
namespace ayther { struct AnimHdFrame; }   // ayther_animation.h (C-S2)

// ---------------------------------------------------------------------------

class VkSprite {
public:
    VkSprite() = default;
    ~VkSprite();

    VkSprite(const VkSprite&) = delete;
    VkSprite& operator=(const VkSprite&) = delete;

    // ---- Lifecycle -----------------------------------------------------------

    /// Initialise the render pass, alpha-blend pipeline, sampler, descriptor
    /// pool, and a framebuffer over `target_view` (the renderer's offscreen
    /// image, format `fmt`, size w×h). Call once after the target exists.
    bool init(const ayther::engine::VulkanContextView& ctx, VkFormat fmt, uint32_t w, uint32_t h,
              VkImageView target_view,
              const char* vert_spv_path, const char* frag_spv_path);

    /// Recreate the framebuffer after the offscreen target is resized.
    /// The pipeline and render pass are format-fixed — they survive resizes.
    void rebuild(const ayther::engine::VulkanContextView& ctx, uint32_t w, uint32_t h, VkImageView target_view);

/// Deterministic early release. The destructor performs the same idempotent
/// cleanup when the sprite owner leaves scope.
    void shutdown(const ayther::engine::VulkanContextView& ctx);

    // ---- Per-frame draw ------------------------------------------------------

    /// Render alpha-blended HD sprites over `target_image` (the offscreen).
    ///
    /// Entry state: target_image in TRANSFER_DST_OPTIMAL.
    /// Exit  state: target_image in TRANSFER_DST_OPTIMAL.
    ///
    /// cmd     — the recording command buffer (the renderer's per-frame cmd).
    /// subs    — resolved sprite substitutions from SpriteSubstitutor.
    /// pack    — open .ay archive (null → no-op).
    /// emu_w/h — native emulator resolution (320×240) for pixel scale factor.
    /// flips   — optional, parallel to `subs` (length=count): bit0 hflip, bit1 vflip.
    ///           For plane tiles (Phase 2c): a flipped instance of the pattern
    ///           uses the SAME texture flipped at load time (pre-flip, no shader).
    ///           nullptr = no flips (real sprites).
    /// slots   — C8 (optional, parallel to `subs`): SAT slot per sub. When passed,
    ///           the subs are drawn by DESCENDING slot (frontmost = lowest slot =
    ///           drawn last = on top) to respect sprite-vs-sprite occlusion between
    ///           overlapping HD ones. nullptr = ordered by asset_path (batching; for
    ///           plane tiles, which do not overlap by SAT).
    void draw(const ayther::engine::VulkanContextView& ctx, VkCommandBuffer cmd, VkImage target_image,
              const AytherSpriteSub* subs, uint32_t count,
              AyArchive* pack,
              uint32_t emu_w, uint32_t emu_h,
              const uint8_t* flips = nullptr,
              const uint8_t* tint  = nullptr,    ///< E1 chromatic: RGB tint per sub
                                                 ///< (stride 3, Q2.6: 64 = 1.0)
              const uint8_t* slots = nullptr,    ///< C8: SAT slot per sub (z-order)
              /// PER-SUB layer dimming (optional, parallel to `subs`): opacity
              /// 0-255 (255 = opaque). Only the plane tiles use it, where one
              /// call mixes subs from different layers and the global `set_dim`
              /// is not enough. nullptr = the opacity from set_dim.
              const uint8_t* alphas = nullptr,
              /// (Acetates): blend mode of the WHOLE CALL — 0 = normal alpha
              /// (the usual pipeline) · 1 = additive (src·a + dst, so a flash
              /// does not come out milky). Per call and not per sub: the unit
              /// is the layer, and blend is fixed pipeline state.
              uint8_t blend = 0);

    /// C-S2 animations: draws HD frames from a SHEET in phase — like draw()
    /// but each quad samples the `src_*` SUB-RECT (UV) of the sheet and the
    /// destination is a FLOAT rect (sub-pixel, for the Level 1 geometric
    /// tween). Same pipeline (the sub-rect travels in push constants); same
    /// entry/exit states as draw().
    void draw_anim(const ayther::engine::VulkanContextView& ctx, VkCommandBuffer cmd, VkImage target_image,
                   const ayther::AnimHdFrame* frames, uint32_t count,
                   AyArchive* pack,
                   uint32_t emu_w, uint32_t emu_h);


    /// The current VIDEO frame of the Kinematic, full screen and OPAQUE (it
    /// covers 100%: it replaces the screen, it does not compose over it).
    ///
    /// It has its OWN texture and descriptor, and that is not gratuitous
    /// duplication:
    ///  · It cannot go through `draw()` — that resolves the texture by
    ///    `asset_path` against the cache, where the entry falls into the
    ///    deferred staging release and after a few frames stops accepting
    ///    uploads SILENTLY. The video would freeze on its first frame.
    ///  · It has its OWN resources on purpose. The compose had its own, and
    ///    sharing them with a video of different dimensions would have fired
    ///    `shutdown+delete` twice per frame on a texture referenced by command
    ///    buffers in flight. The compose is gone; the criterion still holds for
    ///    the next one that wants to reuse them.
    ///
    /// `seq` is the session's content counter: if it has not changed since the
    /// last draw, the re-upload is skipped. Without it, the full memcpy is paid
    /// for every interface frame while the video is paused.
    ///
    /// LINEAR sampling (not NEAREST like the overlay): the canvas is the
    /// display height × 4/3 —on a 4K display that is 2880×2160— so the video is
    /// almost always MAGNIFIED, and with NEAREST it would come out blocky. It
    /// is the first texture in the motor that is neither pixel art nor an HD
    /// master.
    /// It receives the THREE PLANES from the decoder (I420), not a BGRA. The
    /// conversion is done by video.frag. `y_stride` and friends are NOT `w`:
    /// libvpx aligns the rows.
    void draw_video(const ayther::engine::VulkanContextView& ctx, VkCommandBuffer cmd, VkImage target_image,
                    const uint8_t* y, uint32_t y_stride,
                    const uint8_t* u, uint32_t u_stride,
                    const uint8_t* v, uint32_t v_stride,
                    uint32_t w, uint32_t h, uint64_t seq);

    bool is_ready() const { return pipeline_ != VK_NULL_HANDLE; }

    /// Free all cached sprite textures and reset the descriptor pool,
    /// without destroying the pipeline or framebuffers.
    ///
    /// Call this during a pack hotreload (after vkDeviceWaitIdle) to evict
    /// all loaded textures so they are re-fetched from the new pack.
    /// The pipeline stays alive — no need to re-call init().
    void clear_textures(const ayther::engine::VulkanContextView& ctx);

    /// Evict ONE asset (`path` and its flipped variants `path#N`) from the
    /// cache so the next draw reloads it from the pack/disk. For assets
    /// REWRITTEN live (the snapshot of a pose is regenerated on every edit of
    /// the rig in Pose) — without this the sub would keep drawing the old
    /// texture stretched to the new bbox. It waits for GPU idle only if a
    /// texture was loaded.
    void evict(const ayther::engine::VulkanContextView& ctx, const std::string& path);

    /// Phase 2: PRE-WARMS an asset — reads the bytes and queues the decode on
    /// the worker, without blocking. Call it when assigning/feeding poses: by
    /// the time the asset appears on screen the texture is already decoded (the
    /// upload is done by pump_uploads) and there is no decode spike on the
    /// render thread. Only LOOSE assets from disk (pack == nullptr, and if the
    /// path does not exist → no-op, so pack-relative names are not
    /// negative-cached).
    /// `mask` = Wardrobe: the SAME path may be assigned both as an asset and as
    /// a mask — the mask is cached under its own key (`path#m…`) and decoded as
    /// R8 (1 channel).
    void prewarm(const std::string& path, AyArchive* pack, uint8_t flip = 0,
                 bool mask = false);

    /// Authoring HOT-RELOAD (2026-07-24): stats the assets loaded FROM DISK and
    /// evicts the ones whose mtime changed — or that APPEARED (the
    /// negative-cache of a PNG that did not exist is lifted once the file is
    /// created). The next draw reloads them. Call at a low cadence (~1×/s):
    /// with no changes it is just one stat per asset; with changes it pays for
    /// the evict (GPU idle once). PACK assets are not watched (the pack is
    /// immutable until its own hot-reload).
    void poll_disk(const ayther::engine::VulkanContextView& ctx);

    /// Phase 2: uploads the finished decodes to the GPU (at most
    /// `max_uploads` per frame, which bounds the hitch). Call it ONCE per frame
    /// with the cmd recording, OUTSIDE a render pass — even if there are no
    /// subs this frame.
    void pump_uploads(const ayther::engine::VulkanContextView& ctx, VkCommandBuffer cmd, int max_uploads = 2);

    /// R-8: the texture state of an asset WITHOUT triggering its load. The key
    /// mirrors the one draw() uses (path + flip suffix). Read-only — so the
    /// checker mode can tell "authored but the asset did NOT load"
    /// (negative-cache) from a healthy asset or one still in flight.
    enum class TexState { NotRequested, Pending, Ready, Failed };
    TexState texture_state(const std::string& path, uint8_t flip = 0) const;

    /// DIMMING of the HD quads: multiplies the tint (`d`) and the opacity
    /// (`a`) of `draw`/`draw_anim` until the next change. The compositor moves
    /// it layer by layer — the dim of the focused layer has to reach the HD,
    /// which is exactly what is being authored. 1.0, 1.0 = no dimming.
    /// Opacity is kept apart from the tint because darkening alone does NOT let
    /// you see through: the dimmed HD still covered the focused layer behind
    /// it.
    /// Phase 0: X displacement in EMU SPACE pixels for widescreen centring. It
    /// is STATE and not a parameter, by the same criterion as `set_dim`: the
    /// subs come in native coordinates (0..320) and the canvas may be wider, so
    /// the shift belongs to the whole PASS and not to each quad. 0 (the
    /// default) does not change a single pixel.
    void set_wide_offset(float off_x_emu) { wide_off_x_ = off_x_emu; }

    void set_dim(float d, float a = 1.0f) noexcept { dim_ = d; dim_a_ = a; }

private:
    const ayther::engine::VulkanContextView* context_ = nullptr;
    // ---- Vulkan objects ----
    VkRenderPass          render_pass_  = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_layout_  = VK_NULL_HANDLE;
    VkPipelineLayout      pipe_layout_  = VK_NULL_HANDLE;
    float                 wide_off_x_   = 0.0f;   ///< phase 0 (set_wide_offset)
    float                 dim_          = 1.0f;   ///< layer dimming: tint (set_dim)
    float                 dim_a_        = 1.0f;   ///< layer dimming: opacity (set_dim)
    VkPipeline            pipeline_     = VK_NULL_HANDLE;
    /// ADDITIVE variant of the sprite pipeline (src·a + dst, same layout and
    /// same shaders — only the blend state changes). The Acetates select it
    /// with the `blend` argument of draw(). NULL = init failed to create it
    /// (the additive draw falls back to normal alpha, with a reason in the
    /// log).
    VkPipeline            pipeline_add_ = VK_NULL_HANDLE;
    /// MULTIPLY and SCREEN variants of the sprite pipeline — same layout and
    /// same vert, their OWN frag (sprite_mult/sprite_screen: the mix towards
    /// white/black by strength cannot be expressed with fixed factors without
    /// premultiplying) + fixed COLOR factors:
    ///   mult   = (DST_COLOR, ZERO)             → out = dst · mix(1, c·tint, a·op)
    ///   screen = (ONE_MINUS_DST_COLOR, ONE)    → out = dst + (c·tint·a·op)·(1−dst)
    /// The Acetates select them with blend=2/3. NULL = their .spv was missing
    /// at init — that mode falls back to normal alpha, with a reason in the log
    /// (the same degradation as the video and the Wardrobe).
    VkPipeline            pipeline_mult_   = VK_NULL_HANDLE;
    VkPipeline            pipeline_screen_ = VK_NULL_HANDLE;
    /// Subtractive, darken and lighten — they REUSE the frags above, only the
    /// VkBlendOp changes (which is why there are no new .spv to stage):
    ///   sub (4) = screen frag + (ONE, ONE, REVERSE_SUBTRACT) → dst − c·f
    ///   min (5) = mult   frag + op MIN → min(dst, mix(1, c·tint, f))
    ///   max (6) = screen frag + op MAX → max(dst, c·tint·f)
    VkPipeline            pipeline_sub_ = VK_NULL_HANDLE;
    VkPipeline            pipeline_min_ = VK_NULL_HANDLE;
    VkPipeline            pipeline_max_ = VK_NULL_HANDLE;
    VkSampler             sampler_      = VK_NULL_HANDLE;
    /// CHAIN of descriptor pools (it grows on demand): the single pool of
    /// 128 assets ×2 sets ran out on real projects (Golden Axe: 65 assets ×2
    /// facings = 260 sets) and the textures that lost the load-order lottery
    /// stayed negative-cached for the WHOLE session — poses invisible in their
    /// frame windows (Tyris report f515-536/f559-568, 2026-07-16).
    std::vector<VkDescriptorPool> desc_pools_;

    // ---- Foreground compose (Edit) — full-screen BGRA buffer, NEAREST ------
    VkSampler        sampler_nn_   = VK_NULL_HANDLE;   // point sampler (crisp mask)

    // WARDROBE — a 2-sampler pipeline (binding 0 = asset · 1 = R8 mask) with
    // sprite_mask.frag: only the quads WITH a mask use it; the classic pipeline
    // and sprite.frag stay untouched (byte-exact GPU oracles). NULL = the .spv
    // was not there at init — the masked quads fall back to the classic
    // pipeline (whole-sprite tint), with a reason in the log.
    VkDescriptorSetLayout mask_layout_      = VK_NULL_HANDLE;
    VkPipelineLayout      mask_pipe_layout_ = VK_NULL_HANDLE;
    VkPipeline            mask_pipeline_    = VK_NULL_HANDLE;
    /// OWN pools for the 2-sampler sets (the `desc_pools_` chain allocates with
    /// `desc_layout_`, which has 1). It grows on demand like that one.
    std::vector<VkDescriptorPool> mask_pools_;
    /// Combined set (asset + mask) per texture pair and filter. It stores the
    /// seq of BOTH entries: an evict/hot-reload changes the seq and the set is
    /// rewritten before the next bind (evict already waited for GPU idle).
    struct MaskSet {
        VkDescriptorSet ds        = VK_NULL_HANDLE;
        uint32_t        asset_seq = 0;
        uint32_t        mask_seq  = 0;
    };
    std::unordered_map<std::string, MaskSet> mask_set_cache_;

    // VIDEO — OWN resources, see draw_video. A separate pool for the same
    // reason as the overlay one: clear_textures resets `pool_` on a pack
    // hot-reload and would take out the video set mid-playback.
    VkDescriptorPool video_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet  video_ds_   = VK_NULL_HANDLE;
    /// Three R8 textures (luma + the two half-size chromas) instead of one
    /// BGRA8. That uploads 1.5 bytes per pixel instead of 4.
    std::unique_ptr<VkTexture> video_tex_[3];
    /// The video's own pipeline: same render pass and same push constant as the
    /// sprite one, a different fragment shader and three bindings.
    VkDescriptorSetLayout video_layout_      = VK_NULL_HANDLE;
    VkPipelineLayout      video_pipe_layout_ = VK_NULL_HANDLE;
    VkPipeline            video_pipeline_    = VK_NULL_HANDLE;
    uint32_t         video_w_ = 0, video_h_ = 0;
    uint64_t         video_seq_ = 0;   // last `seq` uploaded (0 = nothing yet)

    std::vector<VkFramebuffer> framebuffers_;
    uint32_t                   fb_w_ = 0, fb_h_ = 0;

    // ---- Sprite texture cache ----
    // Two descriptor sets per texture: NEAREST (magnified pixel art = crisp)
    // and LINEAR (MINIFIED HD art — a 1296² master drawn at ~72px with NEAREST
    // looks pixelated). The draw picks per quad according to the scaling
    // direction.
    struct TexEntry {
        std::unique_ptr<VkTexture> tex;
        VkDescriptorSet desc_set     = VK_NULL_HANDLE;   // sampler NEAREST
        VkDescriptorSet desc_set_lin = VK_NULL_HANDLE;   // sampler LINEAR (minificación)
        bool            valid    = false;
        bool            stale    = false;    // evict(): reload on the next ensure_loaded
        bool            pending  = false;    // async decode in flight
        uint32_t        seq      = 0;        // invalidates stale results (evict)
        /// Hot-reload: mtime of the DISK file when its bytes were read (ticks
        /// of fs::file_time_type). -1 = it came from the pack / do not watch;
        /// -2 = the file did NOT exist at load time (watch for its
        /// appearance).
        int64_t         disk_mtime = -1;
    };
    std::unordered_map<std::string, TexEntry> tex_cache_;

    // ---- Deferred staging release ------------------------------------------
    // Asset textures upload ONCE, but VkTexture retains its host-visible
    // staging for life (up to ~10 MB per master → tens of MB per session). It
    // cannot be released in finish_upload: the copy has only just been RECORDED
    // into this frame's cmd. It is released kStagingLingerPumps pumps later
    // (> frames-in-flight ⇒ the fence of the upload frame has already
    // signalled). The entry stores key+seq: if there was an evict/reload the
    // seq does not match and it is skipped (the NEW texture still needs its
    // staging).
    struct StagingRelease { std::string key; uint32_t seq = 0; uint64_t due = 0; };
    static constexpr uint64_t kStagingLingerPumps = 4;   // frames-in-flight (2) + margin
    std::vector<StagingRelease> staging_release_;
    uint64_t                    pump_tick_ = 0;
    size_t                      staging_freed_total_ = 0;   // observability (log)
    /// Measured cost of the complete upload: the budget of 1 per frame was set
    /// assuming 4-5 ms for a large master, and that has to be verified against
    /// the real path before deciding whether one video frame per game frame
    /// fits. Observability only — it changes no behaviour.
    /// Cost breakdown: image+mips init · pixel upload · descriptors.
    /// Accumulated; the log prints the split so we know which of the three
    /// optimisations in the issue is worth writing before writing it.
    double                      up_init_ms_ = 0.0;
    double                      up_copy_ms_ = 0.0;
    double                      up_desc_ms_ = 0.0;
    /// Fine-grained init breakdown, copied from VkTexture::last_init_cost()
    /// per asset. It accumulates HERE and not in the texture .cpp because
    /// TileTexCache also calls init(): shared accumulators divided by the asset
    /// counter made the parts sum to more than the whole.
    double                      up_img_ms_  = 0.0;
    double                      up_view_ms_ = 0.0;
    double                      up_stg_ms_  = 0.0;
    double                      up_log_ms_  = 0.0;
    double                      upload_ms_max_   = 0.0;
    double                      upload_ms_total_ = 0.0;
    double                      upload_mb_total_ = 0.0;
    int                         upload_count_    = 0;

    // ---- Async decode (phase 2) --------------------------------------------
    // stbi on a 1440×1296 master takes TENS of ms: done on the render thread it
    // was a spike > frame budget on the FIRST appearance of every asset (and of
    // every flipped variant) → the audio backlog (~46 ms) drained → crackle
    // ("sound degradation with HD", phase 2). The worker does stbi + BGRA swap
    // + pre-flip; the render thread only reads bytes, queues and uploads
    // (pump_uploads, bounded). Single worker: FIFO; the TexEntry states are
    // touched ONLY on the render thread.
    struct DecodeJob  { std::string key; std::string path; uint8_t flip = 0;
                        bool r8 = false;   ///< Wardrobe: decode to 1 channel (mask)
                        uint32_t gen = 0, seq = 0; std::vector<uint8_t> raw; };
    struct DecodeDone { std::string key; std::string path;
                        bool r8 = false;
                        uint32_t gen = 0, seq = 0;
                        int w = 0, h = 0; std::vector<uint8_t> bgra; };
    std::thread             decode_worker_;
    std::mutex              decode_mx_;
    std::condition_variable decode_cv_;
    std::deque<DecodeJob>   decode_jobs_;
    std::vector<DecodeDone> decode_done_;
    bool                    decode_quit_ = false;
    uint32_t                decode_gen_  = 0;   // clear_textures() invalidates in-flight work

    void decode_loop();
    /// Reads the bytes (pack/disk — here, so AyArchive is never touched from
    /// the worker) and queues the decode. Marks the entry pending.
    void enqueue_decode(const std::string& key, const std::string& path,
                        AyArchive* pack, uint8_t flip, bool mask_r8 = false);
    /// Texture + descriptors from already-decoded BGRA (the GPU half of the old
    /// synchronous ensure_loaded). true = entry.valid.
    /// `r8` = Wardrobe: the texture uploads as R8/Gray8 and does NOT allocate
    /// the 1-sampler descriptor sets (a mask is never drawn on its own — it
    /// lives in the combined sets of `mask_set_cache_`).
    bool finish_upload(const ayther::engine::VulkanContextView& ctx, VkCommandBuffer cmd, TexEntry& entry,
                       const std::string& path, int w, int h, const uint8_t* bgra,
                       bool r8 = false);

    // ---- Helpers ----
    bool create_render_pass  (const ayther::engine::VulkanContextView& ctx, VkFormat fmt);
    /// Parameterised by SAMPLER count and with an explicit output, because
    /// there are now two pipelines: the sprite one (1 sampler, sprite.frag) and
    /// the video one (3 — luma and the two chromas —, video.frag). Everything
    /// else —render pass, blending, push constant, dynamic state— is identical,
    /// so duplicating the function would have meant duplicating 150 lines to
    /// change one integer and one path.
    /// `out_pipeline_add` (optional) ALSO creates the additive variant of the
    /// same pipeline (same layout/shaders, blend src·a + dst) — for the
    /// Acetates. nullptr = the alpha pipeline only (video, compose).
    /// `variants` (optional) creates variants with their OWN FRAG and fixed
    /// COLOR blend factors (multiply, screen), reusing layout/vert/state. The
    /// degradation is PER variant: if its .spv is missing or creation fails,
    /// its `out` stays NULL (the draw falls back to normal alpha) and
    /// create_pipeline does NOT fail because of it.
    /// `color_op` completes the expressiveness — REVERSE_SUBTRACT (subtract),
    /// MIN (darken) and MAX (lighten) come out of the SAME pair of frags by
    /// changing only the op (MIN/MAX ignore the factors).
    struct PipelineBlendVariant {
        const char*   frag_spv;    ///< the variant's own .frag.spv
        VkBlendFactor src_color;
        VkBlendFactor dst_color;
        VkBlendOp     color_op;
        VkPipeline*   out;
    };
    bool create_pipeline     (const ayther::engine::VulkanContextView& ctx,
                              const char* vert_spv_path,
                              const char* frag_spv_path,
                              uint32_t    sampler_count,
                              VkDescriptorSetLayout* out_layout,
                              VkPipelineLayout*      out_pipe_layout,
                              VkPipeline*            out_pipeline,
                              VkPipeline*            out_pipeline_add = nullptr,
                              const PipelineBlendVariant* variants = nullptr,
                              uint32_t variant_count = 0);
    bool create_sampler      (const ayther::engine::VulkanContextView& ctx);
    bool create_desc_pool    (const ayther::engine::VulkanContextView& ctx);
    /// Allocates a set from the LAST pool in the chain; if it is exhausted, it
    /// creates a new pool and retries. VK_NULL_HANDLE only on a real driver
    /// failure.
    VkDescriptorSet alloc_desc_set(const ayther::engine::VulkanContextView& ctx);
    bool create_framebuffer  (const ayther::engine::VulkanContextView& ctx, VkImageView view, uint32_t w, uint32_t h);
    void destroy_framebuffers(const ayther::engine::VulkanContextView& ctx);

    /// Ensure the sprite texture for `path` is loaded into the GPU and its
    /// descriptor set is allocated.  Must be called OUTSIDE a render pass.
    /// `flip` (bit0 hflip, bit1 vflip) → flipped variant cached separately (the
    /// texture is flipped while decoding; the cache key carries the flip
    /// suffix).
    TexEntry* ensure_loaded(const ayther::engine::VulkanContextView& ctx, VkCommandBuffer cmd,
                            const std::string& path, AyArchive* pack,
                            uint8_t flip = 0, bool mask = false);

    /// Cache key of a texture: `path` · `path#N` (flipped variant) ·
    /// `path#m`/`path#mN` (Wardrobe — the same PNG may be assigned both as an
    /// asset AND as a mask, and the mask is additionally decoded as R8).
    static std::string tex_key(const std::string& path, uint8_t flip, bool mask) {
        if (!mask)
            return (flip & 3) ? path + "#" + static_cast<char>('0' + (flip & 3))
                              : path;
        std::string k = path + "#m";
        if (flip & 3) k += static_cast<char>('0' + (flip & 3));
        return k;
    }

    /// Combined asset+mask set (Wardrobe): allocated from `mask_pools_` and
    /// (re)written when either of the two entries changed seq. nullptr = one of
    /// the textures is not ready yet (the caller falls back to the classic
    /// pipeline).
    VkDescriptorSet get_mask_set(const ayther::engine::VulkanContextView& ctx, const std::string& asset_key,
                                 const std::string& mask_key, TexEntry& asset,
                                 TexEntry& mask, bool linear);
    VkDescriptorSet alloc_mask_set(const ayther::engine::VulkanContextView& ctx);
    bool            create_mask_pool(const ayther::engine::VulkanContextView& ctx);
};
