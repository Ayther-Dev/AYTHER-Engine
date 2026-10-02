#include <ayther/engine/music_source_lifecycle.hpp>

#include <cstdio>

namespace {
void check(bool value, const char *message, int &failures) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}
} // namespace

int main() {
  using namespace ayther::engine;
  int failures = 0;
  auto decision = admit_music_source(
      {100, 120, false, false, 240, MusicSourceFailure::none});
  check(decision.action == MusicSourceAction::reject &&
            decision.diagnostic == "source_too_short",
        "known source deficit without loop is rejected before playback",
        failures);

  decision = admit_music_source(
      {100, 120, true, true, 240, MusicSourceFailure::none});
  check(decision.action == MusicSourceAction::repeat_authored_loop &&
            decision.fade_frames == 100,
        "explicit loop may bridge pending transition and shortens fade",
        failures);

  decision = admit_music_source(
      {100, 120, false, true, 240, MusicSourceFailure::none});
  check(decision.action == MusicSourceAction::return_to_original &&
            decision.diagnostic == "source_exhausted_before_transition" &&
            decision.fade_frames == 100,
        "exhausted source without authored loop returns original", failures);

  decision = admit_music_source(
      {1000, 120, false, false, 240, MusicSourceFailure::decoder});
  check(decision.action == MusicSourceAction::close_contribution &&
            decision.diagnostic == "asset_playback_failed" &&
            !decision.restart_requested,
        "decoder failure closes contribution without automatic restart",
        failures);
  return failures == 0 ? 0 : 1;
}
