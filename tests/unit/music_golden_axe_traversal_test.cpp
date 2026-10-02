#include <ayther/engine/music_causal_recognizer.hpp>

#include <cstdio>
#include <string>

namespace {
void check(bool value, const char *message, int &failures) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

ayther::engine::MusicSequenceDefinition battle_fixture(std::string name) {
  using namespace ayther::engine;
  MusicSequenceDefinition sequence;
  sequence.identity = MusicIdentityId{1};
  sequence.name = std::move(name);
  sequence.bus = AudioBusId{1};
  sequence.entry_node = SequenceNodeId{1};
  sequence.segments = {{SequenceSegmentId{1}, "intro"},
                       {SequenceSegmentId{2}, "loop"}};
  sequence.nodes = {{SequenceNodeId{1}, SequenceSegmentId{1},
                     AssetAssignmentId{1}},
                    {SequenceNodeId{2}, SequenceSegmentId{2},
                     AssetAssignmentId{2}}};
  sequence.edges = {{SequenceEdgeId{1}, SequenceNodeId{1}, SequenceNodeId{2}}};
  sequence.points = {{SequencePointId{1}, SequenceNodeId{2}, 16,
                      PointRole::loop_end}};
  sequence.assignments = {
      {AssetAssignmentId{1}, SequenceSegmentId{1}, AssetId{1}, {0, 16}},
      {AssetAssignmentId{2}, SequenceSegmentId{2}, AssetId{2}, {16, 48}}};
  return sequence;
}

ayther::engine::MusicSequenceDefinition wilderness_fixture(std::string name) {
  using namespace ayther::engine;
  MusicSequenceDefinition sequence;
  sequence.identity = MusicIdentityId{2};
  sequence.name = std::move(name);
  sequence.bus = AudioBusId{1};
  sequence.entry_node = SequenceNodeId{10};
  sequence.segments = {{SequenceSegmentId{10}, "intro"},
                       {SequenceSegmentId{11}, "loop-1"},
                       {SequenceSegmentId{12}, "loop-2"}};
  sequence.nodes = {{SequenceNodeId{10}, SequenceSegmentId{10},
                     AssetAssignmentId{10}},
                    {SequenceNodeId{11}, SequenceSegmentId{11},
                     AssetAssignmentId{11}},
                    {SequenceNodeId{12}, SequenceSegmentId{12},
                     AssetAssignmentId{12}}};
  sequence.edges = {
      {SequenceEdgeId{10}, SequenceNodeId{10}, SequenceNodeId{11}},
      {SequenceEdgeId{11}, SequenceNodeId{11}, SequenceNodeId{10}},
      {SequenceEdgeId{12}, SequenceNodeId{10}, SequenceNodeId{12}}};
  sequence.points = {
      {SequencePointId{10}, SequenceNodeId{11}, 20, PointRole::loop_end},
      {SequencePointId{11}, SequenceNodeId{12}, 20, PointRole::loop_end}};
  sequence.assignments = {
      {AssetAssignmentId{10}, SequenceSegmentId{10}, AssetId{10}, {0, 10}},
      {AssetAssignmentId{11}, SequenceSegmentId{11}, AssetId{11}, {0, 20}},
      {AssetAssignmentId{12}, SequenceSegmentId{12}, AssetId{12}, {0, 20}}};
  return sequence;
}
} // namespace

int main() {
  using namespace ayther::engine;
  int failures = 0;

  const auto battle = battle_fixture("The Battle");
  CausalMusicRecognizer battle_recognizer{battle};
  check(battle_recognizer.observe({7, MusicSignalRole::entry_trigger, {}, false,
                                   "shared"}) ==
            MusicRecognitionAction::entered,
        "The Battle enters Intro only while inactive", failures);
  const auto battle_occurrence = battle_recognizer.state().position().occurrence;
  const auto intro_appearance = battle_recognizer.state().position().appearance;
  check(battle_recognizer.observe({7, MusicSignalRole::entry_trigger, {}, false,
                                   "shared"}) ==
                MusicRecognitionAction::continuity_evidence &&
            battle_recognizer.state().position().appearance == intro_appearance,
        "shared title/menu signature does not rearm Intro", failures);
  check(battle_recognizer.observe(
            {8, MusicSignalRole::transition, SequenceNodeId{2}, true,
             "declared-boundary"}) ==
            MusicRecognitionAction::transitioned,
        "declared boundary advances Intro to Loop", failures);
  const auto loop_appearance = battle_recognizer.state().position().appearance;
  check(battle_recognizer.internal_loop() &&
            battle_recognizer.state().position().appearance == loop_appearance &&
            battle_recognizer.state().position().iteration == 1 &&
            battle_recognizer.state().position().occurrence == battle_occurrence,
        "The Battle loop return preserves occurrence and appearance", failures);

  const auto wilderness = wilderness_fixture("Wilderness");
  CausalMusicRecognizer wilderness_recognizer{wilderness};
  check(wilderness_recognizer.observe(
            {70, MusicSignalRole::entry_trigger, {}, false, "entry"}) ==
            MusicRecognitionAction::entered,
        "Wilderness enters Intro", failures);
  const auto wilderness_occurrence =
      wilderness_recognizer.state().position().occurrence;
  check(wilderness_recognizer.observe(
            {71, MusicSignalRole::transition, SequenceNodeId{11}, true,
             "to-loop-1"}) ==
            MusicRecognitionAction::transitioned,
        "Wilderness advances to Loop 1", failures);
  for (int repetition = 0; repetition < 3; ++repetition)
    check(wilderness_recognizer.internal_loop(),
          "Wilderness accepts variable authored Loop 1 repetitions", failures);
  check(wilderness_recognizer.state().position().iteration == 3,
        "Loop 1 records each internal repetition", failures);
  check(wilderness_recognizer.observe(
            {72, MusicSignalRole::transition, SequenceNodeId{10}, true,
             "to-intro"}) ==
                MusicRecognitionAction::transitioned &&
            wilderness_recognizer.state().position().visit == 2 &&
            wilderness_recognizer.state().position().occurrence ==
                wilderness_occurrence,
        "recurrent Intro creates a visit but preserves occurrence", failures);
  const auto recurrent_intro =
      wilderness_recognizer.state().position().appearance;
  check(wilderness_recognizer.observe(
            {70, MusicSignalRole::evidence, {}, false, "shared"}) ==
                MusicRecognitionAction::evidence_observed &&
            wilderness_recognizer.state().position().appearance ==
                recurrent_intro,
        "shared evidence with insufficient context does not move traversal",
        failures);
  check(wilderness_recognizer.observe(
            {73, MusicSignalRole::transition, SequenceNodeId{12}, true,
             "to-loop-2"}) ==
            MusicRecognitionAction::transitioned,
        "authored context selects Loop 2", failures);
  check(wilderness_recognizer.internal_loop() &&
            wilderness_recognizer.state().position().iteration == 1,
        "Loop 2 has its own repetition counter", failures);

  const auto renamed = battle_fixture("arbitrary-fixture-name");
  CausalMusicRecognizer renamed_recognizer{renamed};
  check(renamed_recognizer.observe(
            {7, MusicSignalRole::entry_trigger, {}, false, "renamed-entry"}) ==
                MusicRecognitionAction::entered &&
            renamed_recognizer.observe(
                {8, MusicSignalRole::transition, SequenceNodeId{2}, true,
                 "renamed-transition"}) ==
                MusicRecognitionAction::transitioned,
        "behavior is independent of sequence and signal names", failures);

  return failures == 0 ? 0 : 1;
}
