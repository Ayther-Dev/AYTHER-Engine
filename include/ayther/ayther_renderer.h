#pragma once
// ---------------------------------------------------------------------------
// AytherRenderer — the motor's visual (HD) layer (R3).
//
// Consumes a FrameView (the deterministic CPU output of AytherSession::step())
// + a borrowed VkContext, and renders the HD frame into an offscreen VkImage
// (VkRenderTarget). The frontend then presents that image:
//   - ayther_play blits it to its swapchain;
//   - ayther_lab samples it in an ImGui viewport.
//
// Kept SEPARATE from AytherSession on purpose: the session stays Vulkan-free /
// headless (CI, determinism, future mobile); the renderer is the swappable GPU
// layer. See docs/CPP_API_REFERENCE.md#rendering-and-vulkan.
//
// Lifecycle: init(ctx, w, h) → render(ctx, cmd, fv) per frame → shutdown(ctx).
// Single-owner; driven from the same thread as the session.
//
// R3.0: scaffold — owns the offscreen target; render() clears it. The emu-frame,
// HD-tile, sprite and post-process passes land in R3.1 / R3.2.
// ---------------------------------------------------------------------------
#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>

#include "vulkan_backend/vk_render_target.h"
#include "runtime_options.h"
#include "vulkan_backend/vk_texture.h"        // emu-frame texture
#include "vulkan_backend/tile_tex_cache.h"    // HD tile textures
#include "vulkan_backend/vk_sprite.h"         // HD sprite overlay (R3.2)
#include "vulkan_backend/vk_indexed_plane.h"  // R-5: indexed compose

#include <string>

struct AyArchive;   // opaque (ayther_core_ffi.h) — HD asset source
class  VkContext;
class  AytherLayerStack;   // R-4 — layer model (ayther_layers.h)

namespace ayther {

struct FrameView;   // ayther_session.h (full definition needed only in the .cpp)
struct SceneElement;   // ayther_session.h — R-8: sub_texture_state()

/// @brief Vulkan presentation layer for one session frame stream.
///
/// The renderer borrows `VkContext`; the context and its device must outlive
/// every renderer resource. All methods are render-thread affine and are not
/// safe for concurrent use. Destruction releases initialized resources;
/// shutdown() remains available for deterministic release before the context.
class AytherRenderer {
public:
    AytherRenderer();
    ~AytherRenderer();

    AytherRenderer(const AytherRenderer&)            = delete;
    AytherRenderer& operator=(const AytherRenderer&) = delete;

    // Create the offscreen target + the render pipelines at the canvas
    // resolution. ayther_play passes its swapchain extent; ayther_lab its
    // viewport size. `shader_dir` locates the SPIR-V (sprite.*.spv); if the
    // shaders are missing the sprite overlay is disabled (emu+tiles still work).
    bool init(VkContext& ctx, uint32_t canvas_w, uint32_t canvas_h,
              const char* shader_dir,
              const RuntimeOptions& options = RuntimeOptions::process());

    // Recreate at a new canvas size (window / viewport resize).
    bool resize(VkContext& ctx, uint32_t canvas_w, uint32_t canvas_h);

    // Evict cached HD tile textures so the next frame re-fetches from the new
    // pack. Call on pack hot-reload, after vkDeviceWaitIdle().
    void evict_pack_textures(VkContext& ctx);

    // Evict ONE HD sprite texture by asset path (including its flipped
    // variants) so the next frame reloads it from the pack/disk. For assets
    // rewritten live — a pose snapshot is regenerated on every edit of the rig
    // in Pose, and the per-path cache used to leave it stale.
    // evict_pack_textures throws EVERYTHING away; this is surgical (it waits
    // for GPU idle only if the texture was loaded).
    void evict_sprite_texture(VkContext& ctx, const std::string& path);

    /// Phase 2: pre-warms the texture of a LOOSE asset from disk (async
    /// decode) — call it when assigning/feeding poses so the first appearance
    /// does not pay for the decode (a spike > frame budget → audio crackle).
    /// flip = flipped variant (bit0 h, bit1 v). No-op if the path does not
    /// exist.
    void prewarm_sprite(const std::string& path, uint8_t flip = 0);

    /// Wardrobe: pre-warms the tint MASK of a pose (R8 decode under its own
    /// key `path#m…` — the same PNG may be assigned both as an asset and as a
    /// mask). Same policy as prewarm_sprite.
    void prewarm_sprite_mask(const std::string& path, uint8_t flip = 0);

    /// Authoring HOT-RELOAD (2026-07-24): stats the HD assets loaded FROM
    /// DISK and evicts the ones that changed or appeared — the next frame
    /// reloads them (images edited in graphics/ outside the Lab used to look
    /// stale until a restart). Call at a low cadence (~1×/s).
    void poll_disk_sprite_textures(VkContext& ctx);

