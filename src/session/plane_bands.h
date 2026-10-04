// Spec 002, R8 (RF-10.1, RF-8.2): planes A and B composed exactly under
// per-line hscroll and 2-cell column vscroll. A rigid cell quad cannot shear,
// so the visible screen is cut into bands — runs of lines with one H, times
// 16-pixel columns with one V — and every cell is placed per band with that
// band's scroll and clipped to it. Pure: the session feeds it the frame's
// VRAM (bus view as the session reads it), VDP registers and VSRAM.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ayther::session {

/// One placed cell of plane A (0) or B (1), clipped to its band.
struct BandCell {
  std::int16_t x = 0, y = 0; ///< top-left of the 8x8 cell, screen px
  std::uint16_t pattern = 0;
  std::uint8_t palette = 0;
  std::uint8_t flips = 0; ///< bit0 hflip · bit1 vflip
  std::uint8_t priority = 0;
  std::uint8_t plane = 0;
  std::int16_t clip_x0 = 0, clip_y0 = 0, clip_x1 = 0, clip_y1 = 0;
};

struct PlaneBandInput {
  std::span<const std::uint8_t>
      vram; ///< 64 KiB, read as LE words like the session
  std::span<const std::uint8_t> regs;  ///< 0x20 VDP registers
  std::span<const std::uint8_t> vsram; ///< 80 bytes
  int width = 320;
  int height = 224;
};

namespace plane_bands_detail {
inline std::uint32_t le16(std::span<const std::uint8_t> m, std::uint32_t o) {
  return o + 1 < m.size() ? (std::uint32_t)m[o] | ((std::uint32_t)m[o + 1] << 8)
                          : 0U;
}
inline std::uint32_t le32(std::span<const std::uint8_t> m, std::uint32_t o) {
  return le16(m, o) | (le16(m, o + 2) << 16);
}
inline int plane_cells(int bits) {
  return bits == 1 ? 64 : (bits == 3 ? 128 : 32);
}
inline int floor_div(int a, int b) {
  return a >= 0 ? a / b : -((-a + b - 1) / b);
}
inline int wrap(int a, int n) { return ((a % n) + n) % n; }
} // namespace plane_bands_detail

/// H of `plane` (0 = A, 1 = B) on screen line `line`.
inline int plane_hscroll(const PlaneBandInput &in, int plane, int line) {
  using namespace plane_bands_detail;
  if (in.regs.size() < 0x20)
    return 0;
  const std::uint32_t hscb = ((std::uint32_t)in.regs[0x0D] << 10) & 0xFC00;
  const std::uint32_t hmask_tab[4] = {0x00, 0x07, 0xF8, 0xFF};
  const std::uint32_t hmask = hmask_tab[in.regs[0x0B] & 3];
  const std::uint32_t hw =
      le32(in.vram, hscb + (((std::uint32_t)line & hmask) << 2));
  return plane == 0 ? (int)(hw & 0x3FF) : (int)((hw >> 16) & 0x3FF);
}

/// V of `plane` on the 16-px screen column `column` (the global V without
/// 2-cell mode).
inline int plane_vscroll(const PlaneBandInput &in, int plane, int column) {
  using namespace plane_bands_detail;
  if (in.regs.size() < 0x20)
    return 0;
  const bool two_cell = (in.regs[0x0B] & 0x04) != 0;
  const int col = two_cell ? (std::min)((std::max)(column, 0), 19) : 0;
  const std::uint32_t vw = le32(in.vsram, (std::uint32_t)col * 4);
  return plane == 0 ? (int)(vw & 0x3FF) : (int)((vw >> 16) & 0x3FF);
}

/// Whether the frame needs bands: H varies over the visible lines or V
/// varies over the visible columns, for plane A or B.
inline bool needs_bands(const PlaneBandInput &in) {
  if (in.regs.size() < 0x20 || in.vram.size() < 0x10000)
    return false;
  const int columns = (in.width + 15) / 16;
  for (int plane = 0; plane < 2; ++plane) {
    const int h0 = plane_hscroll(in, plane, 0);
    for (int line = 1; line < in.height; ++line)
      if (plane_hscroll(in, plane, line) != h0)
        return true;
    const int v0 = plane_vscroll(in, plane, 0);
    for (int column = 1; column < columns; ++column)
      if (plane_vscroll(in, plane, column) != v0)
        return true;
  }
  return false;
}

/// The cells of `plane`, placed and clipped per band.
inline std::vector<BandCell> band_cells(const PlaneBandInput &in, int plane) {
  using namespace plane_bands_detail;
  std::vector<BandCell> out;
  if (in.regs.size() < 0x20 || in.vram.size() < 0x10000 || plane < 0 ||
      plane > 1)
    return out;
  const int wc = plane_cells(in.regs[0x10] & 3);
  const int hc = plane_cells((in.regs[0x10] >> 4) & 3);
  const std::uint32_t base = plane == 0
                                 ? (std::uint32_t)(in.regs[0x02] & 0x38) << 10
                                 : (std::uint32_t)(in.regs[0x04] & 0x07) << 13;
  // Line bands (one H each) and column bands (one V each), merged runs.
  struct Run {
    int lo, hi, value;
  };
  std::vector<Run> rows;
  for (int line = 0; line < in.height; ++line) {
    const int h = plane_hscroll(in, plane, line);
    if (!rows.empty() && rows.back().value == h)
      rows.back().hi = line + 1;
    else
      rows.push_back({line, line + 1, h});
  }
  std::vector<Run> cols;
  for (int column = 0; column * 16 < in.width; ++column) {
    const int v = plane_vscroll(in, plane, column);
    const int x1 = (std::min)(column * 16 + 16, in.width);
    if (!cols.empty() && cols.back().value == v)
      cols.back().hi = x1;
    else
      cols.push_back({column * 16, x1, v});
  }
  for (const Run &r : rows)
    for (const Run &c : cols) {
      const int h = r.value;
      const int v = c.value;
      // Cells whose screen rect meets the band: screen x = cell*8 + h,
      // screen y = row*8 - v (the session's mapping, unwrapped).
      for (int row = floor_div(r.lo + v, 8); row <= floor_div(r.hi - 1 + v, 8);
           ++row)
        for (int cell = floor_div(c.lo - h, 8);
             cell <= floor_div(c.hi - 1 - h, 8); ++cell) {
          const std::uint32_t w = le16(
              in.vram,
              base + (std::uint32_t)(wrap(row, hc) * wc + wrap(cell, wc)) * 2);
          const std::uint16_t pattern = w & 0x7FF;
          if (pattern == 0)
            continue;
          BandCell bc;
          bc.x = (std::int16_t)(cell * 8 + h);
          bc.y = (std::int16_t)(row * 8 - v);
          bc.pattern = pattern;
          bc.palette = (std::uint8_t)((w >> 13) & 3);
          bc.flips = (std::uint8_t)(((w >> 11) & 1) | (((w >> 12) & 1) << 1));
          bc.priority = (std::uint8_t)((w >> 15) & 1);
          bc.plane = (std::uint8_t)plane;
          bc.clip_x0 = (std::int16_t)c.lo;
          bc.clip_x1 = (std::int16_t)c.hi;
          bc.clip_y0 = (std::int16_t)r.lo;
          bc.clip_y1 = (std::int16_t)r.hi;
          out.push_back(bc);
        }
    }
  return out;
}

} // namespace ayther::session
