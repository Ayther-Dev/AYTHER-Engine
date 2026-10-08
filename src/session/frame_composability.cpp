#include "session/frame_composability.h"

#include <cstddef>

namespace ayther::session {
namespace {

using engine::render_observation::Composability;

std::uint32_t le32(std::span<const std::uint8_t> bytes, std::size_t at) {
  return static_cast<std::uint32_t>(bytes[at]) |
         (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[at + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
}

// Per-line or per-cell hscroll with a real variation over the visible lines.
// The table is written in vblank (the raster signal does not see it), but the
// inventory samples H per band at the tile centre: composing would shear
// inside a tile. A uniform H composes.
bool hscroll_varies(const FrameComposabilityInput &in) {
  const unsigned mode = in.vdp_regs[11] & 3U;
  if (mode == 0)
    return false;
  const std::uint32_t base = static_cast<std::uint32_t>(in.vdp_regs[13] & 0x3F)
                             << 10;
  const std::uint32_t mask = mode == 3 ? 0xFF : mode == 2 ? 0xF8 : 0x07;
  const auto entry = [&](std::uint32_t line) {
    return le32(in.vram, base + ((line & mask) << 2));
  };
  const std::uint32_t first = entry(0);
  for (std::uint32_t line = 1; line < in.fb_height; ++line)
    if (entry(line) != first)
      return true;
  return false;
}

// The vertical twin: 2-cell column vscroll with a variation between the
// visible columns. The inventory takes V per cell from its left column, the
// VDP applies it per pixel column: tiles across the boundary would shear.
bool vscroll_varies(const FrameComposabilityInput &in) {
  if ((in.vdp_regs[11] & 4U) == 0 || in.vsram.size() < 4)
    return false;
  const std::size_t columns = (in.fb_width + 15) / 16;
  const std::uint32_t first = le32(in.vsram, 0);
  for (std::size_t column = 1;
       column < columns && column * 4 + 3 < in.vsram.size(); ++column)
    if (le32(in.vsram, column * 4) != first)
      return true;
  return false;
}

} // namespace

FrameComposability classify_frame(const FrameComposabilityInput &in) noexcept {
  bool hscroll = false;
  bool vscroll = false;
  if (in.vram.size() >= 0x10000 && in.vdp_regs.size() >= 0x20) {
    hscroll = hscroll_varies(in);
    vscroll = vscroll_varies(in);
  }
  // Spec 002 (A): register 1 bit 6 is the display enable. With it off at the
  // end of the frame the VDP shows the backdrop, and while it was off the
  // game could change any VDP state without the raster journal seeing it, so
  // the lines drawn before cannot be checked against the final state either:
  // the whole frame is the core's image.
  const bool display_off =
      in.vdp_regs.size() > 1 && (in.vdp_regs[1] & 0x40U) == 0;
  FrameComposability out;
  out.scene_dirty = static_cast<std::uint8_t>(
      (in.raster > 0 ? kDirtyRaster : 0) | (in.layer_dim ? kDirtyDim : 0) |
      (hscroll || vscroll ? kDirtyScroll : 0) |
      (display_off ? kDirtyDisplayOff : 0) |
      (!in.parsed_sprites_complete ? kDirtyIncompleteSpriteCapture : 0) |
      (!in.geometry_current ? kDirtyGeometryPending : 0));
  if (!in.parsed_sprites_complete)
    out.reason = Composability::other;
  else if (!in.geometry_current)
    out.reason = Composability::other;
  else if (in.raster > 0)
    out.reason = Composability::raster_split;
  else if (display_off)
    out.reason = Composability::other;
  else if (in.layer_dim)
    out.reason = Composability::fade;
  else if (hscroll)
    out.reason = Composability::line_hscroll;
  else if (vscroll)
    out.reason = Composability::column_vscroll;
  return out;
}

} // namespace ayther::session
