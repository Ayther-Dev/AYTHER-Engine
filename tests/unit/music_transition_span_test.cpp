#include <ayther/engine/music_transition_span.hpp>

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
  const MusicTransitionSpan authored{
      100, 130,    {1000, 1240}, {2000, 2240}, SequenceNodeId{2},
      240, 48'000, false,        false,        false};
  auto result = evaluate_transition_span(authored);
  check(result.valid && result.structural_boundary == 100 &&
            result.audible_end == 130 && result.play_tail && result.play_link &&
            result.replaced_region.begin == 2000 &&
            result.replaced_region.end == 2240,
        "structural boundary is independent from sustained audible tail",
        failures);

  auto included = authored;
  included.transition_included_in_asset = true;
  result = evaluate_transition_span(included);
  check(result.valid && result.play_tail && !result.play_link,
        "transition already included in asset is not duplicated", failures);

  auto foreign = authored;
  foreign.foreign_effect = true;
  result = evaluate_transition_span(foreign);
  check(result.valid && !result.play_tail && result.play_link,
        "foreign effect is not reclassified as musical tail", failures);

  auto excessive = authored;
  excessive.link_duration_frames = 480'001;
  result = evaluate_transition_span(excessive);
  check(!result.valid && result.diagnostic == "link_duration_limit",
        "link longer than ten seconds is rejected", failures);
  excessive.link_duration_frames = 480'000;
  check(evaluate_transition_span(excessive).valid,
        "ten-second link boundary is inclusive", failures);

  auto indefinite = authored;
  indefinite.tail_loops = true;
  result = evaluate_transition_span(indefinite);
  check(!result.valid && result.diagnostic == "indefinite_tail_forbidden",
        "tail without finite end cannot loop indefinitely", failures);
  auto missing_destination = authored;
  missing_destination.destination = {};
  check(!evaluate_transition_span(missing_destination).valid,
        "every link has an explicit destination", failures);
  return failures == 0 ? 0 : 1;
}
