#include <ayther/engine/music_sequence.hpp>

#include <cstdio>

namespace {

void check(const bool condition, const char *message, int &failures) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

} // namespace

int main() {
  using namespace ayther::engine;
  int failures = 0;

  MusicSequenceDefinition battle;
  battle.identity = MusicIdentityId{0xBA771E};
  battle.bus = AudioBusId{1};
  battle.entry_node = SequenceNodeId{10};
  battle.segments = {{SequenceSegmentId{100}, "Intro"},
                     {SequenceSegmentId{200}, "Loop"}};
  battle.nodes = {
      {SequenceNodeId{10}, SequenceSegmentId{100}, AssetAssignmentId{1000}},
      {SequenceNodeId{20}, SequenceSegmentId{200}, AssetAssignmentId{2000}},
  };
  battle.edges = {
      {SequenceEdgeId{1}, SequenceNodeId{10}, SequenceNodeId{20}},
      {SequenceEdgeId{2}, SequenceNodeId{20}, SequenceNodeId{20}},
      {SequenceEdgeId{3}, SequenceNodeId{20}, SequenceNodeId{10}},
  };
  battle.points = {
      {SequencePointId{1}, SequenceNodeId{10}, 0, PointRole::entry},
      {SequencePointId{2}, SequenceNodeId{10}, 575, PointRole::boundary},
      {SequencePointId{3}, SequenceNodeId{20}, 0, PointRole::entry},
      {SequencePointId{4}, SequenceNodeId{20}, 576, PointRole::loop_end},
  };
  battle.assignments = {
      {AssetAssignmentId{1000}, SequenceSegmentId{100}, AssetId{700},
       SampleRegion{0, 574}},
      {AssetAssignmentId{2000}, SequenceSegmentId{200}, AssetId{701},
       SampleRegion{0, 577}},
  };

  check(valid_sequence_shape(battle), "The Battle graph is structurally valid",
        failures);

  SequenceTraversal traversal{battle};
  check(traversal.enter(OccurrenceId{42}), "entry node opens an occurrence",
        failures);
  const auto intro = traversal.position();
  check(intro.identity == battle.identity &&
            intro.occurrence == OccurrenceId{42},
        "identity and occurrence are independent from the asset", failures);
  check(intro.node == SequenceNodeId{10} && intro.appearance.value == 1 &&
            intro.visit == 1 && intro.iteration == 0,
        "entry creates the first Intro appearance", failures);

  check(traversal.transition_to(SequenceNodeId{20}),
        "Intro transitions to Loop", failures);
  const auto loop = traversal.position();
  check(loop.appearance.value == 2 && loop.visit == 1 && loop.iteration == 0,
        "transition creates a Loop appearance", failures);

  check(traversal.return_internal_loop(), "Loop can return internally",
        failures);
  const auto repeated = traversal.position();
  check(repeated.appearance == loop.appearance &&
            repeated.visit == loop.visit && repeated.iteration == 1,
        "internal loop preserves appearance and increments iteration",
        failures);

  check(traversal.transition_to(SequenceNodeId{20}),
        "self transition is distinct from an internal loop", failures);
  const auto revisited = traversal.position();
  check(revisited.appearance.value == 3 && revisited.visit == 2 &&
            revisited.iteration == 0,
        "self transition creates an appearance and increments visits",
        failures);

  check(traversal.transition_to(SequenceNodeId{10}),
        "a recurrent Intro is a valid authored transition", failures);
  const auto recurrent_intro = traversal.position();
  check(recurrent_intro.identity == battle.identity &&
            recurrent_intro.occurrence == OccurrenceId{42} &&
            recurrent_intro.visit == 2,
        "recurrent Intro keeps musical identity and occurrence", failures);

  return failures == 0 ? 0 : 1;
}
