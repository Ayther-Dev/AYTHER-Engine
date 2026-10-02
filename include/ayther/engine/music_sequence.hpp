#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace ayther::engine {

template <class Tag> struct StrongId {
  std::uint64_t value{};
  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return value != 0;
  }
  friend constexpr bool operator==(StrongId, StrongId) noexcept = default;
};

using MusicIdentityId = StrongId<struct MusicIdentityTag>;
using OccurrenceId = StrongId<struct OccurrenceTag>;
using SequenceSegmentId = StrongId<struct SequenceSegmentTag>;
using SequenceNodeId = StrongId<struct SequenceNodeTag>;
using SequenceEdgeId = StrongId<struct SequenceEdgeTag>;
using SequencePointId = StrongId<struct SequencePointTag>;
using AssetAssignmentId = StrongId<struct AssetAssignmentTag>;
using AssetId = StrongId<struct AssetTag>;
using AudioBusId = StrongId<struct AudioBusTag>;
using AppearanceId = StrongId<struct AppearanceTag>;

enum class PointRole : std::uint8_t {
  entry,
  reference,
  boundary,
  loop_begin,
  loop_end,
  exit,
  transition,
};

struct SampleRegion {
  std::uint64_t begin{};
  std::uint64_t end{};

  [[nodiscard]] constexpr bool valid() const noexcept { return begin < end; }
  friend constexpr bool operator==(SampleRegion, SampleRegion) noexcept = default;
};

struct SequenceSegment {
  SequenceSegmentId id{};
  std::string name;
};

struct SequenceNode {
  SequenceNodeId id{};
  SequenceSegmentId segment{};
  AssetAssignmentId assignment{};
};

struct SequenceCondition {
  std::string fact;
  bool expected{};
};

struct SequenceEdge {
  SequenceEdgeId id{};
  SequenceNodeId from{};
  SequenceNodeId to{};
  std::int32_t priority{};
  bool advances_time{true};
  std::vector<SequenceCondition> conditions;

  SequenceEdge() = default;
  SequenceEdge(SequenceEdgeId edge_id, SequenceNodeId source,
               SequenceNodeId destination, std::int32_t edge_priority = 0,
               bool has_temporal_progress = true,
               std::vector<SequenceCondition> edge_conditions = {})
      : id(edge_id), from(source), to(destination), priority(edge_priority),
        advances_time(has_temporal_progress),
        conditions(std::move(edge_conditions)) {}
};

struct SequencePoint {
  SequencePointId id{};
  SequenceNodeId node{};
  std::uint64_t offset{};
  PointRole role{PointRole::reference};
};

struct AssetAssignment {
  AssetAssignmentId id{};
  SequenceSegmentId segment{};
  AssetId asset{};
  SampleRegion region{};
  std::uint64_t required_source_length{};

  AssetAssignment() = default;
  AssetAssignment(AssetAssignmentId assignment_id,
                  SequenceSegmentId segment_id, AssetId asset_id,
                  SampleRegion source_region,
                  std::uint64_t minimum_source_length = 0)
      : id(assignment_id), segment(segment_id), asset(asset_id),
        region(source_region), required_source_length(minimum_source_length) {}
};

struct MusicSequenceDefinition {
  MusicIdentityId identity{};
  std::string name;
  AudioBusId bus{};
  SequenceNodeId entry_node{};
  std::vector<SequenceSegment> segments;
  std::vector<SequenceNode> nodes;
  std::vector<SequenceEdge> edges;
  std::vector<SequencePoint> points;
  std::vector<AssetAssignment> assignments;
};

struct SequencePosition {
  MusicIdentityId identity{};
  OccurrenceId occurrence{};
  SequenceNodeId node{};
  SequenceSegmentId segment{};
  AssetAssignmentId assignment{};
  AppearanceId appearance{};
  std::uint64_t visit{};
  std::uint64_t iteration{};
};

namespace detail {

template <class Range, class Id, class Projection>
[[nodiscard]] bool has_unique_nonzero_ids(const Range &range,
                                          Projection projection) {
  for (auto current = range.begin(); current != range.end(); ++current) {
    const Id id = projection(*current);
    if (!id)
      return false;
    if (std::find_if(std::next(current), range.end(), [&](const auto &value) {
          return projection(value) == id;
        }) != range.end())
      return false;
  }
  return true;
}

} // namespace detail

