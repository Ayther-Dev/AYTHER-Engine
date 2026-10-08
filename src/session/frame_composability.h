// Why a frame can or cannot be composed element by element (spec 002,
// contracts.md C3 Composability). Pure: the session feeds it the frame's
// raster signal and VDP tables; the renderer only reads the scene_dirty bits.
#pragma once

#include <ayther/engine/render_observer.hpp>

#include <cstdint>
#include <span>

namespace ayther::session {

/// FrameView::scene_dirty bits.
inline constexpr std::uint8_t kDirtyRaster = 1; // mid-screen raster writes
inline constexpr std::uint8_t kDirtyDim = 2;    // authoring dim of Animate
inline constexpr std::uint8_t kDirtyScroll = 4; // line hscroll / column vscroll
/// Spec 002 (A): the display is off (VDP register 1, bit 6) at the end of
/// the frame. The VDP shows the backdrop only: the frame is the core's
/// image, without HD.
inline constexpr std::uint8_t kDirtyDisplayOff = 8;
/// The core truncated the parsed-sprite stream. The scene inventory and its
/// depth/claim relationships are incomplete, so the complete core frame wins.
inline constexpr std::uint8_t kDirtyIncompleteSpriteCapture = 16;
/// VDP_REGS already describes the following geometry while the framebuffer
/// and SYSTEM viewport still describe the frame that was emitted. Mixing
/// either state into scene composition would combine two different frames.
inline constexpr std::uint8_t kDirtyGeometryPending = 32;

/// True when no part of the HD scene may be composited over the core image.
/// Kept beside the dirty-bit contract so the renderer and unit tests consume
/// exactly the same mask.
[[nodiscard]] inline constexpr bool
frame_requires_core_image(std::uint8_t scene_dirty) noexcept {
  return (scene_dirty &
          (kDirtyRaster | kDirtyDisplayOff | kDirtyIncompleteSpriteCapture |
           kDirtyGeometryPending)) != 0;
}

struct FrameComposabilityInput {
  /// Raster fallback reasons of the frame (ABI snapshot) or the legacy count
  /// of mid-screen writes; 0 = none.
  std::uint32_t raster = 0;
  /// The authoring dim of the Animate view is on.
  bool layer_dim = false;
  /// False when AYTHER_OVERFLOW_PARSED_SPRITES marks the ABI snapshot.
  bool parsed_sprites_complete = true;
  /// False while AYTHER_SYSTEM_GEOMETRY_PENDING says VDP_REGS belongs to the
  /// next frame rather than to the framebuffer being published.
  bool geometry_current = true;
  std::span<const std::uint8_t> vram;
  std::span<const std::uint8_t> vdp_regs;
  std::span<const std::uint8_t> vsram;
  std::uint32_t fb_width = 0;
  std::uint32_t fb_height = 0;
};

struct FrameComposability {
  std::uint8_t scene_dirty = 0;
  /// The first reason that applies, in this order: other (incomplete sprite
  /// capture), other (geometry pending), raster_split, other (the display is
  /// off), fade, line_hscroll, column_vscroll.
  engine::render_observation::Composability reason =
      engine::render_observation::Composability::composable;
};

[[nodiscard]] FrameComposability
classify_frame(const FrameComposabilityInput &in) noexcept;

} // namespace ayther::session
