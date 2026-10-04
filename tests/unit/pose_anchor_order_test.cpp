// Spec 002, BR-084 (RF-8.1, RF-8.4): each pose substitution is ordered by its
// frontmost member. Two overlapping poses whose SAT slot order and link-chain
// order disagree: the anchor (and so the depth) follows the chain, which is
// the order the VDP draws. No occurrence outside a pose can anchor it, and a
// pose without members has no anchor (no slot taken from its area).
#include "session/pose_anchor.h"

#include <array>
#include <cstdint>
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

using ayther::session::kNoPoseAnchor;
using ayther::session::pose_anchors;

constexpr std::uint32_t kNone = UINT32_MAX;

AytherSpriteOccurrence occ(std::uint8_t slot, std::uint8_t w = 2,
                           std::uint8_t h = 2) {
  AytherSpriteOccurrence o{};
  o.slot = slot;
  o.w_tiles = w;
  o.h_tiles = h;
  o.hash = 0x1000U + slot;
  return o;
}
} // namespace

int main() try {
  // Pose A: slots 2 (chain 4) and 5 (chain 1). Pose B: slots 3 (chain 0) and
  // 4 (chain 3). By slot A is in front (2 < 3); by chain B is (0 < 1).
  // Slot 1 (chain 2) belongs to no pose.
  const std::vector<AytherSpriteOccurrence> occs = {occ(2), occ(5), occ(3),
                                                    occ(4), occ(1)};
  const std::vector<std::uint32_t> owner = {0, 0, 1, 1, kNone};
  std::array<std::uint8_t, 80> chain{};
  chain.fill(0xFF);
  chain[1] = 2;
  chain[2] = 4;
  chain[3] = 0;
  chain[4] = 3;
  chain[5] = 1;
  std::array<std::uint32_t, 2> anchor{};
  pose_anchors(occs, owner, chain, anchor);
  check(anchor[0] == 1, "RF-8.1: pose A is anchored at its frontmost member "
                        "by chain (slot 5), not by slot (slot 2)");
  check(anchor[1] == 2,
        "RF-8.1: pose B is anchored at its frontmost member (slot 3)");
  check(chain[occs[anchor[1]].slot] < chain[occs[anchor[0]].slot],
        "RF-8.4: B orders in front of A, as the VDP chain draws them");

  // Opposite chain order on the same slots: the anchors follow the chain.
  chain[2] = 0;
  chain[3] = 4;
  chain[4] = 1;
  chain[5] = 3;
  pose_anchors(occs, owner, chain, anchor);
  check(anchor[0] == 0 && anchor[1] == 3,
        "RF-8.4: with the opposite chain order the anchors swap members");

  // Unknown chain: the lowest SAT slot among the members.
  chain.fill(0xFF);
  pose_anchors(occs, owner, chain, anchor);
  check(anchor[0] == 0 && anchor[1] == 2,
        "unknown chain falls back to the lowest member slot");

  // A 1x1 pose whose only member is at slot 0, with a foreign sprite at slot 1
  // (== its area): the foreign sprite never anchors it. A second pose has no
  // member on screen: no anchor.
  const std::vector<AytherSpriteOccurrence> single = {occ(1, 2, 1),
                                                      occ(0, 1, 1)};
  const std::vector<std::uint32_t> single_owner = {kNone, 0};
  std::array<std::uint32_t, 2> single_anchor{};
  pose_anchors(single, single_owner, chain, single_anchor);
  check(single_anchor[0] == 1,
        "RF-8.1: a foreign sprite at slot == area does not anchor the pose");
  check(single_anchor[1] == kNoPoseAnchor,
        "a pose without members has no anchor (no slot from its area)");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
