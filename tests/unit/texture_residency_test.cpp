// Spec 002, BR-088 (RF-9.1, RF-9.2): a replacement hides the originals it
// claims only with its texture resident. One case per texture state: while
// pending (or not yet requested) the original is drawn, ready draws the
// replacement, and a failed asset keeps the original and reports it.
#include "session/texture_residency.h"

#include <cstdio>
#include <exception>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

using ayther::session::claimed_draw;
using ayther::session::ClaimedDraw;
using ayther::session::Residency;
} // namespace

int main() try {
  check(claimed_draw(Residency::pending) == ClaimedDraw::original,
        "RF-9.1: pending texture → the original stays drawn");
  check(claimed_draw(Residency::not_requested) == ClaimedDraw::original,
        "RF-9.1: texture not yet requested → the original stays drawn");
  check(claimed_draw(Residency::ready) == ClaimedDraw::replacement,
        "RF-9.2: resident texture → the replacement is drawn");
  check(claimed_draw(Residency::failed) == ClaimedDraw::original_missing,
        "RF-9.1: failed texture → the original stays and the asset is "
        "reported missing");
  static_assert(claimed_draw(Residency::ready) == ClaimedDraw::replacement);

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