[[nodiscard]] inline bool
valid_sequence_shape(const MusicSequenceDefinition &definition) {
  const auto segment_exists = [&](const SequenceSegmentId id) {
    return std::ranges::any_of(definition.segments,
                               [&](const auto &value) { return value.id == id; });
  };
  const auto node_exists = [&](const SequenceNodeId id) {
    return std::ranges::any_of(definition.nodes,
                               [&](const auto &value) { return value.id == id; });
  };
  const auto assignment_exists = [&](const AssetAssignmentId id) {
    return std::ranges::any_of(
        definition.assignments,
        [&](const auto &value) { return value.id == id; });
  };

  if (!definition.identity || !definition.bus || !definition.entry_node ||
      definition.segments.empty() || definition.nodes.empty() ||
      !node_exists(definition.entry_node))
    return false;
  if (!detail::has_unique_nonzero_ids<decltype(definition.segments),
                                      SequenceSegmentId>(
          definition.segments, [](const auto &value) { return value.id; }) ||
      !detail::has_unique_nonzero_ids<decltype(definition.nodes),
                                      SequenceNodeId>(
          definition.nodes, [](const auto &value) { return value.id; }) ||
      !detail::has_unique_nonzero_ids<decltype(definition.edges),
                                      SequenceEdgeId>(
          definition.edges, [](const auto &value) { return value.id; }) ||
      !detail::has_unique_nonzero_ids<decltype(definition.points),
                                      SequencePointId>(
          definition.points, [](const auto &value) { return value.id; }) ||
      !detail::has_unique_nonzero_ids<decltype(definition.assignments),
                                      AssetAssignmentId>(
          definition.assignments, [](const auto &value) { return value.id; }))
    return false;

  return std::ranges::all_of(definition.nodes, [&](const auto &node) {
           return segment_exists(node.segment) &&
                  assignment_exists(node.assignment);
         }) &&
         std::ranges::all_of(definition.edges, [&](const auto &edge) {
           return node_exists(edge.from) && node_exists(edge.to);
         }) &&
         std::ranges::all_of(definition.points, [&](const auto &point) {
           return node_exists(point.node);
         }) &&
         std::ranges::all_of(definition.assignments, [&](const auto &value) {
           return segment_exists(value.segment) && value.asset &&
                  value.region.valid();
         });
}

class SequenceTraversal {
public:
  explicit SequenceTraversal(const MusicSequenceDefinition &definition) noexcept
      : definition_(&definition) {}

  [[nodiscard]] bool enter(const OccurrenceId occurrence) {
    if (!occurrence || !valid_sequence_shape(*definition_))
      return false;
    visits_.clear();
    appearance_serial_ = 0;
    position_ = {};
    position_.identity = definition_->identity;
    position_.occurrence = occurrence;
    return enter_node(definition_->entry_node);
  }

  [[nodiscard]] bool transition_to(const SequenceNodeId destination) {
    if (!position_.occurrence)
      return false;
    const bool authored = std::ranges::any_of(
        definition_->edges, [&](const auto &edge) {
          return edge.from == position_.node && edge.to == destination;
        });
    return authored && enter_node(destination);
  }

  [[nodiscard]] bool return_internal_loop() {
    if (!position_.occurrence)
      return false;
    const bool loop_authored = std::ranges::any_of(
        definition_->points, [&](const auto &point) {
          return point.node == position_.node && point.role == PointRole::loop_end;
        });
    if (!loop_authored)
      return false;
    ++position_.iteration;
    return true;
  }

  [[nodiscard]] const SequencePosition &position() const noexcept {
    return position_;
  }

private:
  [[nodiscard]] bool enter_node(const SequenceNodeId node_id) {
    const auto node = std::ranges::find_if(
        definition_->nodes,
        [&](const auto &value) { return value.id == node_id; });
    if (node == definition_->nodes.end())
      return false;

    auto visit = std::ranges::find_if(
        visits_, [&](const auto &value) { return value.first == node_id; });
    if (visit == visits_.end()) {
      visits_.emplace_back(node_id, 1);
      visit = std::prev(visits_.end());
    } else {
      ++visit->second;
    }

    position_.node = node->id;
    position_.segment = node->segment;
    position_.assignment = node->assignment;
    position_.appearance = AppearanceId{++appearance_serial_};
    position_.visit = visit->second;
    position_.iteration = 0;
    return true;
  }

  const MusicSequenceDefinition *definition_;
  SequencePosition position_{};
  std::vector<std::pair<SequenceNodeId, std::uint64_t>> visits_;
  std::uint64_t appearance_serial_{};
};

} // namespace ayther::engine
