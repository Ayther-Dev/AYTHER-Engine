#pragma once

#include <ayther/engine/music_event_scoring.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ayther::engine {

struct PatternAnalysisOptions {
  std::size_t minimum_events{2};
  std::size_t maximum_events{64};
  double minimum_score{0.75};
  std::uint64_t frame_tolerance{2};
};

struct MusicPatternProposal {
  EventRange reference{};
  EventRange occurrence{};
  MusicAlignmentScore score;
  bool approximate{};
  std::string reason;
};

struct PatternId {
  std::uint64_t value{};
  friend constexpr bool operator==(PatternId, PatternId) noexcept = default;
};

struct NamedMusicPattern {
  PatternId id{};
  std::string name;
  std::vector<MusicEvent> events;
};

enum class PatternRelationKind : std::uint8_t { prefix, contained };

struct PatternRelation {
  PatternId smaller{};
  PatternId larger{};
  PatternRelationKind kind{PatternRelationKind::contained};
  std::size_t offset{};
};

namespace detail {
[[nodiscard]] inline std::vector<MusicEvent>
event_window(const std::vector<MusicEvent> &events, std::size_t begin,
             std::size_t count) {
  return {events.begin() + static_cast<std::ptrdiff_t>(begin),
          events.begin() + static_cast<std::ptrdiff_t>(begin + count)};
}

[[nodiscard]] inline EventRange
event_range(const std::vector<MusicEvent> &events, std::size_t begin,
            std::size_t count) {
  const auto &last = events[begin + count - 1];
  return {events[begin].begin, last.begin + last.duration.value_or(1)};
}

[[nodiscard]] inline bool identity_window_matches(
    const std::vector<MusicEvent> &smaller,
    const std::vector<MusicEvent> &larger, std::size_t offset,
    std::uint64_t frame_tolerance) {
  if (offset + smaller.size() > larger.size() || smaller.empty())
    return false;
  const auto smaller_origin = smaller.front().begin;
  const auto larger_origin = larger[offset].begin;
  for (std::size_t index = 0; index < smaller.size(); ++index) {
    const auto &left = smaller[index];
    const auto &right = larger[offset + index];
    if (!events_equivalent(left, right) ||
        distance(left.begin - smaller_origin, right.begin - larger_origin) >
            frame_tolerance)
      return false;
  }
  return true;
}
} // namespace detail

[[nodiscard]] inline std::vector<MusicPatternProposal>
find_music_patterns(const std::vector<MusicEvent> &input,
                    const PatternAnalysisOptions &options) {
  const auto events = detail::causal_ordered(input);
  std::vector<MusicPatternProposal> proposals;
  if (options.minimum_events < 2 || options.minimum_events > options.maximum_events)
    return proposals;
  const auto maximum = std::min(options.maximum_events, events.size());
  for (std::size_t reference_count = options.minimum_events;
       reference_count <= maximum; ++reference_count) {
    for (std::size_t observed_count = options.minimum_events;
         observed_count <= maximum; ++observed_count) {
      for (std::size_t reference_begin = 0;
           reference_begin + reference_count <= events.size();
           ++reference_begin) {
        for (std::size_t observed_begin = reference_begin + 1;
             observed_begin + observed_count <= events.size(); ++observed_begin) {
          const auto reference = detail::event_window(
              events, reference_begin, reference_count);
          const auto observed =
              detail::event_window(events, observed_begin, observed_count);
          auto score = score_music_events(reference, observed, "captured events",
                                          options.frame_tolerance);
          if (!score.score || *score.score < options.minimum_score)
            continue;
          proposals.push_back(
              {detail::event_range(events, reference_begin, reference_count),
               detail::event_range(events, observed_begin, observed_count),
               std::move(score),
               false,
               {}});
          auto &proposal = proposals.back();
          proposal.approximate = !proposal.score.score ||
                                 *proposal.score.score < 1.0 ||
                                 reference_count != observed_count;
          proposal.reason = proposal.approximate ? "approximate_repetition"
                                                 : "exact_repetition";
        }
      }
    }
  }
  return proposals;
}

[[nodiscard]] inline std::vector<PatternRelation>
relate_music_patterns(const std::vector<NamedMusicPattern> &patterns,
                      std::uint64_t frame_tolerance) {
  std::vector<PatternRelation> relations;
  for (const auto &smaller : patterns) {
    for (const auto &larger : patterns) {
      if (smaller.id == larger.id || smaller.events.size() >= larger.events.size())
        continue;
      for (std::size_t offset = 0;
           offset + smaller.events.size() <= larger.events.size(); ++offset) {
        if (!detail::identity_window_matches(smaller.events, larger.events, offset,
                                             frame_tolerance))
          continue;
        relations.push_back({smaller.id, larger.id,
                             offset == 0 ? PatternRelationKind::prefix
                                         : PatternRelationKind::contained,
                             offset});
      }
    }
  }
  return relations;
}

} // namespace ayther::engine
