#include <ayther/engine/music_event_scoring.hpp>

#include <cstdio>

namespace {
void check(bool value, const char *message, int &failures) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

ayther::engine::MusicEvent event(std::uint64_t id, std::uint64_t begin,
                                 std::uint64_t duration,
                                 std::uint64_t signature,
                                 std::uint64_t causal_order) {
  return {ayther::engine::EventId{id},
          begin,
          duration,
          signature,
          1,
          60,
          ayther::engine::EventProvenance{"fixture", 3, causal_order}};
}
} // namespace

int main() {
  using namespace ayther::engine;
  int failures = 0;

  check(classify_music_score(0.90) == MusicScoreCategory::high &&
            classify_music_score(0.899) == MusicScoreCategory::medium &&
            classify_music_score(0.75) == MusicScoreCategory::medium &&
            classify_music_score(0.749) == MusicScoreCategory::low,
        "0.75 and 0.90 boundaries are exact", failures);

  const auto aligned =
      score_music_events({event(1, 100, 5, 10, 1), event(2, 105, 5, 11, 2),
                          event(3, 110, 5, 12, 3)},
                         {event(4, 200, 5, 10, 1), event(5, 207, 7, 11, 2),
                          event(6, 214, 5, 12, 3), event(7, 208, 1, 99, 4)},
                         "all instruments", 2);
  check(aligned.reference_count == 3 && aligned.observed_count == 4 &&
            aligned.match_count == 2 && aligned.additional_count == 2,
        "alignment is one-to-one, keeps additions and does not time-stretch",
        failures);
  check(aligned.score && *aligned.score == 4.0 / 7.0,
        "S publishes 2M/(R+O) and all counts", failures);

  const auto empty = score_music_events({}, {}, "empty", 2);
  check(!empty.score && empty.category == MusicScoreCategory::no_pattern,
        "zero denominator is not applicable and has no pattern", failures);

  auto unknown = event(8, 0, 1, 1, 1);
  unknown.pitch.reset();
  const auto missing = score_music_events({unknown}, {unknown}, "tonal", 2);
  check(missing.match_count == 0,
        "a missing required attribute is not a wildcard", failures);

  const auto too_few = score_music_events(
      {event(10, 0, 1, 10, 1), event(11, 1, 1, 11, 2), event(12, 2, 1, 12, 3)},
      {event(20, 0, 1, 10, 1), event(21, 1, 1, 11, 2), event(22, 2, 1, 12, 3)},
      "short", 2);
  check(too_few.score && *too_few.score == 1.0 && too_few.insufficient_evidence,
        "fewer than four pairs remains insufficient at S=1", failures);

  const auto one_begin =
      score_music_events({event(30, 0, 1, 20, 1), event(31, 0, 1, 21, 1),
                          event(32, 0, 1, 22, 1), event(33, 0, 1, 23, 1)},
                         {event(40, 0, 1, 20, 1), event(41, 0, 1, 21, 1),
                          event(42, 0, 1, 22, 1), event(43, 0, 1, 23, 1)},
                         "simultaneous", 2);
  check(one_begin.insufficient_evidence,
        "fewer than two distinct starts remains insufficient", failures);

  const auto omitted_first = score_music_events(
      {event(50, 0, 1, 30, 1), event(51, 4, 1, 31, 2), event(52, 8, 1, 32, 3),
       event(53, 12, 1, 33, 4), event(54, 16, 1, 34, 5)},
      {event(61, 4, 1, 31, 2), event(62, 8, 1, 32, 3), event(63, 12, 1, 33, 4),
       event(64, 16, 1, 34, 5)},
      "omitted prefix", 2);
  check(
      omitted_first.match_count == 4 && omitted_first.score &&
          *omitted_first.score == 8.0 / 9.0,
      "alignment anchors at the first observable pair after a prefix omission",
      failures);

  return failures == 0 ? 0 : 1;
}
