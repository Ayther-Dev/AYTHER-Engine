// Spec 002, BR-104 (RF-8.3): the partition of a pose replacement by its
// members. Contiguous members split the rectangle at their border; separated
// members split the gap by distance; the silhouette excess takes the depth of
// the nearest member; overlapping members give the overlap to the frontmost.
#include "session/pose_depth.h"

#include <cstdio>
#include <exception>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

using ayther::session::DepthBox;
using ayther::session::partition_by_members;

/// The chain the partition gives pixel (x, y), or -1 if no part covers it,
/// -2 if two parts do.
int chain_at(const std::vector<DepthBox> &parts, int x, int y) {
  int found = -1;
  for (const DepthBox &p : parts)
    if (x >= p.x && x < p.x + p.w && y >= p.y && y < p.y + p.h)
      found = found == -1 ? p.chain : -2;
  return found;
}

/// The parts tile `quad` exactly: every pixel covered once.
bool tiles(const std::vector<DepthBox> &parts, const DepthBox &quad) {
  for (int y = quad.y; y < quad.y + quad.h; ++y)
    for (int x = quad.x; x < quad.x + quad.w; ++x)
      if (chain_at(parts, x, y) < 0)
        return false;
  return true;
}
} // namespace

int main() try {
  // Contiguous: a 32x16 quad over two 16x16 members, chains 2 (left) and 5.
  {
    const DepthBox quad{100, 50, 32, 16, 0};
    const std::vector<DepthBox> m = {{100, 50, 16, 16, 2},
                                     {116, 50, 16, 16, 5}};
    const auto parts = partition_by_members(quad, m);
    check(tiles(parts, quad) && chain_at(parts, 105, 55) == 2 &&
              chain_at(parts, 125, 55) == 5,
          "RF-8.3: contiguous members split the quad at their border, each "
          "part at its member's depth");
    check(parts.size() == 2, "contiguous members give exactly two parts");
  }
  // Separated: members at x 0..8 and 24..32 of a 32-wide quad; the gap is
  // split by distance.
  {
    const DepthBox quad{0, 0, 32, 8, 0};
    const std::vector<DepthBox> m = {{0, 0, 8, 8, 1}, {24, 0, 8, 8, 3}};
    const auto parts = partition_by_members(quad, m);
    check(tiles(parts, quad) && chain_at(parts, 10, 4) == 1 &&
              chain_at(parts, 22, 4) == 3,
          "RF-8.3: the gap between separated members goes to the nearest one");
  }
  // Silhouette excess: the asset overflows its single member by 8 px on each
  // side; a second member below. Excess pixels take the nearest member.
  {
    const DepthBox quad{40, 40, 32, 40, 0};
    const std::vector<DepthBox> m = {{48, 48, 16, 16, 4}, {48, 64, 16, 8, 1}};
    const auto parts = partition_by_members(quad, m);
    check(tiles(parts, quad) && chain_at(parts, 41, 41) == 4 &&
              chain_at(parts, 70, 50) == 4 && chain_at(parts, 41, 78) == 1 &&
              chain_at(parts, 55, 66) == 1,
          "RF-8.3: the silhouette excess takes the depth of the nearest "
          "member");
  }
  // Overlap: the frontmost member owns the overlap.
  {
    const DepthBox quad{0, 0, 24, 16, 0};
    const std::vector<DepthBox> m = {{0, 0, 16, 16, 6}, {8, 0, 16, 16, 2}};
    const auto parts = partition_by_members(quad, m);
    check(tiles(parts, quad) && chain_at(parts, 4, 4) == 6 &&
              chain_at(parts, 12, 4) == 2 && chain_at(parts, 20, 4) == 2,
          "RF-8.3: where members overlap, the frontmost owns the pixels");
  }
  // One member: one part, the whole quad.
  {
    const DepthBox quad{0, 0, 16, 16, 0};
    const std::vector<DepthBox> m = {{4, 4, 8, 8, 9}};
    const auto parts = partition_by_members(quad, m);
    check(parts.size() == 1 && tiles(parts, quad) && chain_at(parts, 0, 0) == 9,
          "a single member gives one part over the whole quad");
  }

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
