// Spec 002, BR-039 (RF-8.4, RF-9.1, RF-9.3, RF-10.1, RF-10.3): the O2
// structural auditor of the render probe (plan §5.13). Each of its six
// invariants has a synthetic fixture that satisfies it and one that breaks it.
#include "../../tools/render_probe/probe_o2.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <utility>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

using ayther::probe::check_o2;
using ayther::probe::O2Frame;
using ayther::probe::O2Invariant;
using ayther::probe::O2Occurrence;
using ayther::probe::O2Replacement;

O2Occurrence occ(std::uint32_t index, bool core_drawn, bool claimed,
                 bool original_drawn, std::uint8_t depth, std::int16_t x = 0,
                 std::int16_t y = 0, std::uint8_t flips = 0) {
  O2Occurrence o;
  o.index = index;
  o.core_drawn = core_drawn;
  o.claimed = claimed;
  o.original_drawn = original_drawn;
  o.depth = depth;
  o.x = x;
  o.y = y;
  o.flips = flips;
  return o;
}

O2Replacement pose_of(std::uint32_t index, std::uint64_t key,
                      std::vector<std::uint32_t> members) {
  O2Replacement r;
  r.index = index;
  r.key = key;
  r.drawn = true;
  r.texture_ready = true;
  r.partition_depths.assign(members.size(), 0);
  r.members = std::move(members);
  return r;
}

// A pose of members 0 and 1 drawn in two partitions at their depths, and an
// unclaimed sprite 2 drawn as the original. Satisfies every invariant.
O2Frame good_frame() {
  O2Frame f;
  f.occurrences = {occ(0, true, true, false, 3), occ(1, true, true, false, 5),
                   occ(2, true, false, true, 4)};
  O2Replacement pose;
  pose.index = 0;
  pose.key = 0x1234;
  pose.drawn = true;
  pose.texture_ready = true;
  pose.members = {0, 1};
  pose.partition_depths = {3, 5};
  f.replacements = {pose};
  return f;
}

bool violates(const O2Frame &frame, O2Invariant invariant,
              const O2Frame *previous = nullptr) {
  for (const auto &v : check_o2(frame, previous))
    if (v.invariant == invariant)
      return true;
  return false;
}

bool clean(const O2Frame &frame, const O2Frame *previous = nullptr) {
  return check_o2(frame, previous).empty();
}
} // namespace

