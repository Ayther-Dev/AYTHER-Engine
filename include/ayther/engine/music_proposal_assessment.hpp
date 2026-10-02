#pragma once

#include <ayther/engine/music_event_scoring.hpp>
#include <ayther/engine/music_sequence.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ayther::engine {

enum class BoundaryCertainty : std::uint8_t {
  indeterminate,
  partial,
  confirmed,
};

enum class PositionCertainty : std::uint8_t {
  unresolved,
  ambiguous,
  resolved,
};

enum class EventMembership : std::uint8_t {
  uncertain,
  independent,
  musical,
};

struct ProposalEvidence {
  double music_score{};
  std::size_t match_count{};
  std::size_t distinct_starts{};
  bool start_anchor{};
  bool end_anchor{};
  bool unique_boundaries{};
  std::vector<AppearanceId> eligible_appearances;
  std::optional<AppearanceId> author_choice;
  std::int32_t transition_priority{};
  bool tail_end_observed{};
  bool full_loop_observed{};
  EventMembership crossing_event_membership{EventMembership::uncertain};
};

struct MusicProposalAssessment {
  MusicScoreCategory music_category{MusicScoreCategory::no_pattern};
  bool insufficient_evidence{true};
  BoundaryCertainty boundaries{BoundaryCertainty::indeterminate};
  PositionCertainty position{PositionCertainty::unresolved};
  std::vector<AppearanceId> alternatives;
  std::vector<std::string> missing_discriminants;
  bool tail_open{};
  bool loop_demonstrated{};
  bool author_selected{};
  bool automatically_accepted{};
  EventMembership crossing_event_membership{EventMembership::uncertain};
};

[[nodiscard]] inline MusicProposalAssessment
assess_music_proposal(const ProposalEvidence &evidence) {
  MusicProposalAssessment result;
  result.music_category = classify_music_score(evidence.music_score);
  result.insufficient_evidence =
      evidence.match_count < 4 || evidence.distinct_starts < 2;
  if (evidence.start_anchor && evidence.end_anchor &&
      evidence.unique_boundaries)
    result.boundaries = BoundaryCertainty::confirmed;
  else if (evidence.start_anchor || evidence.end_anchor)
    result.boundaries = BoundaryCertainty::partial;

  result.alternatives = evidence.eligible_appearances;
  if (evidence.eligible_appearances.size() == 1)
    result.position = PositionCertainty::resolved;
  else if (evidence.eligible_appearances.size() > 1) {
    result.position = PositionCertainty::ambiguous;
    result.missing_discriminants.emplace_back("appearance_context");
  } else {
    result.missing_discriminants.emplace_back("eligible_appearance");
  }
  if (result.boundaries != BoundaryCertainty::confirmed)
    result.missing_discriminants.emplace_back("boundary_anchor");
  if (result.insufficient_evidence)
    result.missing_discriminants.emplace_back("minimum_approximate_evidence");

  result.tail_open = !evidence.tail_end_observed;
  result.loop_demonstrated = evidence.full_loop_observed;
  result.author_selected = evidence.author_choice.has_value();
  result.crossing_event_membership = evidence.crossing_event_membership;
  // Scores, priorities and choices are evidence or policy inputs. Acceptance is
  // an explicit authoring operation outside this pure assessment.
  result.automatically_accepted = false;
  return result;
}

} // namespace ayther::engine
