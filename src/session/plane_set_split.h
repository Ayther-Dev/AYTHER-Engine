// Spec 002, R4 (RF-8.1, RF-8.4): a plane set whose cells mix VDP priorities
// is drawn in both passes. Its replacement is split into quads by priority —
// per row, runs of contiguous member cells of the same priority — so each
// part takes the z of the cells it replaces; a set of one priority stays one
// quad over its whole box. Pure: the session feeds it the matched cells.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ayther::session {

struct SetCell {
  std::uint16_t dx = 0; ///< cell column inside the set box
  std::uint16_t dy = 0; ///< cell row inside the set box
  std::uint8_t priority = 0;
};

struct SetQuad {
  std::uint16_t x = 0, y = 0; ///< first cell inside the box
  std::uint16_t w = 0, h = 0; ///< size in cells
  std::uint8_t priority = 0;
};

/// The quads of a `w_cells`×`h_cells` set from its visible member `cells`.
/// `anchor_priority` is the priority of a set whose cells all agree (or of a
/// set without visible cells).
inline std::vector<SetQuad>
split_set_by_priority(std::uint16_t w_cells, std::uint16_t h_cells,
                      std::span<const SetCell> cells,
                      std::uint8_t anchor_priority) {
  bool mixed = false;
  for (const SetCell &c : cells)
    mixed = mixed || c.priority != cells[0].priority;
  if (!mixed) {
    const std::uint8_t pri =
        cells.empty() ? anchor_priority : cells[0].priority;
    return {SetQuad{0, 0, w_cells, h_cells, pri}};
  }
  // Per row, runs of contiguous member cells of one priority.
  std::vector<std::int8_t> grid(static_cast<std::size_t>(w_cells) * h_cells,
                                -1);
  for (const SetCell &c : cells)
    if (c.dx < w_cells && c.dy < h_cells)
      grid[static_cast<std::size_t>(c.dy) * w_cells + c.dx] =
          static_cast<std::int8_t>(c.priority & 1U);
  std::vector<SetQuad> out;
  for (std::uint16_t y = 0; y < h_cells; ++y) {
    std::uint16_t x = 0;
    while (x < w_cells) {
      const std::int8_t p = grid[static_cast<std::size_t>(y) * w_cells + x];
      if (p < 0) {
        ++x;
        continue;
      }
      std::uint16_t end = x + 1;
      while (end < w_cells &&
             grid[static_cast<std::size_t>(y) * w_cells + end] == p)
        ++end;
      out.push_back(SetQuad{x, y, static_cast<std::uint16_t>(end - x), 1,
                            static_cast<std::uint8_t>(p)});
      x = end;
    }
  }
  return out;
}

} // namespace ayther::session
