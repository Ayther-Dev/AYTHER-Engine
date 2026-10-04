// Spec 002, R2 (RF-8.3): the depth of each part of a pose replacement. The HD
// asset of a pose covers several member sprites, and the VDP draws each of
// them at its own link-chain position: a sprite between two members in the
// chain is in front of one and behind the other. The replacement's rectangle
// is partitioned so that each part takes the depth of the member it covers;
// the silhouette excess (asset pixels outside every member) takes the depth of
// the nearest member. Pure: the session feeds it the pose's members.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ayther::session {

/// A rectangle in screen pixels with the chain position of its member.
struct DepthBox {
  int x = 0, y = 0; ///< top-left
  int w = 0, h = 0; ///< size
  std::uint8_t chain = 0xFF;
};

/// The parts of the replacement rectangle `quad`, each with the chain of the
/// member that owns it: inside a member, that member (the frontmost one where
/// members overlap); outside every member, the nearest member (ties: the
/// frontmost). The parts tile `quad` exactly, merged into rectangles.
inline std::vector<DepthBox>
partition_by_members(const DepthBox &quad, std::span<const DepthBox> members) {
  if (quad.w <= 0 || quad.h <= 0)
    return {};
  if (members.empty())
    return {quad};
  // The owner of each pixel of a row, then rows merged into rectangles: a
  // run of rows with the same segments extends the rectangles above.
  const auto owner = [&](int x, int y) -> std::uint8_t {
    std::uint8_t inside = 0xFF;
    bool any_inside = false;
    long best_d = -1;
    std::uint8_t nearest = 0xFF;
    for (const DepthBox &m : members) {
      if (x >= m.x && x < m.x + m.w && y >= m.y && y < m.y + m.h) {
        if (!any_inside || m.chain < inside)
          inside = m.chain;
        any_inside = true;
        continue;
      }
      // Squared distance (in half pixels) from the pixel centre to the box.
      const long cx = 2L * x + 1, cy = 2L * y + 1;
      const long x0 = 2L * m.x, x1 = 2L * (m.x + m.w);
      const long y0 = 2L * m.y, y1 = 2L * (m.y + m.h);
      const long dx = cx < x0 ? x0 - cx : (cx > x1 ? cx - x1 : 0);
      const long dy = cy < y0 ? y0 - cy : (cy > y1 ? cy - y1 : 0);
      const long d = dx * dx + dy * dy;
      if (best_d < 0 || d < best_d || (d == best_d && m.chain < nearest)) {
        best_d = d;
        nearest = m.chain;
      }
    }
    return any_inside ? inside : nearest;
  };
  struct Run {
    int x0, x1;
    std::uint8_t chain;
    bool operator==(const Run &) const = default;
  };
  std::vector<DepthBox> out;
  std::vector<Run> prev;
  std::vector<std::size_t> open; // index in out of each run of `prev`
  for (int y = quad.y; y < quad.y + quad.h; ++y) {
    std::vector<Run> row;
    for (int x = quad.x; x < quad.x + quad.w; ++x) {
      const std::uint8_t c = owner(x, y);
      if (!row.empty() && row.back().chain == c && row.back().x1 == x)
        row.back().x1 = x + 1;
      else
        row.push_back(Run{x, x + 1, c});
    }
    if (row == prev) {
      for (const std::size_t k : open)
        ++out[k].h;
      continue;
    }
    open.clear();
    for (const Run &r : row) {
      open.push_back(out.size());
      out.push_back(DepthBox{r.x0, y, r.x1 - r.x0, 1, r.chain});
    }
    prev = std::move(row);
  }
  return out;
}

} // namespace ayther::session
