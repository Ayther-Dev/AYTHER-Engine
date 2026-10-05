// Spec 002, DI-17 (RF-10.1, RF-10.3): the line bands of a frame with raster
// writes in mid-screen. The scene is composed with the VDP state at the end
// of the frame; a line is drawn with another state only when a journaled
// write left a register, a CRAM entry, a VSRAM word or an hscroll word with a
// value other than the final one while that line was drawn. Those lines are
// the bands; the rest of the frame composes as usual. Pure: the session feeds
// it the core's raster journal, the final VDP memories and, when it has
// them, the registers the previous frame ended with.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ayther::session {

/// AYTHER_RASTER_REASON_* bits of the core (`fallback_reasons`).
inline constexpr std::uint32_t kRasterReasonReg = 1U << 0;
inline constexpr std::uint32_t kRasterReasonCram = 1U << 1;
inline constexpr std::uint32_t kRasterReasonVsram = 1U << 2;
inline constexpr std::uint32_t kRasterReasonHscroll = 1U << 3;
inline constexpr std::uint32_t kRasterReasonDma = 1U << 4;
inline constexpr std::uint32_t kRasterReasonVram = 1U << 6;
inline constexpr std::uint32_t kRasterReasonJournalOverflow = 1U << 7;

inline constexpr std::uint32_t kMaxRasterBands = 16;

/// One journaled write: `line` is the first display line drawn after it.
struct RasterEvent {
  std::uint16_t line = 0;
  std::uint16_t reason = 0;
  std::uint16_t address = 0;
  std::uint16_t data = 0;
};

/// The VDP memories at the end of the frame, in the core's layout.
struct RasterFinalState {
  std::span<const std::uint8_t> regs;
  std::span<const std::uint8_t> cram;
  std::span<const std::uint8_t> vsram;
  std::span<const std::uint8_t> vram;
  /// The registers at the end of the previous frame (empty = unknown). The
  /// lines before the first write to a register were drawn with that value:
  /// a one-shot change (the display turned off, a plane base moved) touches
  /// them too.
  std::span<const std::uint8_t> previous_regs;
};

struct RasterBand {
  std::uint16_t y0 = 0; ///< first line
  std::uint16_t y1 = 0; ///< one past the last line
};

struct RasterBands {
  /// False when the writes cannot be placed on lines (a reason the journal
  /// does not record, a dropped event, no journal): the whole frame is
  /// non-composable.
  bool localized = false;
  std::uint32_t count = 0;
  std::array<RasterBand, kMaxRasterBands> bands{};
};

namespace raster_bands_detail {
inline bool read16(std::span<const std::uint8_t> m, std::size_t o,
                   std::uint16_t &out) noexcept {
  if (o + 1 >= m.size())
    return false;
  // The core stores CRAM, VSRAM and VRAM words as host 16-bit values (a
  // little-endian host); its raster replay writes them the same way.
  out = static_cast<std::uint16_t>(m[o] | (m[o + 1] << 8));
  return true;
}

/// The value the frame ends with at the event's address, as the event's
/// `data` is written.
inline bool final_value(const RasterEvent &e, const RasterFinalState &s,
                        std::uint16_t &out) noexcept {
  switch (e.reason) {
  case kRasterReasonReg:
    if (e.address >= s.regs.size())
      return false;
    out = s.regs[e.address];
    return true;
  case kRasterReasonCram:
    return read16(s.cram, static_cast<std::size_t>(e.address & 0x3F) * 2, out);
  case kRasterReasonVsram:
    return read16(s.vsram, e.address & 0x7E, out);
  case kRasterReasonHscroll:
    return read16(s.vram, e.address & 0xFFFE, out);
  default:
    return false;
  }
}
} // namespace raster_bands_detail