int main() try {
  const O2Frame good = good_frame();
  check(clean(good) && clean(good, &good),
        "a frame that satisfies every invariant reports nothing");

  // 1. claimed ⊆ members of applied replacements.
  {
    O2Frame bad = good;
    bad.occurrences[2].claimed = true; // claimed, but no replacement has it
    bad.occurrences[2].original_drawn = false;
    check(violates(bad, O2Invariant::claimed_in_applied) &&
              !violates(good, O2Invariant::claimed_in_applied),
          "RF-9.1: a claimed sprite outside every applied replacement is "
          "reported");
  }
  // 2. Every claimed member is drawn with a resident texture.
  {
    O2Frame bad = good;
    bad.replacements[0].texture_ready = false;
    check(violates(bad, O2Invariant::claimed_drawn_resident) &&
              !violates(good, O2Invariant::claimed_drawn_resident),
          "RF-9.1: a claimed member drawn without its texture is reported");
  }
  // 3. Each partition at its member's depth.
  {
    O2Frame bad = good;
    bad.replacements[0].partition_depths = {3, 3}; // member 1 is at 5
    check(violates(bad, O2Invariant::partition_depth) &&
              !violates(good, O2Invariant::partition_depth),
          "RF-8.4: a partition drawn at another member's depth is reported");
  }
  // 4. Every unclaimed sprite the core drew is drawn.
  {
    O2Frame bad = good;
    bad.occurrences[2].original_drawn = false;
    check(violates(bad, O2Invariant::unclaimed_drawn) &&
              !violates(good, O2Invariant::unclaimed_drawn),
          "RF-10.3: an unclaimed sprite missing from the frame is reported");
  }
  // 5. Nothing shows both as original and as replacement.
  {
    O2Frame bad = good;
    bad.occurrences[0].original_drawn = true;
    check(violates(bad, O2Invariant::no_double) &&
              !violates(good, O2Invariant::no_double),
          "RF-10.1: an original drawn under its own replacement is reported");
  }
  // 6. A replacement leaves in the same frame as its members.
  {
    O2Frame lingering = good; // members gone, replacement still drawn
    lingering.occurrences[0].core_drawn = false;
    lingering.occurrences[0].claimed = false;
    lingering.occurrences[1].core_drawn = false;
    lingering.occurrences[1].claimed = false;
    O2Frame early = good; // members still claimed, replacement gone
    early.replacements[0].drawn = false;
    O2Frame gone = good; // members and replacement leave together
    gone.occurrences.resize(1);
    gone.occurrences[0] = occ(2, true, false, true, 4);
    gone.replacements.clear();
    check(violates(lingering, O2Invariant::transition, &good) &&
              violates(early, O2Invariant::transition, &good) &&
              !violates(gone, O2Invariant::transition, &good),
          "RF-9.3: a replacement that outlives its members, or leaves before "
          "them, is reported");
  }

  // 6b. A pose handing off to another pose of the same sprites is not a
  //     replacement leaving before its members (spec 002, whole-take O2:
  //     the 116 `transition` frames of Toma 3 are all hand-offs).
  {
    // Dwarf - Stand 01 -> Stand 02: the same three sprites (hashes); the
    // head turns (h-flip) and moves 6 px. The game alternates the two every
    // 8-9 frames and the replacement changes asset in the same frame.
    O2Frame stand01;
    stand01.occurrences = {occ(10, true, true, false, 1, 100, 50, 0),
                           occ(11, true, true, false, 2, 100, 66, 0),
                           occ(12, true, true, false, 3, 116, 66, 0)};
    stand01.replacements = {pose_of(0, 0xD01, {10, 11, 12})};
    for (std::size_t i = 0; i < 3; ++i)
      stand01.replacements[0].partition_depths[i] =
          stand01.occurrences[i].depth;
    O2Frame stand02 = stand01;
    stand02.occurrences[0].flips = 1;
    stand02.occurrences[0].x = 94;
    stand02.replacements[0].key = 0xD02;
    check(clean(stand02, &stand01) && clean(stand01, &stand02),
          "6b: Dwarf Stand 01 <-> 02 (same hashes, other flips) hands off");
    // The same hand-off with an unchanged layout (two poses of the same
    // sprites in the same arrangement): every member moves to the new pose.
    O2Frame same = stand01;
    same.replacements[0].key = 0xD02;
    check(clean(same, &stand01),
          "6b: a hand-off with the same layout is accepted");
    // Without the new pose the members are claimed by nothing: reported.
    O2Frame dropped = stand01;
    dropped.replacements[0].drawn = false;
    check(violates(dropped, O2Invariant::transition, &stand01),
          "6b: a pose dropped while its members stay as they were is "
          "reported");
    // Split between two other poses: not every member moves to one.
    O2Frame split = stand01;
    split.replacements = {pose_of(1, 0xE01, {10, 11}), pose_of(2, 0xE02, {12})};
    split.replacements[0].partition_depths = {1, 2};
    split.replacements[1].partition_depths = {3};
    check(violates(split, O2Invariant::transition, &stand01),
          "6b: members that scatter over several poses are reported");
  }
  {
    // Maceman - Walk 03 (3 members) -> Walk 01 (5 members): the pose grows
    // as more sprites enter from the right; its three members move to it.
    O2Frame walk03;
    walk03.occurrences = {occ(20, true, true, false, 4, 300, 100),
                          occ(21, true, true, false, 5, 300, 116),
                          occ(22, true, true, false, 6, 300, 132)};
    walk03.replacements = {pose_of(0, 0xA03, {20, 21, 22})};
    walk03.replacements[0].partition_depths = {4, 5, 6};
    O2Frame walk01 = walk03;
    walk01.occurrences.push_back(occ(23, true, true, false, 7, 316, 100));
    walk01.occurrences.push_back(occ(24, true, true, false, 8, 316, 116));
    walk01.replacements = {pose_of(0, 0xA01, {20, 21, 22, 23, 24})};
    walk01.replacements[0].partition_depths = {4, 5, 6, 7, 8};
    check(clean(walk01, &walk03),
          "6b: Maceman Walk 03 -> Walk 01 (3 -> 5 members) hands off");
  }

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
