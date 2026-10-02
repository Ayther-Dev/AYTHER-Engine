#include <ayther/engine/music_proposal_assessment.hpp>

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

  ProposalEvidence ambiguous;
  ambiguous.music_score = 0.96;
  ambiguous.match_count = 8;
  ambiguous.distinct_starts = 4;
  ambiguous.start_anchor = true;
  ambiguous.end_anchor = true;
  ambiguous.unique_boundaries = true;
  ambiguous.eligible_appearances = {AppearanceId{1}, AppearanceId{2}};
  ambiguous.transition_priority = 99;
  ambiguous.tail_end_observed = true;
  ambiguous.full_loop_observed = true;
  const auto ambiguity = assess_music_proposal(ambiguous);
  check(ambiguity.music_category == MusicScoreCategory::high &&
            ambiguity.boundaries == BoundaryCertainty::confirmed &&
            ambiguity.position == PositionCertainty::ambiguous &&
            ambiguity.alternatives.size() == 2 &&
            !ambiguity.automatically_accepted,
        "high score and priority do not resolve two eligible appearances",
        failures);
  check(!ambiguity.missing_discriminants.empty(),
        "ambiguity explains the missing discriminant", failures);

  ProposalEvidence partial = ambiguous;
  partial.end_anchor = false;
  partial.tail_end_observed = false;
  partial.full_loop_observed = false;
  partial.crossing_event_membership = EventMembership::independent;
  partial.eligible_appearances = {AppearanceId{3}};
  const auto open = assess_music_proposal(partial);
  check(open.boundaries == BoundaryCertainty::partial && open.tail_open &&
            !open.loop_demonstrated &&
            open.position == PositionCertainty::resolved &&
            open.crossing_event_membership == EventMembership::independent,
        "one anchor gives partial limits and keeps an unobserved tail open",
        failures);

  ProposalEvidence unknown = partial;
  unknown.start_anchor = false;
  unknown.eligible_appearances.clear();
  const auto indeterminate = assess_music_proposal(unknown);
  check(indeterminate.boundaries == BoundaryCertainty::indeterminate &&
            indeterminate.position == PositionCertainty::unresolved,
        "no anchors or eligible appearance stays indeterminate", failures);

  ProposalEvidence chosen = ambiguous;
  chosen.author_choice = AppearanceId{2};
  const auto authored = assess_music_proposal(chosen);
  check(authored.position == PositionCertainty::ambiguous &&
            authored.author_selected && !authored.automatically_accepted,
        "author choice is recorded without pretending identification",
        failures);

  ProposalEvidence insufficient = partial;
  insufficient.music_score = 1.0;
  insufficient.match_count = 3;
  insufficient.distinct_starts = 2;
  const auto short_pattern = assess_music_proposal(insufficient);
  check(short_pattern.insufficient_evidence &&
            short_pattern.music_category == MusicScoreCategory::high,
        "musical category and evidence sufficiency remain independent",
        failures);

  return failures == 0 ? 0 : 1;
}
