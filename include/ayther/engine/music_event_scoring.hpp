#pragma once

#include <ayther/engine/music_event_normalization.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace ayther::engine {

enum class MusicScoreCategory : std::uint8_t {
  no_pattern,
  low,
  medium,
  high,
};

struct MusicAlignmentScore {
  std::size_t match_count{};
  std::size_t reference_count{};
  std::size_t observed_count{};
  std::size_t additional_count{};
  std::size_t distinct_matched_starts{};
  std::optional<double> score;
  MusicScoreCategory category{MusicScoreCategory::no_pattern};
  bool insufficient_evidence{true};
  std::string scope;
};

[[nodiscard]] constexpr MusicScoreCategory
classify_music_score(double score) noexcept {
  if (score >= 0.90)
    return MusicScoreCategory::high;
  if (score >= 0.75)
    return MusicScoreCategory::medium;
  return MusicScoreCategory::low;
}

namespace detail {
[[nodiscard]] constexpr std::uint64_t distance(std::uint64_t left,
                                               std::uint64_t right) noexcept {
  return left > right ? left - right : right - left;
}

[[nodiscard]] inline std::vector<MusicEvent>
causal_ordered(std::vector<MusicEvent> events) {
  std::ranges::sort(events, {}, [](const MusicEvent &event) {
    return std::tuple{event.begin, event.provenance.causal_order,
                      event.provenance.source_ordinal, event.id.value};
  });
  return events;
}
} // namespace detail

[[nodiscard]] inline MusicAlignmentScore
score_music_events(std::vector<MusicEvent> reference,
                   std::vector<MusicEvent> observed, std::string scope,
                   std::uint64_t frame_tolerance) {
  reference = detail::causal_ordered(std::move(reference));
  observed = detail::causal_ordered(std::move(observed));
  MusicAlignmentScore result;
  result.reference_count = reference.size();
  result.observed_count = observed.size();
  result.scope = std::move(scope);
  if (reference.empty() && observed.empty())
    return result;

  std::size_t reference_anchor = 0;
  std::size_t observed_anchor = 0;
  bool anchor_found = false;
  for (std::size_t expected = 0; expected < reference.size() && !anchor_found;
       ++expected)
    for (std::size_t actual = 0; actual < observed.size(); ++actual)
      if (events_equivalent(reference[expected], observed[actual])) {
        reference_anchor = expected;
        observed_anchor = actual;
        anchor_found = true;
        break;
      }
  const std::uint64_t reference_origin =
      anchor_found ? reference[reference_anchor].begin : 0;
  const std::uint64_t observed_origin =
      anchor_found ? observed[observed_anchor].begin : 0;
  std::size_t observed_cursor = observed_anchor;
  std::optional<std::uint64_t> previous_matched_start;
  for (std::size_t expected_index = reference_anchor;
       anchor_found && expected_index < reference.size(); ++expected_index) {
    const auto &expected = reference[expected_index];
    for (std::size_t index = observed_cursor; index < observed.size(); ++index) {
      const auto &actual = observed[index];
      if (!events_equivalent(expected, actual))
        continue;
      const auto expected_relative = expected.begin - reference_origin;
      const auto actual_relative = actual.begin - observed_origin;
      if (detail::distance(expected_relative, actual_relative) > frame_tolerance)
        continue;
      if (expected.duration && actual.duration &&
          detail::distance(*expected.duration, *actual.duration) > frame_tolerance)
        continue;
      ++result.match_count;
      if (!previous_matched_start || *previous_matched_start != expected.begin) {
        ++result.distinct_matched_starts;
        previous_matched_start = expected.begin;
      }
      observed_cursor = index + 1;
      break;
    }
  }

  result.additional_count = result.observed_count - result.match_count;
  const auto denominator = result.reference_count + result.observed_count;
  if (denominator == 0)
    return result;
  result.score = (2.0 * static_cast<double>(result.match_count)) /
                 static_cast<double>(denominator);
  result.category = classify_music_score(*result.score);
  result.insufficient_evidence = result.match_count < 4 ||
                                 result.distinct_matched_starts < 2;
  return result;
}

} // namespace ayther::engine
