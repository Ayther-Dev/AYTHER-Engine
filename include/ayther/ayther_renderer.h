#pragma once
// ---------------------------------------------------------------------------
// AytherRenderer — the motor's visual (HD) layer (R3).
//
// Consumes a FrameView (the deterministic CPU output of AytherSession::step())
// + a borrowed ayther::engine::VulkanContextView, and renders the HD frame into
// an offscreen VkImage
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
#include <string>

#include <ayther/engine/pack.hpp>
#include <ayther/engine/vulkan_interop.hpp>

class  AytherLayerStack;   // R-4 — layer model (ayther_layers.h)

namespace ayther {

struct FrameView;   // ayther_session.h (full definition needed only in the .cpp)
struct SceneElement;   // ayther_session.h — R-8: sub_texture_state()

/// @brief Vulkan presentation layer for one session frame stream.
///
/// The renderer borrows the handles in
/// `ayther::engine::VulkanContextView`; the host context and device must outlive
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
    bool init(const ayther::engine::VulkanContextView& ctx,
              uint32_t canvas_w, uint32_t canvas_h,
              const char* shader_dir);

    // Recreate at a new canvas size (window / viewport resize).
    bool resize(const ayther::engine::VulkanContextView& ctx,
                uint32_t canvas_w, uint32_t canvas_h);

    // Evict cached HD tile textures so the next frame re-fetches from the new
    // pack. Call on pack hot-reload, after vkDeviceWaitIdle().
    void evict_pack_textures(const ayther::engine::VulkanContextView& ctx);

    // Evict ONE HD sprite texture by asset path (including its flipped
    // variants) so the next frame reloads it from the pack/disk. For assets
    // rewritten live — a pose snapshot is regenerated on every edit of the rig
    // in Pose, and the per-path cache used to leave it stale.
    // evict_pack_textures throws EVERYTHING away; this is surgical (it waits
    // for GPU idle only if the texture was loaded).
    void evict_sprite_texture(const ayther::engine::VulkanContextView& ctx,
                              const std::string& path);

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
    void poll_disk_sprite_textures(const ayther::engine::VulkanContextView& ctx);

    // Destroy all resources. Safe on a partially-initialized renderer.
    void shutdown(const ayther::engine::VulkanContextView& ctx);

    [[nodiscard]] bool is_ready() const;

    // Useful region of the last uploaded frame (the fb changes video mode).
    [[nodiscard]] uint32_t emu_frame_w() const;
    [[nodiscard]] uint32_t emu_frame_h() const;

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
    bool readback_init(const ayther::engine::VulkanContextView& ctx);
    /// Render + copy + submit + wait. Returns the mapped BGRA (w*h*4 of the
    /// target extent(); valid until the next export_frame) or nullptr.
    /// `vdp_mask` = the same layer eyes as the render (AYTHER_LAYER_* bits:
    /// A=1 B=2 W=4 OBJ=8). The Lab Snapshot lets the user choose which layers
    /// enter the image; 0xFF = all of them, which is what the MP4 export and
    /// the snapshot did before the parameter existed.
    const uint8_t* export_frame(
        const ayther::engine::VulkanContextView& ctx, const FrameView& fv,
        ayther::engine::PackView pack, bool hd_on,
        const AytherLayerStack* layers = nullptr,
        uint8_t vdp_mask = 0xFF);
    void readback_shutdown(const ayther::engine::VulkanContextView& ctx);

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
    void render(const ayther::engine::VulkanContextView& ctx,
                VkCommandBuffer cmd, const FrameView& fv,
                ayther::engine::PackView pack, bool hd_on = true,
                const AytherLayerStack* layers = nullptr,
                uint8_t vdp_mask = 0xFF);

