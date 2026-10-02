#pragma once

#include <ayther/engine/music_sequence.hpp>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ayther::engine {

using SequenceAssetCatalog = std::vector<std::pair<AssetId, std::uint64_t>>;

enum class SequenceDiagnosticCode : std::uint8_t {
  invalid_shape,
  missing_asset,
  region_out_of_range,
  source_too_short,
  dangling_reference,
  missing_edge_destination,
  contradictory_condition,
  no_progress_cycle,
};

struct SequenceDiagnostic {
  SequenceDiagnosticCode code{};
  std::string location;
};

struct SequenceValidationResult {
  std::vector<SequenceDiagnostic> diagnostics;
  [[nodiscard]] bool valid() const noexcept { return diagnostics.empty(); }
};

namespace detail {
inline void diagnose(SequenceValidationResult &result,
                     SequenceDiagnosticCode code, std::string location) {
  result.diagnostics.push_back({code, std::move(location)});
}

[[nodiscard]] inline bool has_no_progress_cycle_from(
    const MusicSequenceDefinition &definition, SequenceNodeId node,
    std::vector<SequenceNodeId> &visiting, std::vector<SequenceNodeId> &done) {
  if (std::ranges::find(done, node) != done.end())
    return false;
  if (std::ranges::find(visiting, node) != visiting.end())
    return true;
  visiting.push_back(node);
  for (const auto &edge : definition.edges)
    if (edge.from == node && !edge.advances_time &&
        has_no_progress_cycle_from(definition, edge.to, visiting, done))
      return true;
  visiting.pop_back();
  done.push_back(node);
  return false;
}
} // namespace detail

[[nodiscard]] inline SequenceValidationResult
validate_sequence(const MusicSequenceDefinition &definition,
                  const SequenceAssetCatalog &assets) {
  SequenceValidationResult result;
  const auto node_exists = [&](SequenceNodeId id) {
    return std::ranges::any_of(definition.nodes,
                               [&](const auto &node) { return node.id == id; });
  };
  const auto segment_exists = [&](SequenceSegmentId id) {
    return std::ranges::any_of(
        definition.segments,
        [&](const auto &segment) { return segment.id == id; });
  };
  const auto assignment_exists = [&](AssetAssignmentId id) {
    return std::ranges::any_of(
        definition.assignments,
        [&](const auto &assignment) { return assignment.id == id; });
  };
  if (!valid_sequence_shape(definition))
    detail::diagnose(result, SequenceDiagnosticCode::invalid_shape, "sequence");

  for (std::size_t index = 0; index < definition.nodes.size(); ++index) {
    const auto &node = definition.nodes[index];
    const std::string location = "nodes[" + std::to_string(index) + "]";
    if (!segment_exists(node.segment))
      detail::diagnose(result, SequenceDiagnosticCode::dangling_reference,
                       location + ".segment");
    if (!assignment_exists(node.assignment))
      detail::diagnose(result, SequenceDiagnosticCode::dangling_reference,
                       location + ".assignment");
  }
  for (std::size_t index = 0; index < definition.points.size(); ++index)
    if (!node_exists(definition.points[index].node))
      detail::diagnose(result, SequenceDiagnosticCode::dangling_reference,
                       "points[" + std::to_string(index) + "].node");

  for (std::size_t index = 0; index < definition.assignments.size(); ++index) {
    const auto &assignment = definition.assignments[index];
    const auto source = std::ranges::find_if(
        assets, [&](const auto &asset) { return asset.first == assignment.asset; });
    const std::string location = "assignments[" + std::to_string(index) + "]";
    if (source == assets.end()) {
      detail::diagnose(result, SequenceDiagnosticCode::missing_asset, location);
      continue;
    }
    if (assignment.required_source_length != 0 &&
        source->second < assignment.required_source_length)
      detail::diagnose(result, SequenceDiagnosticCode::source_too_short,
                       location + ".required_source_length");
    if (assignment.region.end > source->second)
      detail::diagnose(result, SequenceDiagnosticCode::region_out_of_range,
                       location + ".region");
  }

  for (std::size_t index = 0; index < definition.edges.size(); ++index) {
    const auto &edge = definition.edges[index];
    const std::string location = "edges[" + std::to_string(index) + "]";
    if (!node_exists(edge.to))
      detail::diagnose(result,
                       SequenceDiagnosticCode::missing_edge_destination,
                       location + ".to");
    for (auto condition = edge.conditions.begin();
         condition != edge.conditions.end(); ++condition) {
      if (condition->fact.empty() ||
          std::ranges::any_of(std::next(condition), edge.conditions.end(),
                              [&](const auto &other) {
                                return other.fact == condition->fact &&
                                       other.expected != condition->expected;
                              })) {
        detail::diagnose(result,
                         SequenceDiagnosticCode::contradictory_condition,
                         location + ".conditions");
        break;
      }
    }
  }

  std::vector<SequenceNodeId> visiting;
  std::vector<SequenceNodeId> done;
  for (const auto &node : definition.nodes)
    if (detail::has_no_progress_cycle_from(definition, node.id, visiting, done)) {
      detail::diagnose(result, SequenceDiagnosticCode::no_progress_cycle,
                       "edges");
      break;
    }
  return result;
}

} // namespace ayther::engine