    // Destroy all resources. Safe on a partially-initialized renderer.
    void shutdown(VkContext& ctx);

    bool is_ready() const { return target_.is_ready(); }

    // Useful region of the last uploaded frame (the fb changes video mode).
    uint32_t emu_frame_w() const { return emu_w_; }
    uint32_t emu_frame_h() const { return emu_h_; }

    /// Readback (MP4 export): records into `cmd` the copy of the offscreen
    /// (left in SHADER_READ_ONLY after render()) to a host-visible buffer —
    /// barrier to TRANSFER_SRC + vkCmdCopyImageToBuffer (BGRA tightly packed,
    /// the target extent()) + barrier back. `dst` must hold extent().w*h*4
    /// bytes.
    void copy_target_to_buffer(VkCommandBuffer cmd, VkBuffer dst);

    // ---- CPU frame export (MP4) --------------------------------------------
    // OWN readback resources (a TRANSIENT cmd pool + fence + host-visible VMA
    // buffer with persistent mapping — VMA is private to the engine, which is
    // why they live here and not in the Lab). Export is not tied to present:
    // each export_frame renders `fv` to the offscreen (which must ALREADY be
    // resized to the destination resolution via resize()), copies to the
    // buffer, submits with a fence and WAITS. A single reused buffer
    // (8K ≈ 95 MB host).
    /// Creates the resources for frames of the current extent(). false = no
    /// resources.
    bool readback_init(VkContext& ctx);
    /// Render + copy + submit + wait. Returns the mapped BGRA (w*h*4 of the
    /// target extent(); valid until the next export_frame) or nullptr.
    /// `vdp_mask` = the same layer eyes as the render (AYTHER_LAYER_* bits:
    /// A=1 B=2 W=4 OBJ=8). The Lab Snapshot lets the user choose which layers
    /// enter the image; 0xFF = all of them, which is what the MP4 export and
    /// the snapshot did before the parameter existed.
    const uint8_t* export_frame(VkContext& ctx, const FrameView& fv,
                                AyArchive* pack, bool hd_on,
                                const AytherLayerStack* layers = nullptr,
                                uint8_t vdp_mask = 0xFF);
    void readback_shutdown(VkContext& ctx);

    // Record the HD render for `fv` into `cmd`, targeting the offscreen image.
    // Uploads the emu frame (always) then, when hd_on=true, overlays the resolved
    // HD tile substitutions and HD sprite pass. hd_on=false gives the raw emulator
    // look (M4 Original mode). `pack` provides HD asset bytes; null → emu only.
    //
    // R-4: the lanes are dispatched from the LAYER LIST — explicit order,
    // per-layer visibility, insertable Custom layers (a no-op until R-7).
    // `layers = nullptr` uses the default stack (== the historical hard-wired
    // order: the smokes and the export do not change). The stack's VDP layers
    // are NOT drawn here: their visibility travels through the session mask
    // (AytherLayerStack::vdp_mask, routed by the frontend).
    // R-5 part 2b: `vdp_mask` = the WORKSPACE eyes (Edit/Paint,
    // AYTHER_LAYER_* bits: A=1 B=2 W=4 OBJ=8) — the former 0x102 channel, now a
    // per-element filter of the compose (ANDed with the stack visibility). On
    // frames that fall back to the blit it does not apply (the blit brings the
    // whole frame).
    void render(VkContext& ctx, VkCommandBuffer cmd, const FrameView& fv,
                AyArchive* pack, bool hd_on = true,
                const AytherLayerStack* layers = nullptr,
                uint8_t vdp_mask = 0xFF);

    // R-8: UV checker mode — an authoring diagnostic view. Every element
    // WITHOUT an asset is painted with a checker anchored to the element, using
    // the colour pair of ITS LAYER (pink+black = authored but failed to load);
    // the rest is dimmed (the HD ones draw normally on top). It only affects
    // the scene passes; on frames that fall back to the blit it does not
    // apply.
    void set_checker(bool on) noexcept { checker_ = on; }
    bool checker() const noexcept { return checker_; }

    // FOCUSED layer: the one being authored is composed at full intensity and
    // the rest is dimmed, so the working focus reads at a glance.
    // Scene layer index (0=Plane B · 1=Plane A · 2=Window · 3=Sprites);
    // -1 = no focus (everything at full intensity).
    //
    // It lives here and not in the fork's 0x108 channel because the dim is a
    // COMPOSITION decision, and composition has been ours since R-5: that
    // channel is boolean —it dims "the non-sprite"— and knows nothing about a
    // focused layer.
    void set_focus_layer(int layer) noexcept { focus_layer_ = layer; }
    int  focus_layer() const noexcept { return focus_layer_; }

    /// R-8: load state of an element's SUB asset (for the Lab coverage
    /// report). NotRequested/Pending/Ready/Failed; elements without a sub
    /// return NotRequested.
    VkSprite::TexState sub_texture_state(const FrameView& fv,
                                         const SceneElement& e) const;

