// Spec 002, BR-039 (RF-8.4, RF-9.1, RF-9.3, RF-10.1, RF-10.3): the O2
// structural auditor of the render probe (plan §5.13). Each of its six
// invariants has a synthetic fixture that satisfies it and one that breaks it.
#include "../../tools/render_probe/probe_o2.h"

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

using ayther::probe::check_o2;
using ayther::probe::O2Frame;
using ayther::probe::O2Invariant;
using ayther::probe::O2Occurrence;
using ayther::probe::O2Replacement;

// A pose of members 0 and 1 drawn in two partitions at their depths, and an
// unclaimed sprite 2 drawn as the original. Satisfies every invariant.
O2Frame good_frame() {
  O2Frame f;
  f.occurrences = {{0, true, true, false, 3},
                   {1, true, true, false, 5},
                   {2, true, false, true, 4}};
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
    gone.occurrences[0] = {2, true, false, true, 4};
    gone.replacements.clear();
    check(violates(lingering, O2Invariant::transition, &good) &&
              violates(early, O2Invariant::transition, &good) &&
              !violates(gone, O2Invariant::transition, &good),
          "RF-9.3: a replacement that outlives its members, or leaves before "
          "them, is reported");
  }

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
