#include <ayther/engine/music_causal_recognizer.hpp>

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
  MusicSequenceDefinition sequence;
  sequence.identity = MusicIdentityId{7};
  sequence.bus = AudioBusId{1};
  sequence.entry_node = SequenceNodeId{1};
  sequence.segments = {{SequenceSegmentId{1}, "a"},
                       {SequenceSegmentId{2}, "b"}};
  sequence.nodes = {{SequenceNodeId{1}, SequenceSegmentId{1},
                     AssetAssignmentId{1}},
                    {SequenceNodeId{2}, SequenceSegmentId{2},
                     AssetAssignmentId{2}}};
  sequence.edges = {{SequenceEdgeId{1}, SequenceNodeId{1}, SequenceNodeId{2}}};
  sequence.assignments = {
      {AssetAssignmentId{1}, SequenceSegmentId{1}, AssetId{1}, {0, 10}},
      {AssetAssignmentId{2}, SequenceSegmentId{2}, AssetId{2}, {0, 10}}};

  CausalMusicRecognizer recognizer{sequence};
  check(recognizer.observe({100, MusicSignalRole::entry_trigger, {}, false,
                            "screen-title"}) ==
            MusicRecognitionAction::entered &&
            recognizer.state().position().occurrence == OccurrenceId{1},
        "entry trigger starts the inactive identity", failures);
  const auto first_appearance = recognizer.state().position().appearance;
  check(recognizer.observe({100, MusicSignalRole::entry_trigger, {}, false,
                            "screen-menu"}) ==
            MusicRecognitionAction::continuity_evidence &&
            recognizer.state().position().occurrence == OccurrenceId{1} &&
            recognizer.state().position().appearance == first_appearance,
        "shared entry signature does not rearm Intro or reclaim the bus",
        failures);
  check(recognizer.observe({200, MusicSignalRole::reference, {}, false,
                            "passive-anchor"}) ==
            MusicRecognitionAction::reference_observed &&
            recognizer.state().position().node == SequenceNodeId{1},
        "passive reference never moves traversal position", failures);
  check(recognizer.observe({300, MusicSignalRole::transition,
                            SequenceNodeId{2}, false, "loop"}) ==
            MusicRecognitionAction::transition_rejected &&
            recognizer.state().position().node == SequenceNodeId{1},
        "transition signal without a declared condition is rejected", failures);
  check(recognizer.observe({300, MusicSignalRole::transition,
                            SequenceNodeId{2}, true, "loop"}) ==
            MusicRecognitionAction::transitioned &&
            recognizer.state().position().node == SequenceNodeId{2},
        "transition signal moves only to an authored successor", failures);
  check(recognizer.observe({400, MusicSignalRole::evidence, {}, false,
                            "Game Over"}) ==
            MusicRecognitionAction::evidence_observed &&
            recognizer.state().active(),
        "Game Over name alone is not a terminal condition", failures);
  check(recognizer.observe({401, MusicSignalRole::terminal, {}, false,
                            "Stage 01"}) ==
            MusicRecognitionAction::terminal_rejected &&
            recognizer.state().active(),
        "terminal role without declared condition is rejected", failures);
  check(recognizer.observe({402, MusicSignalRole::terminal, {}, true,
                            "unrelated-label"}) ==
            MusicRecognitionAction::finished && !recognizer.state().active(),
        "declared terminal condition ends independently of display name",
        failures);
  return failures == 0 ? 0 : 1;
}