    // ---- The offscreen result — the frontend presents or samples it --------
    VkImage     framebuffer_image()   const { return target_.image();   }
    VkImageView framebuffer_view()    const { return target_.view();    }
    VkSampler   framebuffer_sampler() const { return target_.sampler(); }
    VkExtent2D  framebuffer_extent()  const { return target_.extent();  }

    // ---- COMPARISON image (A/B preview) ------------------------------------
    // A second offscreen holding a COPY of the already-rendered frame, so two
    // versions of the same frame can be shown at once (split Original |
    // AYTHER). It exists because the renderer has a single `target_`: without a
    // copy, drawing both versions would require rendering twice per frame.
    //
    // The intended use is WHILE PAUSED — the authoring gesture is comparing ONE
    // frame: render the original version, capture it here, render the HD one
    // and draw both. Live it would work the same, but the cost becomes
    // per-frame and that decision is not the renderer's to make.
    //
    // It is not created until somebody asks for it (capture_compare): a Lab
    // that never opens the A/B does not pay the memory of a second canvas,
    // which at 8K is not small.
    bool        compare_ready() const { return compare_.is_ready(); }
    /// The IMAGE, for the runtime split — which composes with per-region blits
    /// rather than sampling, so it needs the handle and not the view.
    VkImage     compare_image()   const { return compare_.image();   }
    VkImageView compare_view()    const { return compare_.view();    }
    VkSampler   compare_sampler() const { return compare_.sampler(); }
    VkExtent2D  compare_extent()  const { return compare_.extent();  }

    /// Copies the CURRENT offscreen (exactly as render() left it) into the
    /// comparison image. Creates/resizes the image if needed. Record it before
    /// re-rendering the same frame with the other configuration.
    /// false = the image could not be created (out of memory) — the caller
    /// degrades to showing a single version.
    bool capture_compare(VkContext& ctx, VkCommandBuffer cmd);
    /// Releases the comparison image (leaving the A/B returns the memory).
    void compare_release(VkContext& ctx);

    /// Reads the comparison image back to the CPU — BGRA of the target extent,
    /// using the same readback resources as `export_frame` (requires
    /// readback_init). It exists for the A/B ORACLE: what makes the comparison
    /// honest is that the copy contains exactly what the offscreen held at
    /// capture time, and that can only be asserted by reading it. nullptr if
    /// there is no copy or the submit fails.
    const uint8_t* readback_compare(VkContext& ctx);

    /// Capture + its own submit, so capture_compare can be used outside a
    /// frame (oracles). In the Lab the capture travels in the cmd of the frame
    /// in progress.
    bool capture_compare_now(VkContext& ctx);

private:
    /// Injected at init(); held by value so the renderer never outlives a
    /// reference to somebody else's options.
    RuntimeOptions options_;

    struct FrameScratch;

    // Genesis Mode 5 max framebuffer — the native canvas the tile grid maps onto.
    static constexpr uint32_t kEmuW = 320;
    static constexpr uint32_t kEmuH = 240;

    VkRenderTarget target_;       // offscreen HD frame
    VkRenderTarget compare_;      // copy for the A/B (lazy, see above)
    // ---- MP4 export readback (readback_init/export_frame) ------------------
    // VmaAllocation forward-declared as in vk_texture.h (VMA is private).
    VkCommandPool   rb_pool_  = VK_NULL_HANDLE;
    VkCommandBuffer rb_cmd_   = VK_NULL_HANDLE;
    VkFence         rb_fence_ = VK_NULL_HANDLE;
    VkBuffer        rb_buf_   = VK_NULL_HANDLE;
    VmaAllocation   rb_alloc_ = VK_NULL_HANDLE;
    void*           rb_map_   = nullptr;
    VkTexture      emu_tex_;
    uint32_t  emu_w_ = 0, emu_h_ = 0;   // dims of the last uploaded frame   // emulator software framebuffer → GPU
    TileTexCache   tile_cache_;   // HD tile textures from the pack
    VkSprite       sprite_;       // HD sprite overlay pass (into the offscreen)
    bool           sprite_ok_ = false;  // false if the SPIR-V shaders are absent
    // R-5: the indexed pipeline (VRAM+CRAM on the GPU) used to compose the
    // scene from the inventory when fv.scene is published — no blit.
    VkIndexedPlane indexed_;
    bool           indexed_ok_ = false;
    bool           checker_    = false;   // R-8: UV checker mode
    int            focus_layer_ = -1;     // focused layer (-1 = none)
    // Capacity-retaining temporary storage belongs to this renderer instance.
    // Keeping it behind an implementation object avoids exposing render-only
    // element types in the public header while preserving allocation reuse.
    std::unique_ptr<FrameScratch> scratch_;
    VkContext* context_ = nullptr;
};

}  // namespace ayther
