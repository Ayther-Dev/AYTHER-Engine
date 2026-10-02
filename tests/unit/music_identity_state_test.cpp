#include <ayther/engine/music_identity_state.hpp>

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
  sequence.segments = {{SequenceSegmentId{1}, "intro"},
                       {SequenceSegmentId{2}, "loop"}};
  sequence.nodes = {
      {SequenceNodeId{1}, SequenceSegmentId{1}, AssetAssignmentId{1}},
      {SequenceNodeId{2}, SequenceSegmentId{2}, AssetAssignmentId{2}}};
  sequence.edges = {{SequenceEdgeId{1}, SequenceNodeId{1}, SequenceNodeId{2}},
                    {SequenceEdgeId{2}, SequenceNodeId{2}, SequenceNodeId{1}},
                    {SequenceEdgeId{3}, SequenceNodeId{1}, SequenceNodeId{1}}};
  sequence.points = {
      {SequencePointId{1}, SequenceNodeId{2}, 20, PointRole::loop_end}};
  sequence.assignments = {
      {AssetAssignmentId{1}, SequenceSegmentId{1}, AssetId{1}, {0, 10}},
      {AssetAssignmentId{2}, SequenceSegmentId{2}, AssetId{2}, {0, 10}}};

  MusicIdentityState state{sequence};
  check(state.enter(OccurrenceId{40}, IdentityStateCause::entry_trigger) &&
            state.position().identity == MusicIdentityId{7} &&
            state.position().occurrence == OccurrenceId{40} &&
            state.position().node == SequenceNodeId{1} &&
            state.position().visit == 1 && state.position().iteration == 0,
        "initial entry creates identity occurrence and first appearance",
        failures);
  const auto intro_appearance = state.position().appearance;
  check(state.transition(SequenceNodeId{1},
                         IdentityStateCause::authored_transition) &&
            state.position().appearance != intro_appearance &&
            state.position().visit == 2 && state.position().iteration == 0,
        "transition to same node creates a new appearance and visit", failures);
  check(state.transition(SequenceNodeId{2},
                         IdentityStateCause::authored_transition),
        "authored transition enters loop node", failures);
  const auto loop_appearance = state.position().appearance;
  check(state.internal_loop(IdentityStateCause::internal_loop_return) &&
            state.position().appearance == loop_appearance &&
            state.position().iteration == 1,
        "internal loop preserves appearance and increments only iteration",
        failures);
  check(state.transition(SequenceNodeId{1},
                         IdentityStateCause::authored_transition) &&
            state.position().visit == 3,
        "authored return to Intro preserves occurrence and increments visits",
        failures);
  state.finish(IdentityStateCause::declared_terminal);
  check(!state.active() &&
            state.last_cause() == IdentityStateCause::declared_terminal,
        "declared terminal ends the occurrence with a cause", failures);
  check(state.restart(OccurrenceId{41}, IdentityStateCause::explicit_restart) &&
            state.position().occurrence == OccurrenceId{41} &&
            state.position().visit == 1,
        "explicit restart creates a new occurrence and traversal", failures);
  return failures == 0 ? 0 : 1;
}
