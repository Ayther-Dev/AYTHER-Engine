// Spec 002, BR-098 (RF-8.1, RF-8.4): a plane set with pri-0 and pri-1 cells
// is split by priority, so it is drawn in both passes; a set of one
// priority stays one quad over its whole box.
#include "session/plane_set_split.h"

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

using ayther::session::SetCell;
using ayther::session::SetQuad;
using ayther::session::split_set_by_priority;

bool has(const std::vector<SetQuad> &q, SetQuad want) {
  for (const SetQuad &x : q)
    if (x.x == want.x && x.y == want.y && x.w == want.w && x.h == want.h &&
        x.priority == want.priority)
      return true;
  return false;
}
} // namespace

int main() try {
  // One priority: one quad over the whole 3x2 box.
  const std::vector<SetCell> flat = {{0, 0, 1}, {1, 0, 1}, {2, 0, 1},
                                     {0, 1, 1}, {1, 1, 1}, {2, 1, 1}};
  const auto one = split_set_by_priority(3, 2, flat, 1);
  check(one.size() == 1 && has(one, {0, 0, 3, 2, 1}),
        "a set of one priority stays one quad over its box");

  // Row 0: pri-0, pri-0, pri-1. Row 1: pri-1, pri-0, pri-0.
  const std::vector<SetCell> mixed = {{0, 0, 0}, {1, 0, 0}, {2, 0, 1},
                                      {0, 1, 1}, {1, 1, 0}, {2, 1, 0}};
  const auto parts = split_set_by_priority(3, 2, mixed, 1);
  check(parts.size() == 4 && has(parts, {0, 0, 2, 1, 0}) &&
            has(parts, {2, 0, 1, 1, 1}) && has(parts, {0, 1, 1, 1, 1}) &&
            has(parts, {1, 1, 2, 1, 0}),
        "RF-8.1: a mixed set splits into runs per row, each at its "
        "priority");

  // A gap (a member off screen) breaks a run.
  const std::vector<SetCell> gap = {{0, 0, 0}, {2, 0, 0}, {1, 1, 1}};
  const auto runs = split_set_by_priority(3, 2, gap, 0);
  check(runs.size() == 3 && has(runs, {0, 0, 1, 1, 0}) &&
            has(runs, {2, 0, 1, 1, 0}) && has(runs, {1, 1, 1, 1, 1}),
        "only visible member cells are covered when the set is split");

  // No visible cells: the anchor's priority, whole box.
  const auto none = split_set_by_priority(2, 2, {}, 0);
  check(none.size() == 1 && has(none, {0, 0, 2, 2, 0}),
        "a set without visible cells keeps one quad");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
