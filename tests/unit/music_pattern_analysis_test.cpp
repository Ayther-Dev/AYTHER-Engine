#include <ayther/engine/music_pattern_analysis.hpp>

#include <algorithm>
#include <cstdio>

namespace {
void check(bool value, const char *message, int &failures) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

ayther::engine::MusicEvent event(std::uint64_t id, std::uint64_t begin,
                                 std::uint64_t signature) {
  return {ayther::engine::EventId{id}, begin, 1, signature, 1, 60,
          ayther::engine::EventProvenance{"fixture", 2, id}};
}
} // namespace

int main() {
  using namespace ayther::engine;
  int failures = 0;

  const std::vector<MusicEvent> events{
      event(1, 0, 10),  event(2, 4, 11),  event(3, 8, 12),
      event(4, 12, 13), event(5, 20, 10), event(6, 24, 11),
      event(7, 26, 99), event(8, 28, 12), event(9, 32, 13),
      event(10, 40, 10), event(11, 44, 11), event(12, 52, 13)};
  const auto original = events;
  const auto proposals = find_music_patterns(
      events, PatternAnalysisOptions{.minimum_events = 2,
                                     .maximum_events = 5,
                                     .minimum_score = 0.75,
                                     .frame_tolerance = 2});

  check(events == original, "analysis never mutates original events", failures);
  check(std::ranges::any_of(proposals, [](const auto &proposal) {
          return proposal.score.match_count == 4 &&
                 proposal.score.observed_count == 5 && proposal.approximate;
        }),
        "a repeated motif with inserted noise is proposed", failures);
  check(std::ranges::any_of(proposals, [](const auto &proposal) {
          return proposal.score.match_count == 3 &&
                 proposal.score.reference_count == 4 &&
                 proposal.score.observed_count == 3;
        }),
        "a repetition with an omitted note remains visible", failures);

  const std::vector<MusicEvent> overlapping{
      event(20, 0, 1), event(21, 2, 2), event(22, 4, 1),
      event(23, 6, 2), event(24, 8, 1), event(25, 10, 2)};
  const auto overlap_proposals = find_music_patterns(
      overlapping, PatternAnalysisOptions{.minimum_events = 4,
                                           .maximum_events = 4,
                                           .minimum_score = 0.90,
                                           .frame_tolerance = 0});
  check(std::ranges::any_of(overlap_proposals, [](const auto &proposal) {
          return proposal.reference.begin < proposal.occurrence.begin &&
                 proposal.reference.end > proposal.occurrence.begin;
        }),
        "overlapping repeated intervals are retained", failures);

  const std::vector<NamedMusicPattern> catalog{
      {PatternId{1}, "whole", {event(30, 0, 1), event(31, 2, 2),
                                 event(32, 4, 3), event(33, 6, 4)}},
      {PatternId{2}, "prefix", {event(40, 0, 1), event(41, 2, 2)}},
      {PatternId{3}, "inside", {event(50, 0, 2), event(51, 2, 3)}}};
  const auto relations = relate_music_patterns(catalog, 0);
  check(std::ranges::any_of(relations, [](const auto &relation) {
          return relation.smaller == PatternId{2} &&
                 relation.larger == PatternId{1} &&
                 relation.kind == PatternRelationKind::prefix;
        }),
        "shared prefixes are reported", failures);
  check(std::ranges::any_of(relations, [](const auto &relation) {
          return relation.smaller == PatternId{3} &&
                 relation.larger == PatternId{1} &&
                 relation.kind == PatternRelationKind::contained;
        }),
        "contained motifs are not discarded by maximality", failures);

  return failures == 0 ? 0 : 1;
}
