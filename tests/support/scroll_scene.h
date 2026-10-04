// Synthetic VDP scenes with per-line hscroll and 2-cell column vscroll for
// the spec 002 R8 tests: planes A and B filled with cells of four patterns
// whose pixels vary inside the tile (so any shear or misplacement shows), and
// the scroll tables of each case. Same memory view as the session.
#pragma once

#include <ayther/ayther_session.h>

#include "session/plane_bands.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ayther::test {

struct ScrollScene {
  std::vector<std::uint8_t> vram = std::vector<std::uint8_t>(0x10000, 0);
  std::vector<std::uint8_t> regs = std::vector<std::uint8_t>(0x20, 0);
  std::vector<std::uint8_t> vsram = std::vector<std::uint8_t>(80, 0);
  std::vector<std::uint8_t> cram = std::vector<std::uint8_t>(128, 0);
  int width = 320;
  int height = 224;

  enum class Mode { column_vscroll, line_hscroll, both };

  explicit ScrollScene(Mode mode) {
    // 64x32 planes; A at 0xC000, B at 0xE000; hscroll table at 0xFC00.
    regs[0x10] = 0x01;
    regs[0x02] = 0x30;
    regs[0x04] = 0x07;
    regs[0x0D] = 0x3F;
    regs[0x07] = 0x00;
    regs[0x0B] = mode == Mode::column_vscroll ? 0x04
                 : mode == Mode::line_hscroll ? 0x03
                                              : 0x07;
    // Patterns 1..4: index (x + 2y + p) % 15 + 1 (pattern bytes at ^1).
    for (std::uint32_t p = 1; p <= 4; ++p)
      for (std::uint32_t y = 0; y < 8; ++y)
        for (std::uint32_t x = 0; x < 8; x += 2) {
          const auto ix = [&](std::uint32_t xx) {
            return (std::uint8_t)((xx + 2 * y + p) % 15 + 1);
          };
          vram[(p * 32 + y * 4 + x / 2) ^ 1U] =
              (std::uint8_t)((ix(x) << 4) | ix(x + 1));
        }
    // Name tables: A uses palette 0, B palette 1; patterns by position,
    // some flipped, some high priority, a few empty (pattern 0).
    for (int plane = 0; plane < 2; ++plane) {
      const std::uint32_t base = plane == 0 ? 0xC000U : 0xE000U;
      for (int cy = 0; cy < 32; ++cy)
        for (int cx = 0; cx < 64; ++cx) {
          const int k = cx * 7 + cy * 3 + plane;
          std::uint16_t w = (std::uint16_t)(k % 5); // 0 = empty
          if (w != 0) {
            w |= (std::uint16_t)((plane & 3) << 13);
            if (k % 3 == 0)
              w |= 0x0800; // hflip
            if (k % 7 == 0)
              w |= 0x1000; // vflip
            if (k % 4 == 0)
              w |= 0x8000; // high priority
          }
          const std::uint32_t at = base + (std::uint32_t)(cy * 64 + cx) * 2;
          vram[at] = (std::uint8_t)(w & 0xFF);
          vram[at + 1] = (std::uint8_t)(w >> 8);
        }
    }
    // Scroll tables.
    for (int line = 0; line < 256; ++line) {
      int ha = 0;
      int hb = 0;
      if (mode != Mode::column_vscroll) {
        ha = (line / 2) & 0x3FF;     // a slow ramp: a band per 2 lines
        hb = (line * 3 + 5) & 0x3FF; // a fast one: a band per line
      }
      const std::uint32_t at = 0xFC00U + (std::uint32_t)line * 4;
      const std::uint32_t word = (std::uint32_t)ha | ((std::uint32_t)hb << 16);
      for (int b = 0; b < 4; ++b)
        vram[at + b] = (std::uint8_t)(word >> (8 * b));
    }
    for (int col = 0; col < 20; ++col) {
      int va = 0;
      int vb = 0;
      if (mode != Mode::line_hscroll) {
        va = (col * 3) & 0x3FF;
        vb = (col * 5 + 1) & 0x3FF;
      }
      const std::uint32_t word = (std::uint32_t)va | ((std::uint32_t)vb << 16);
      for (int b = 0; b < 4; ++b)
        vsram[(std::size_t)col * 4 + b] = (std::uint8_t)(word >> (8 * b));
    }
    // Palettes 0 and 1: 15 distinct colours each.
    for (int i = 1; i < 16; ++i) {
      const std::uint16_t c0 = (std::uint16_t)((i & 7) | ((i >> 1) << 3));
      const std::uint16_t c1 = (std::uint16_t)(((i >> 1) << 6) | (i & 7) << 3);
      cram[(std::size_t)i * 2] = (std::uint8_t)(c0 & 0xFF);
      cram[(std::size_t)i * 2 + 1] = (std::uint8_t)(c0 >> 8);
      cram[(std::size_t)(16 + i) * 2] = (std::uint8_t)(c1 & 0xFF);
      cram[(std::size_t)(16 + i) * 2 + 1] = (std::uint8_t)(c1 >> 8);
    }
  }

  [[nodiscard]] session::PlaneBandInput input() const {
    return {vram, regs, vsram, width, height};
  }

  /// Scene elements of the banded cells of planes A and B, in the session's
  /// draw order: per priority, plane B then plane A.
  [[nodiscard]] std::vector<SceneElement> elements() const {
    std::vector<SceneElement> out;
    for (int pri = 0; pri < 2; ++pri)
      for (int plane = 1; plane >= 0; --plane)
        for (const session::BandCell &c : session::band_cells(input(), plane)) {
          if (c.priority != pri)
            continue;
          SceneElement e{};
          e.x = c.x;
          e.y = c.y;
          e.w = 8;
          e.h = 8;
          e.pattern = c.pattern;
          e.palette = c.palette;
          e.flips = c.flips;
          e.priority = c.priority;
          e.layer = c.plane == 0 ? 1 : 0; // scene: 0 = B, 1 = A
          e.clip_x0 = c.clip_x0;
          e.clip_y0 = c.clip_y0;
          e.clip_x1 = c.clip_x1;
          e.clip_y1 = c.clip_y1;
          out.push_back(e);
        }
    return out;
  }
};

} // namespace ayther::test
