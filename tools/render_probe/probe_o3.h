// O3 of the render probe (spec 002, DI-17): continuity between consecutive
// frames. A block of the composed image may only change where the core's
// image changes too. Pure; no session, renderer or files.
#pragma once

#include "probe_images.h"

#include <cstdint>

namespace ayther::probe {

struct O3Result {
  std::uint32_t unexplained_blocks = 0;
  int y0 = 0; ///< first line of the reported blocks
  int y1 = 0; ///< one past their last line
};

/// Blocks of 8x8 pixels. A block is a discontinuity when the composed image
/// changes by more than `kO3Change` (mean absolute difference per channel)
/// between `prev` and `cur`, also allowing for motion (kO3Motion), while the
/// core's image changes by no more than `kO3CoreStill` in the same block.
inline constexpr int kO3Block = 8;
inline constexpr int kO3Change = 24;
inline constexpr int kO3CoreStill = 4;
/// Motion: a block that matches the previous composed image displaced by up
/// to this many pixels moved (scroll, a walking sprite); it did not change.
inline constexpr int kO3Motion = 8;

[[nodiscard]] inline int o3_block_change(const RgbImage &a, const RgbImage &b,
                                         int bx, int by) noexcept {
  long sum = 0;
  int n = 0;
  for (int y = by; y < by + kO3Block && y < static_cast<int>(a.height); ++y)
    for (int x = bx; x < bx + kO3Block && x < static_cast<int>(a.width); ++x)
      for (int c = 0; c < 3; ++c) {
        const std::size_t i =
            (static_cast<std::size_t>(y) * a.width + x) * 3 + c;
        const int d = static_cast<int>(a.rgb[i]) - static_cast<int>(b.rgb[i]);
        sum += d < 0 ? -d : d;
        ++n;
      }
  return n ? static_cast<int>(sum / n) : 0;
}

/// The smallest change of a block of `cur` against `prev` displaced by up to
/// kO3Motion pixels in each direction (pixels outside `prev` are skipped).
[[nodiscard]] inline int o3_block_motion(const RgbImage &prev,
                                         const RgbImage &cur, int bx,
                                         int by) noexcept {
  int best = 256;
  const int w = static_cast<int>(cur.width);
  const int h = static_cast<int>(cur.height);
  for (int dy = -kO3Motion; dy <= kO3Motion && best > 0; ++dy)
    for (int dx = -kO3Motion; dx <= kO3Motion && best > 0; ++dx) {
      long sum = 0;
      int n = 0;
      for (int y = by; y < by + kO3Block && y < h; ++y)
        for (int x = bx; x < bx + kO3Block && x < w; ++x) {
          const int px = x + dx;
          const int py = y + dy;
          if (px < 0 || py < 0 || px >= w || py >= h)
            continue;
          for (int c = 0; c < 3; ++c) {
            const int a =
                cur.rgb[(static_cast<std::size_t>(y) * cur.width + x) * 3 + c];
            const int b =
                prev.rgb[(static_cast<std::size_t>(py) * prev.width + px) * 3 +
                         c];
            sum += a > b ? a - b : b - a;
            ++n;
          }
        }
      if (n >= kO3Block * kO3Block * 3 / 2 && sum / n < best)
        best = static_cast<int>(sum / n);
    }
  return best;
}

[[nodiscard]] inline O3Result check_o3(const RgbImage &prev_composed,
                                       const RgbImage &composed,
                                       const RgbImage &prev_core,
                                       const RgbImage &core) noexcept {
  O3Result out;
  const auto same = [](const RgbImage &a, const RgbImage &b) {
    return a.width == b.width && a.height == b.height &&
           a.rgb.size() == static_cast<std::size_t>(a.width) * a.height * 3 &&
           b.rgb.size() == a.rgb.size();
  };
  if (!same(prev_composed, composed) || !same(prev_core, core) ||
      !same(composed, core))
    return out;
  for (int by = 0; by < static_cast<int>(core.height); by += kO3Block)
    for (int bx = 0; bx < static_cast<int>(core.width); bx += kO3Block) {
      if (o3_block_change(prev_composed, composed, bx, by) <= kO3Change ||
          o3_block_change(prev_core, core, bx, by) > kO3CoreStill ||
          o3_block_motion(prev_composed, composed, bx, by) <= kO3Change)
        continue;
      if (out.unexplained_blocks == 0 || by < out.y0)
        out.y0 = by;
      const int bottom = by + kO3Block < static_cast<int>(core.height)
                             ? by + kO3Block
                             : static_cast<int>(core.height);
      if (bottom > out.y1)
        out.y1 = bottom;
      ++out.unexplained_blocks;
    }
  return out;
}

} // namespace ayther::probe
