#include <ayther/engine/music_transition_queue.hpp>

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
  auto result =
      queue.submit({1, SequenceNodeId{2}, 5, 10, AppearanceId{7}, true}, 100);
  check(result.status == TransitionQueueStatus::queued &&
            queue.pending_count() == 1 &&
            queue.deadline_music_ns() == 2'000'000'100ULL,
        "active link accepts one bounded pending transition", failures);

  result =
      queue.submit({2, SequenceNodeId{2}, 5, 10, AppearanceId{7}, true}, 500);
  check(result.status == TransitionQueueStatus::replaced_recent &&
            queue.pending_event_id() == 2 &&
            queue.deadline_music_ns() == 2'000'000'100ULL,
        "equal same destination keeps recent cause without extending deadline",
        failures);
  result =
      queue.submit({3, SequenceNodeId{3}, 5, 10, AppearanceId{7}, true}, 600);
  check(result.status == TransitionQueueStatus::conflict &&
            result.diagnostic == "transition_conflict" &&
            queue.pending_event_id() == 2,
        "equal distinct destinations report conflict", failures);
  result =
      queue.submit({4, SequenceNodeId{3}, 6, 10, AppearanceId{7}, true}, 700);
  check(result.status == TransitionQueueStatus::replaced_priority &&
            queue.pending_event_id() == 4,
        "higher priority replaces the sole pending request", failures);

  result = queue.finish_link(10, AppearanceId{8}, true, 800);
  check(result.status == TransitionQueueStatus::invalidated &&
            queue.pending_count() == 0,
        "appearance is revalidated when active link finishes", failures);

  queue.begin_link();
  check(
      queue.submit({5, SequenceNodeId{2}, 1, 11, AppearanceId{9}, true}, 1'000)
              .status == TransitionQueueStatus::queued,
      "second pending request is accepted", failures);
  result = queue.finish_link(11, AppearanceId{9}, true, 2'000'001'000ULL);
  check(result.status == TransitionQueueStatus::timed_out &&
            result.diagnostic == "recognition_timeout",
        "two-second musical deadline is inclusive and fixed", failures);

  queue.begin_link();
  (void)queue.submit({6, SequenceNodeId{2}, 1, 12, AppearanceId{10}, true}, 0);
  result = queue.finish_link(12, AppearanceId{10}, true, 1);
  check(result.status == TransitionQueueStatus::selected &&
            result.destination == SequenceNodeId{2},
        "valid pending request is selected once", failures);
  check(queue.finish_link(12, AppearanceId{10}, true, 2).status ==
            TransitionQueueStatus::none,
        "consumed transition cannot fire twice", failures);
  return failures == 0 ? 0 : 1;
}
