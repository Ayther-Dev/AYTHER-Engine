#include <ayther/engine/music_analysis_limits.hpp>
#include <ayther/engine/music_analysis_snapshot.hpp>
#include <ayther/engine/music_pattern_analysis.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {
void check(bool value, const char *message, int &failures) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

ayther::engine::MusicEvent event(std::uint64_t id, std::uint64_t begin,
                                 std::uint64_t signature) {
  return {ayther::engine::EventId{id},
          begin,
          1,
          signature,
          1,
          60,
          ayther::engine::EventProvenance{"take", 1, id}};
}

std::vector<std::string> fingerprints(
    const std::vector<ayther::engine::MusicPatternProposal> &proposals) {
  std::vector<std::string> result;
  for (const auto &proposal : proposals) {
    result.push_back(std::to_string(proposal.reference.begin) + ":" +
                     std::to_string(proposal.reference.end) + ":" +
                     std::to_string(proposal.occurrence.begin) + ":" +
                     std::to_string(proposal.occurrence.end) + ":" +
                     std::to_string(proposal.score.match_count) + ":" +
                     proposal.reason);
  }
  return result;
}
} // namespace

int main() {
  using namespace ayther::engine;
  int failures = 0;
  const std::vector<MusicEvent> ordered{event(1, 0, 10),  event(2, 4, 11),
                                        event(3, 8, 12),  event(4, 20, 10),
                                        event(5, 24, 11), event(6, 28, 12)};
  auto reversed = ordered;
  std::ranges::reverse(reversed);
  const PatternAnalysisOptions options{.minimum_events = 2,
                                       .maximum_events = 3,
                                       .minimum_score = 0.75,
                                       .frame_tolerance = 2};
  check(fingerprints(find_music_patterns(ordered, options)) ==
            fingerprints(find_music_patterns(reversed, options)),
        "same revision yields identical proposals and reasons despite storage "
        "order",
        failures);
  check(reversed.front().id == EventId{6},
        "analysis leaves reordered source data untouched", failures);

  AnalysisCoordinator coordinator;
  coordinator.accept(AuthorDecision{ProposalId{44}, 5});
  AnalysisInputs inputs;
  inputs.take = "take";
  inputs.events = ordered;
  inputs.range = {0, 40};
  inputs.authored_revision = 9;
  const auto token = coordinator.begin(inputs);
  check(token && coordinator.cancel(*token) &&
            coordinator.publish(*token, {{ProposalId{45}, "late"}}) ==
                PublishResult::cancelled &&
            coordinator.accepted_decisions().size() == 1,
        "late publication cannot alter accepted decisions", failures);

  const auto empty = find_music_patterns({}, options);
  auto missing = ordered;
  for (auto &value : missing)
    value.pitch.reset();
  check(empty.empty() && find_music_patterns(missing, options).empty(),
        "empty and attribute-missing inputs preserve an empty result",
        failures);

  constexpr std::uint64_t mib = 1024ULL * 1024ULL;
  const AnalysisWorkMetrics at_limit{
      256 * mib, 10'000, 120'000'000'000ULL, 2'000'000'000ULL, 6, 6, true};
  auto exceeded = at_limit;
  ++exceeded.intervals;
  check(evaluate_analysis_work(at_limit).status ==
                AnalysisWorkStatus::complete &&
            evaluate_analysis_work(exceeded).status ==
                AnalysisWorkStatus::analysis_limit &&
            evaluate_analysis_work(exceeded).acceptance_blocked,
        "exact and excessive P18-02 boundaries remain deterministic", failures);

  return failures == 0 ? 0 : 1;
}
