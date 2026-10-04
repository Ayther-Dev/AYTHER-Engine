// Images of the render probe (spec 002, plan §5.13): the core's framebuffer
// and the renderer's readback as tightly packed RGB8, and their comparison
// for the O1 check. Pure; no session, renderer or files.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ayther::probe {

struct RgbImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgb; // width * height * 3
};

/// The core framebuffer (libretro pixel format 0 = 0RGB1555, 1 = XRGB8888,
/// 2 = RGB565) as RGB8. 5- and 6-bit channels are expanded by bit
/// replication, as the renderer's texture sampling does.
inline RgbImage core_image(const void *pixels, std::uint32_t width,
                           std::uint32_t height, std::uint32_t pitch,
                           int format) {
  RgbImage out{width, height, {}};
  out.rgb.resize(static_cast<std::size_t>(width) * height * 3);
  if (pixels == nullptr)
    return out;
  const auto *base = static_cast<const std::uint8_t *>(pixels);
  for (std::uint32_t y = 0; y < height; ++y) {
    const std::uint8_t *row = base + static_cast<std::size_t>(y) * pitch;
    for (std::uint32_t x = 0; x < width; ++x) {
      std::uint8_t r = 0;
      std::uint8_t g = 0;
      std::uint8_t b = 0;
      if (format == 1) {
        std::uint32_t p = 0;
        std::memcpy(&p, row + static_cast<std::size_t>(x) * 4, 4);
        r = static_cast<std::uint8_t>(p >> 16U);
        g = static_cast<std::uint8_t>(p >> 8U);
        b = static_cast<std::uint8_t>(p);
      } else {
        std::uint16_t p = 0;
        std::memcpy(&p, row + static_cast<std::size_t>(x) * 2, 2);
        const bool rgb565 = format == 2;
        const unsigned r5 = rgb565 ? (p >> 11U) & 0x1FU : (p >> 10U) & 0x1FU;
        const unsigned g6 =
            rgb565 ? (p >> 5U) & 0x3FU : (((p >> 5U) & 0x1FU) << 1U);
        const unsigned b5 = p & 0x1FU;
        r = static_cast<std::uint8_t>((r5 << 3U) | (r5 >> 2U));
        g = static_cast<std::uint8_t>((g6 << 2U) | (g6 >> 4U));
        b = static_cast<std::uint8_t>((b5 << 3U) | (b5 >> 2U));
      }
      std::uint8_t *dst =
          out.rgb.data() + (static_cast<std::size_t>(y) * width + x) * 3;
      dst[0] = r;
      dst[1] = g;
      dst[2] = b;
    }
  }
  return out;
}

/// The renderer's BGRA8 readback as RGB8.
inline RgbImage readback_image(const std::uint8_t *bgra, std::uint32_t width,
                               std::uint32_t height) {
  RgbImage out{width, height, {}};
  const std::size_t n = static_cast<std::size_t>(width) * height;
  out.rgb.resize(n * 3);
  for (std::size_t i = 0; bgra != nullptr && i < n; ++i) {
    out.rgb[i * 3 + 0] = bgra[i * 4 + 2];
    out.rgb[i * 3 + 1] = bgra[i * 4 + 1];
    out.rgb[i * 3 + 2] = bgra[i * 4 + 0];
  }
  return out;
}

/// O1 (plan §5.13): whether the composed image is pixel for pixel the
/// core's framebuffer.
struct O1Result {
  bool identical = false;
  std::size_t differing_pixels = 0;
  std::uint32_t first_x = 0; ///< first differing pixel (row-major)
  std::uint32_t first_y = 0;
};

inline O1Result check_o1(const RgbImage &composed, const RgbImage &core) {
  O1Result out;
  if (composed.width != core.width || composed.height != core.height ||
      composed.rgb.size() != core.rgb.size()) {
    out.differing_pixels = static_cast<std::size_t>(core.width) * core.height;
    return out;
  }
  for (std::uint32_t y = 0; y < core.height; ++y)
    for (std::uint32_t x = 0; x < core.width; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * core.width + x) * 3;
      if (std::memcmp(composed.rgb.data() + i, core.rgb.data() + i, 3) == 0)
        continue;
      if (out.differing_pixels == 0) {
        out.first_x = x;
        out.first_y = y;
      }
      ++out.differing_pixels;
    }
  out.identical = out.differing_pixels == 0;
  return out;
}

} // namespace ayther::probe
