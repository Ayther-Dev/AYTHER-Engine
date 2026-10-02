#include <ayther/engine/music_sequence_validation.hpp>

#include <cstdio>

namespace {
void check(bool value, const char *message, int &failures) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

ayther::engine::MusicSequenceDefinition valid_definition() {
  using namespace ayther::engine;
  MusicSequenceDefinition value;
  value.identity = MusicIdentityId{1};
  value.bus = AudioBusId{1};
  value.entry_node = SequenceNodeId{1};
  value.segments = {{SequenceSegmentId{1}, "loop"}};
  value.nodes = {
      {SequenceNodeId{1}, SequenceSegmentId{1}, AssetAssignmentId{1}}};
  value.edges = {{SequenceEdgeId{1},
                  SequenceNodeId{1},
                  SequenceNodeId{1},
                  0,
                  true,
                  {{"playing", true}}}};
  value.points = {
      {SequencePointId{1}, SequenceNodeId{1}, 0, PointRole::loop_begin},
      {SequencePointId{2}, SequenceNodeId{1}, 100, PointRole::loop_end}};
  value.assignments = {
      {AssetAssignmentId{1}, SequenceSegmentId{1}, AssetId{1}, {0, 100}}};
  value.assignments[0].required_source_length = 100;
  return value;
}

bool contains(const ayther::engine::SequenceValidationResult &result,
              ayther::engine::SequenceDiagnosticCode code) {
  for (const auto &diagnostic : result.diagnostics)
    if (diagnostic.code == code && !diagnostic.location.empty())
      return true;
  return false;
}
} // namespace

int main() {
  using namespace ayther::engine;
  int failures = 0;
  const SequenceAssetCatalog assets{{AssetId{1}, 100}};

  auto definition = valid_definition();
  check(validate_sequence(definition, assets).valid(),
        "an intentional loop with temporal progress is valid", failures);

  definition.assignments[0].asset = AssetId{9};
  check(contains(validate_sequence(definition, assets),
                 SequenceDiagnosticCode::missing_asset),
        "a missing asset is diagnosed locally", failures);

  definition = valid_definition();
  definition.assignments[0].region.end = 101;
  check(contains(validate_sequence(definition, assets),
                 SequenceDiagnosticCode::region_out_of_range),
        "an out-of-source region is rejected", failures);

  definition = valid_definition();
  definition.edges[0].to = SequenceNodeId{99};
  check(contains(validate_sequence(definition, assets),
                 SequenceDiagnosticCode::missing_edge_destination),
        "an edge without a destination is rejected", failures);

  definition = valid_definition();
  definition.points[0].node = SequenceNodeId{99};
  check(contains(validate_sequence(definition, assets),
                 SequenceDiagnosticCode::dangling_reference),
        "a missing referenced node is diagnosed at its field", failures);

  definition = valid_definition();
  definition.edges[0].conditions.push_back({"playing", false});
  check(contains(validate_sequence(definition, assets),
                 SequenceDiagnosticCode::contradictory_condition),
        "contradictory conditions are rejected", failures);

  definition = valid_definition();
  definition.edges[0].advances_time = false;
  check(contains(validate_sequence(definition, assets),
                 SequenceDiagnosticCode::no_progress_cycle),
        "a cycle without temporal progress is rejected", failures);

  definition = valid_definition();
  const SequenceAssetCatalog short_assets{{AssetId{1}, 99}};
  check(contains(validate_sequence(definition, short_assets),
                 SequenceDiagnosticCode::source_too_short),
        "a known fixed-duration deficit is rejected", failures);

  return failures == 0 ? 0 : 1;
}
