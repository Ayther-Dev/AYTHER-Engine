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

struct FrameComposabilityInput {
  /// Raster fallback reasons of the frame (ABI snapshot) or the legacy count
  /// of mid-screen writes; 0 = none.
  std::uint32_t raster = 0;
  /// The authoring dim of the Animate view is on.
  bool layer_dim = false;
  std::span<const std::uint8_t> vram;
  std::span<const std::uint8_t> vdp_regs;
  std::span<const std::uint8_t> vsram;
  std::uint32_t fb_width = 0;
  std::uint32_t fb_height = 0;
};

struct FrameComposability {
  std::uint8_t scene_dirty = 0;
  /// The first reason that applies, in this order: raster_split, other (the
  /// display is off), fade, line_hscroll, column_vscroll.
  engine::render_observation::Composability reason =
      engine::render_observation::Composability::composable;
};

[[nodiscard]] FrameComposability
classify_frame(const FrameComposabilityInput &in) noexcept;

} // namespace ayther::session
