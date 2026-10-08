// Spec 002, DI-20 (RF-10.1, RF-10.3): what a raster band shows. DI-17 shows
// the core's image in the lines drawn with another VDP state. A pixel of
// those lines that the core drew exactly as in the previous frame was not
// changed by the writes, so the previous frame's composed HD is its faithful
// picture; only the pixels that changed need the core's image. This splits
// the band lines into runs of the two kinds. Pure: the renderer decides
// whether the previous composed frame is usable at all (it is frame k-1, was
// composed whole, and no sprite crosses the band) and blits the runs.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace ayther::session {

/// Bound on the runs of one frame (Toma 3 frame 1645, the box text, needs
/// about a thousand). Above it the bands stay the core's image
/// (DI-17), never a truncated mix.
inline constexpr std::uint32_t kMaxCarryRuns = 4096;

/// A band of lines [y0, y1), as `FrameView::raster_bands` holds them.
struct LineBand {
  std::int32_t y0 = 0;
  std::int32_t y1 = 0;
};

/// The core's image of one frame, in its native format.
struct CoreImage {
  const std::uint8_t *pixels = nullptr;
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::size_t pitch = 0;
  std::int32_t bytes_per_pixel = 0;
};

/// Pixels [x0, x1) of line y: from the previous composed frame, or the core.
struct CarryRun {
  std::int16_t y = 0;
  std::int16_t x0 = 0;
  std::int16_t x1 = 0;
  bool from_previous = false;
};

struct BandCarry {
  /// False: show every band line from the core's image, as DI-17 does.
  bool usable = false;
  std::uint32_t count = 0;
  std::array<CarryRun, kMaxCarryRuns> runs{};
};

[[nodiscard]] inline BandCarry band_carry(const CoreImage &previous,
                                          const CoreImage &current,
                                          std::span<const LineBand> bands) {
  BandCarry out;
  if (!previous.pixels || !current.pixels || current.width <= 0 ||
      current.height <= 0 || current.bytes_per_pixel <= 0 ||
      previous.width != current.width || previous.height != current.height ||
      previous.bytes_per_pixel != current.bytes_per_pixel)
    return out;
  const auto bpp = static_cast<std::size_t>(current.bytes_per_pixel);
  // A pixel changed by the writes, or any of its 8 neighbours: the previous
  // HD of a tile is slightly larger than its original pixels, so a fringe of
  // it must not survive next to a changed pixel.
  const auto changed = [&](std::int32_t x, std::int32_t y) {
    return std::memcmp(previous.pixels + previous.pitch * y + bpp * x,
                       current.pixels + current.pitch * y + bpp * x, bpp) != 0;
  };
  const auto keeps = [&](std::int32_t x, std::int32_t y) {
    for (std::int32_t ny = y - 1; ny <= y + 1; ++ny)
      for (std::int32_t nx = x - 1; nx <= x + 1; ++nx)
        if (nx >= 0 && ny >= 0 && nx < current.width && ny < current.height &&
            changed(nx, ny))
          return false;
    return true;
  };
  bool carried = false;
  for (const LineBand &band : bands) {
    const std::int32_t y0 = band.y0 < 0 ? 0 : band.y0;
    const std::int32_t y1 = band.y1 > current.height ? current.height : band.y1;
    for (std::int32_t y = y0; y < y1; ++y) {
      std::int32_t start = 0;
      bool same = keeps(0, y);
      for (std::int32_t x = 1; x <= current.width; ++x) {
        const bool next = x < current.width && keeps(x, y);
        if (x < current.width && next == same)
          continue;
        if (out.count == kMaxCarryRuns)
          return BandCarry{};
        out.runs[out.count++] = {static_cast<std::int16_t>(y),
                                 static_cast<std::int16_t>(start),
                                 static_cast<std::int16_t>(x), same};
        carried = carried || same;
        start = x;
        same = next;
      }
    }
  }
  if (!carried)
    return BandCarry{};
  out.usable = true;
  return out;
}

} // namespace ayther::session
