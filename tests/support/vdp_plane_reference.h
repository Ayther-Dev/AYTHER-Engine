// CPU per-pixel reference of the VDP planes A and B (spec 002, R8 tests):
// every screen pixel looks its plane cell up through that line's hscroll and
// that 16-px column's vscroll, as the VDP does. It knows nothing about cells
// or bands, so it checks the banded composition (session/plane_bands.h)
// independently. VRAM follows the session's view: name-table and scroll
// words read little-endian, pattern bytes at offset ^ 1.
#pragma once

#include "reference_compositor.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ayther::test {

struct VdpPlanes {
  std::span<const std::uint8_t> vram;
  std::span<const std::uint8_t> regs;
  std::span<const std::uint8_t> vsram;
  std::span<const std::uint8_t> cram;
  int width = 320;
  int height = 224;
};

inline std::uint32_t le16(std::span<const std::uint8_t> m, std::uint32_t o) {
  return o + 1 < m.size() ? (std::uint32_t)m[o] | ((std::uint32_t)m[o + 1] << 8)
                          : 0U;
}
inline std::uint32_t le32(std::span<const std::uint8_t> m, std::uint32_t o) {
  return le16(m, o) | (le16(m, o + 2) << 16);
}

/// RGB565 of planes B and A (low then high priority) over the backdrop.
inline std::vector<std::uint16_t> compose_vdp_planes(const VdpPlanes &v) {
  const auto cells = [](int b) { return b == 1 ? 64 : (b == 3 ? 128 : 32); };
  const int wc = cells(v.regs[0x10] & 3);
  const int hc = cells((v.regs[0x10] >> 4) & 3);
  const std::uint32_t base[2] = {(std::uint32_t)(v.regs[0x02] & 0x38) << 10,
                                 (std::uint32_t)(v.regs[0x04] & 0x07) << 13};
  const std::uint32_t hscb = ((std::uint32_t)v.regs[0x0D] << 10) & 0xFC00;
  const std::uint32_t hmask_tab[4] = {0x00, 0x07, 0xF8, 0xFF};
  const std::uint32_t hmask = hmask_tab[v.regs[0x0B] & 3];
  const bool two_cell = (v.regs[0x0B] & 0x04) != 0;
  std::array<std::uint16_t, 64> pal{};
  for (std::size_t i = 0; i < 64 && i * 2 + 1 < v.cram.size(); ++i)
    pal[i] =
        genesis565((std::uint16_t)(v.cram[i * 2] | (v.cram[i * 2 + 1] << 8)));
  const std::uint16_t backdrop = pal[v.regs[0x07] & 0x3F];
  std::vector<std::uint16_t> img((std::size_t)v.width * v.height, backdrop);
  std::vector<std::uint8_t> idx((std::size_t)v.width * v.height * 2, 0);
  std::vector<std::uint8_t> pri((std::size_t)v.width * v.height * 2, 0);
  for (int plane = 0; plane < 2; ++plane)
    for (int y = 0; y < v.height; ++y) {
      const std::uint32_t hw =
          le32(v.vram, hscb + (((std::uint32_t)y & hmask) << 2));
      const int h = plane == 0 ? (int)(hw & 0x3FF) : (int)((hw >> 16) & 0x3FF);
      for (int x = 0; x < v.width; ++x) {
        const int col = two_cell ? (std::min)(x >> 4, 19) : 0;
        const std::uint32_t vw = le32(v.vsram, (std::uint32_t)col * 4);
        const int vs =
            plane == 0 ? (int)(vw & 0x3FF) : (int)((vw >> 16) & 0x3FF);
        const int px = ((x - h) % (wc * 8) + wc * 8) % (wc * 8);
        const int py = ((y + vs) % (hc * 8) + hc * 8) % (hc * 8);
        const std::uint32_t w = le16(
            v.vram, base[plane] + (std::uint32_t)((py / 8) * wc + px / 8) * 2);
        const std::uint16_t pattern = w & 0x7FF;
        if (pattern == 0)
          continue; // the session treats tile 0 as blank
        const int tx = (w & 0x0800) ? 7 - (px & 7) : (px & 7);
        const int ty = (w & 0x1000) ? 7 - (py & 7) : (py & 7);
        const std::uint8_t i = tile_pixel(v.vram, pattern, tx, ty);
        if (i == 0)
          continue;
        const std::size_t at = ((std::size_t)y * v.width + x) * 2 + plane;
        idx[at] = (std::uint8_t)((((w >> 13) & 3) << 4) | i);
        pri[at] = (std::uint8_t)((w >> 15) & 1);
      }
    }
  for (int level = 0; level < 2; ++level)
    for (int plane = 1; plane >= 0; --plane) // B then A
      for (std::size_t p = 0; p < (std::size_t)v.width * v.height; ++p) {
        const std::size_t at = p * 2 + plane;
        if (idx[at] != 0 && pri[at] == level)
          img[p] = pal[idx[at]];
      }
  return img;
}

} // namespace ayther::test
