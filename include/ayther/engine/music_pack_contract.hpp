#pragma once

#include <ayther/engine/music_sequence.hpp>

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ayther::engine {

enum class MusicPackContractStatus : std::uint8_t {
  accepted,
  legacy_read_only,
  unsupported_capability,
  invalid_format,
};

struct MusicPackAssignment {
  AssetAssignmentId id{};
  AssetId asset{};
  SampleRegion region{};
};

struct MusicPackContract {
  MusicPackContractStatus status{MusicPackContractStatus::invalid_format};
  std::string profile;
  std::string required_capability;
  std::uint32_t source_rate_hz{};
  std::vector<MusicIdentityId> identities;
  std::vector<SequenceSegment> segments;
  std::vector<MusicPackAssignment> assignments;
};

[[nodiscard]] inline MusicPackContract read_music_pack_contract(
    std::string_view bytes, const std::vector<std::string> &capabilities) {
  MusicPackContract parsed;
  std::istringstream input{std::string{bytes}};
  std::string line;
  if (!std::getline(input, line))
    return parsed;
  const bool current = line == "ayther.music-contract.v1";
  const bool legacy = line == "ayther.music-contract.v0";
  if (!current && !legacy)
    return parsed;

  bool malformed = false;
  bool accepted_behavior = false;
  while (std::getline(input, line)) {
    if (line.empty())
      continue;
    std::istringstream fields{line};
    std::string kind;
    fields >> kind;
    if (kind == "profile") {
      malformed = !(fields >> parsed.profile);
    } else if (kind == "requires") {
      malformed = !(fields >> parsed.required_capability);
    } else if (kind == "source-rate-hz") {
      malformed = !(fields >> parsed.source_rate_hz);
    } else if (kind == "identity") {
      std::uint64_t id{};
      malformed = !(fields >> id) || id == 0;
      if (!malformed)
        parsed.identities.push_back(MusicIdentityId{id});
    } else if (kind == "segment") {
      std::uint64_t id{};
      std::string name;
      malformed = !(fields >> id >> name) || id == 0;
      if (!malformed)
        parsed.segments.push_back({SequenceSegmentId{id}, std::move(name)});
    } else if (kind == "asset") {
      std::uint64_t asset{}, begin{}, end{}, assignment{};
      std::string region_word, assignment_word;
      malformed = !(fields >> asset >> region_word >> begin >> end >>
                    assignment_word >> assignment) ||
                  region_word != "region" ||
                  assignment_word != "assignment" || begin >= end ||
                  asset == 0 || assignment == 0;
      if (!malformed)
        parsed.assignments.push_back({AssetAssignmentId{assignment},
                                      AssetId{asset}, {begin, end}});
    } else if (kind == "node") {
      std::uint64_t node{}, segment{}, assignment{};
      std::string segment_word, assignment_word;
      malformed = !(fields >> node >> segment_word >> segment >>
                    assignment_word >> assignment) ||
                  segment_word != "segment" ||
                  assignment_word != "assignment";
    } else if (kind == "accepted") {
      std::string decision;
      malformed = !(fields >> decision);
      accepted_behavior = !malformed;
    } else if (kind == "name" || kind == "bus" || kind == "entry" ||
               kind == "point" || kind == "edge" ||
               kind == "edge-condition" || kind == "role" ||
               kind == "condition" || kind == "link") {
      std::string value;
      malformed = !(fields >> value);
    } else {
      malformed = true;
    }
    if (malformed)
      return {};
  }

  if (legacy) {
    if (parsed.identities.empty() || parsed.segments.empty() ||
        !parsed.profile.empty() || !parsed.required_capability.empty() ||
        parsed.source_rate_hz != 0 || !parsed.assignments.empty())
      return {};
    parsed.status = MusicPackContractStatus::legacy_read_only;
    return parsed;
  }

  if (parsed.profile.empty() || parsed.required_capability.empty() ||
      parsed.source_rate_hz == 0 || parsed.identities.empty() ||
      parsed.segments.empty() || parsed.assignments.empty() ||
      !accepted_behavior)
    return {};
  if (std::ranges::find(capabilities, parsed.required_capability) ==
      capabilities.end()) {
    MusicPackContract unsupported;
    unsupported.status = MusicPackContractStatus::unsupported_capability;
    unsupported.required_capability = parsed.required_capability;
    return unsupported;
  }
  parsed.status = MusicPackContractStatus::accepted;
  return parsed;
}

} // namespace ayther::engine