    // R-8: UV checker mode — an authoring diagnostic view. Every element
    // WITHOUT an asset is painted with a checker anchored to the element, using
    // the colour pair of ITS LAYER (pink+black = authored but failed to load);
    // the rest is dimmed (the HD ones draw normally on top). It only affects
    // the scene passes; on frames that fall back to the blit it does not
    // apply.
    void set_checker(bool on) noexcept;
    [[nodiscard]] bool checker() const noexcept;

    // FOCUSED layer: the one being authored is composed at full intensity and
    // the rest is dimmed, so the working focus reads at a glance.
    // Scene layer index (0=Plane B · 1=Plane A · 2=Window · 3=Sprites);
    // -1 = no focus (everything at full intensity).
    //
    // It lives here and not in the fork's 0x108 channel because the dim is a
    // COMPOSITION decision, and composition has been ours since R-5: that
    // channel is boolean —it dims "the non-sprite"— and knows nothing about a
    // focused layer.
    void set_focus_layer(int layer) noexcept;
    [[nodiscard]] int focus_layer() const noexcept;

    /// R-8: load state of an element's SUB asset (for the Lab coverage
    /// report). NotRequested/Pending/Ready/Failed; elements without a sub
    /// return NotRequested.
    enum class TextureState : std::uint8_t {
        not_requested,
        pending,
        ready,
        failed,
    };
    [[nodiscard]] TextureState sub_texture_state(
        const FrameView& fv, const SceneElement& e) const;

    // ---- The offscreen result — the frontend presents or samples it --------
    /// Returns the public, borrowed Vulkan handoff for the current target.
    /// The handles and synchronization rules are defined by RenderImageView.
    /// GPU access is valid only after render() has produced the current frame.
    [[nodiscard]] engine::RenderImageView render_image() const noexcept;

    // Legacy accessors retained for source compatibility during the 0.1.x line.
    [[nodiscard]] VkImage framebuffer_image() const;
    [[nodiscard]] VkImageView framebuffer_view() const;
    [[nodiscard]] VkSampler framebuffer_sampler() const;
    [[nodiscard]] VkExtent2D framebuffer_extent() const;

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
    [[nodiscard]] bool compare_ready() const;
    /// Returns the public, borrowed Vulkan handoff for the captured image.
    /// The value is invalid until capture_compare() succeeds.
    [[nodiscard]] engine::RenderImageView compare_render_image() const noexcept;
    /// The IMAGE, for the runtime split — which composes with per-region blits
    /// rather than sampling, so it needs the handle and not the view.
    [[nodiscard]] VkImage compare_image() const;
    [[nodiscard]] VkImageView compare_view() const;
    [[nodiscard]] VkSampler compare_sampler() const;
    [[nodiscard]] VkExtent2D compare_extent() const;

    /// Copies the CURRENT offscreen (exactly as render() left it) into the
    /// comparison image. Creates/resizes the image if needed. Record it before
    /// re-rendering the same frame with the other configuration.
    /// false = the image could not be created (out of memory) — the caller
    /// degrades to showing a single version.
    bool capture_compare(const ayther::engine::VulkanContextView& ctx,
                         VkCommandBuffer cmd);
    /// Releases the comparison image (leaving the A/B returns the memory).
    void compare_release(const ayther::engine::VulkanContextView& ctx);

    /// Reads the comparison image back to the CPU — BGRA of the target extent,
    /// using the same readback resources as `export_frame` (requires
    /// readback_init). It exists for the A/B ORACLE: what makes the comparison
    /// honest is that the copy contains exactly what the offscreen held at
    /// capture time, and that can only be asserted by reading it. nullptr if
    /// there is no copy or the submit fails.
    const uint8_t* readback_compare(const ayther::engine::VulkanContextView& ctx);

    /// Capture + its own submit, so capture_compare can be used outside a
    /// frame (oracles). In the Lab the capture travels in the cmd of the frame
    /// in progress.
    bool capture_compare_now(const ayther::engine::VulkanContextView& ctx);

private:
    struct FrameScratch;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ayther