/// The bands of a frame whose fallback reasons are `reasons`, from its
/// journal (`events` in the order the core recorded them, `dropped` events
/// that did not fit) and the memories it ends with. A line before the first
/// write to a register is drawn with the value the previous frame ended with
/// (`state.previous_regs`): a one-shot write leaves the lines before it
/// touched. Without that value, and for CRAM, VSRAM and hscroll, a line
/// before the first write is taken to be drawn with the final value, as the
/// core's own raster replay does: a game that changes a colour for a band
/// restores it, in the active area or in vblank, every frame.
///
/// Pattern writes (VRAM) are not journaled. For them `recomposed_diff` flags
/// (non-zero) the lines where the core's image differs from the frame the
/// core recomposes from its final state: those lines were drawn with other
/// patterns. Without it a VRAM write keeps the whole frame non-composable.
[[nodiscard]] inline RasterBands
raster_bands(std::uint32_t reasons, std::span<const RasterEvent> events,
             std::uint32_t dropped, const RasterFinalState &state, int height,
             std::span<const std::uint8_t> recomposed_diff = {}) noexcept {
  using raster_bands_detail::final_value;
  RasterBands out;
  if (reasons == 0) {
    out.localized = true;
    return out;
  }
  constexpr std::uint32_t kJournaled = kRasterReasonReg | kRasterReasonCram |
                                       kRasterReasonVsram |
                                       kRasterReasonHscroll;
  constexpr std::uint32_t kLocalizable =
      kJournaled | kRasterReasonDma | kRasterReasonVram;
  const bool patterns = (reasons & kRasterReasonVram) != 0;
  if ((reasons & ~kLocalizable) != 0 || dropped != 0 || height <= 0 ||
      height > 1024 || ((reasons & kJournaled) != 0 && events.empty()) ||
      (patterns && recomposed_diff.size() < static_cast<std::size_t>(height)))
    return out;
  std::array<bool, 1024> touched{};
  if (patterns)
    for (int y = 0; y < height; ++y)
      touched[static_cast<std::size_t>(y)] =
          recomposed_diff[static_cast<std::size_t>(y)] != 0;
  for (std::size_t i = 0; i < events.size(); ++i) {
    const RasterEvent &e = events[i];
    std::uint16_t final = 0;
    if (!final_value(e, state, final))
      return out;
    // The first write to a register: the lines before it were drawn with the
    // value the previous frame ended with.
    if (e.reason == kRasterReasonReg &&
        e.address < state.previous_regs.size() &&
        state.previous_regs[e.address] != final) {
      bool first = true;
      for (std::size_t j = 0; j < i && first; ++j)
        first = events[j].reason != e.reason || events[j].address != e.address;
      for (int y = 0; first && y < e.line && y < height; ++y)
        touched[static_cast<std::size_t>(y)] = true;
    }
    if (e.data == final)
      continue;
    // Drawn with `e.data` from its line until the next write to the same
    // address, or to the bottom of the frame.
    int end = height;
    for (std::size_t j = i + 1; j < events.size(); ++j)
      if (events[j].reason == e.reason && events[j].address == e.address) {
        end = events[j].line;
        break;
      }
    for (int y = e.line; y < end && y < height; ++y)
      touched[static_cast<std::size_t>(y)] = true;
  }
  // Runs of touched lines.
  std::array<RasterBand, 1024> runs{};
  std::size_t n = 0;
  for (int y = 0; y < height;) {
    if (!touched[static_cast<std::size_t>(y)]) {
      ++y;
      continue;
    }
    const int y0 = y;
    while (y < height && touched[static_cast<std::size_t>(y)])
      ++y;
    runs[n++] = {static_cast<std::uint16_t>(y0), static_cast<std::uint16_t>(y)};
  }
  // More runs than bands: merge the two separated by the smallest gap.
  while (n > kMaxRasterBands) {
    std::size_t best = 0;
    for (std::size_t k = 1; k + 1 < n; ++k)
      if (runs[k + 1].y0 - runs[k].y1 < runs[best + 1].y0 - runs[best].y1)
        best = k;
    runs[best].y1 = runs[best + 1].y1;
    for (std::size_t k = best + 1; k + 1 < n; ++k)
      runs[k] = runs[k + 1];
    --n;
  }
  out.localized = true;
  out.count = static_cast<std::uint32_t>(n);
  for (std::size_t k = 0; k < n; ++k)
    out.bands[k] = runs[k];
  return out;
}

/// Flags (1) every line where two RGB565 images of `width` x `height`
/// differ; `a_pitch` and `b_pitch` are in pixels.
inline void differing_lines(const std::uint16_t *a, std::size_t a_pitch,
                            const std::uint16_t *b, std::size_t b_pitch,
                            int width, int height,
                            std::span<std::uint8_t> out) noexcept {
  for (int y = 0; y < height && static_cast<std::size_t>(y) < out.size(); ++y) {
    const std::uint16_t *ra = a + static_cast<std::size_t>(y) * a_pitch;
    const std::uint16_t *rb = b + static_cast<std::size_t>(y) * b_pitch;
    std::uint8_t differs = 0;
    for (int x = 0; x < width && differs == 0; ++x)
      differs = ra[x] != rb[x] ? 1 : 0;
    out[static_cast<std::size_t>(y)] = differs;
  }
}

} // namespace ayther::session
