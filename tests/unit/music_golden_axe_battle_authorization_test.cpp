#include <ayther/engine/music_causal_recognizer.hpp>

#include <cstdio>

namespace {
constexpr std::uint64_t battle_shared_signature = 0x93031940bd4e23eeULL;
constexpr std::uint64_t battle_loop_discriminant = 0x97e5f21f928c5858ULL;

void check(bool value, const char *message, int &failures) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

ayther::engine::MusicSequenceDefinition battle_definition() {
  using namespace ayther::engine;
  MusicSequenceDefinition sequence;
  sequence.identity = MusicIdentityId{0x0e};
  sequence.name = "The Battle";
  sequence.bus = AudioBusId{1};
  sequence.entry_node = SequenceNodeId{1};
  sequence.segments = {{SequenceSegmentId{1}, "Intro"},
                       {SequenceSegmentId{2}, "Loop"}};
  sequence.nodes = {
      {SequenceNodeId{1}, SequenceSegmentId{1}, AssetAssignmentId{1}},
      {SequenceNodeId{2}, SequenceSegmentId{2}, AssetAssignmentId{2}}};
  sequence.edges = {{SequenceEdgeId{1}, SequenceNodeId{1}, SequenceNodeId{2}}};
  sequence.points = {
      {SequencePointId{1}, SequenceNodeId{2}, 16, PointRole::loop_end}};
  sequence.assignments = {
      {AssetAssignmentId{1}, SequenceSegmentId{1}, AssetId{1}, {0, 16}},
      {AssetAssignmentId{2}, SequenceSegmentId{2}, AssetId{2}, {16, 48}}};
  return sequence;
}
} // namespace

int main() {
  using namespace ayther::engine;
  int failures = 0;
  const auto battle = battle_definition();
  CausalMusicRecognizer recognizer{battle};

  check(recognizer.observe({battle_shared_signature,
                            MusicSignalRole::entry_trigger,
                            {},
                            true,
                            "title-entry"}) == MusicRecognitionAction::entered,
        "the shared signature starts The Battle only while inactive", failures);
  const auto occurrence = recognizer.state().position().occurrence;
  const auto intro_appearance = recognizer.state().position().appearance;
  const auto intro_visit = recognizer.state().position().visit;

  for (const char *control : {"title", "menu", "selection"})
    check(recognizer.observe({battle_shared_signature,
                              MusicSignalRole::entry_trigger,
                              {},
                              false,
                              control}) ==
                  MusicRecognitionAction::continuity_evidence &&
              recognizer.state().position().occurrence == occurrence &&
              recognizer.state().position().appearance == intro_appearance &&
              recognizer.state().position().visit == intro_visit,
          "title/menu/selection controls do not rearm Intro", failures);

  check(recognizer.observe(
            {0x1234, MusicSignalRole::evidence, {}, false, "sound-effect"}) ==
                MusicRecognitionAction::evidence_observed &&
            recognizer.state().position().appearance == intro_appearance,
        "effects preserve the authored traversal", failures);

  check(
      recognizer.observe({battle_loop_discriminant, MusicSignalRole::transition,
                          SequenceNodeId{2}, false, "unverified-loop"}) ==
              MusicRecognitionAction::transition_rejected &&
          recognizer.state().position().appearance == intro_appearance,
      "the Loop discriminant cannot be presumed", failures);
  check(
      recognizer.observe({battle_loop_discriminant, MusicSignalRole::transition,
                          SequenceNodeId{2}, true, "verified-loop"}) ==
          MusicRecognitionAction::transitioned,
      "the verified discriminant advances Intro to Loop", failures);

  const auto loop_appearance = recognizer.state().position().appearance;
  check(recognizer.observe({battle_shared_signature,
                            MusicSignalRole::entry_trigger,
                            {},
                            false,
                            "shared-after-loop"}) ==
                MusicRecognitionAction::continuity_evidence &&
            recognizer.state().position().appearance == loop_appearance &&
            recognizer.state().position().occurrence == occurrence,
        "the shared signature cannot rearm Intro from Loop", failures);
  check(recognizer.internal_loop() &&
            recognizer.state().position().appearance == loop_appearance &&
            recognizer.state().position().occurrence == occurrence &&
            recognizer.state().position().iteration == 1,
        "the internal Loop preserves identity occurrence and appearance",
        failures);

  return failures == 0 ? 0 : 1;
}
