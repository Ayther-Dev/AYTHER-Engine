#include <ayther/engine/music_transition_cancel.hpp>

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
  MusicTransitionQueue queue;
  queue.begin_link();
  (void)queue.submit({1, SequenceNodeId{2}, 1, 3, AppearanceId{4}, true}, 0);
  auto result = cancel_music_transition(queue, 2, 48'000, false,
                                        TransitionCancelCause::external_replace);
  check(queue.pending_count() == 0 && result.closed_voices == 2 &&
            result.fade_frames == 240 && result.destination_deferred &&
            !result.resume_allowed,
        "external replacement discards pending and fades before destination",
        failures);

  queue.begin_link();
  (void)queue.submit({2, SequenceNodeId{3}, 1, 3, AppearanceId{5}, true}, 0);
  result = cancel_music_transition(queue, 3, 48'000, true,
                                   TransitionCancelCause::external_cancel);
  check(queue.pending_count() == 0 && result.closed_voices == 3 &&
            result.fade_frames == 0 && !result.pcm_synthesized &&
            !result.resume_allowed,
        "paused cancellation releases voices without ramp or resurrection",
        failures);

  queue.begin_link();
  (void)queue.submit({3, SequenceNodeId{4}, 1, 8, AppearanceId{6}, true}, 0);
  result = cancel_music_transition(queue, 1, 48'000, false,
                                   TransitionCancelCause::restore_generation);
  check(queue.pending_count() == 0 && result.transaction_boundary &&
            result.fade_frames == 0 && !result.pcm_synthesized,
        "restore delegates output boundary without cross-generation fade",
        failures);
  return failures == 0 ? 0 : 1;
}
