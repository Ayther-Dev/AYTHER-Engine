#pragma once

#include <ayther/engine/music_sequence.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace ayther::engine {

struct MusicVoiceOwner {
  MusicIdentityId identity{};
  OccurrenceId occurrence{};
  AudioBusId bus{};
  friend constexpr bool operator==(MusicVoiceOwner,
                                   MusicVoiceOwner) noexcept = default;
};

struct MusicAssetBinding {
  AssetAssignment general;
  std::vector<std::pair<SequenceNodeId, std::optional<AssetAssignment>>>
      individual;
};

enum class MusicAssetAction : std::uint8_t {
  kept_original,
  kept_active,
  started,
  continued,
  invalid_individual,
};

struct MusicAssetResult {
  MusicAssetAction action{MusicAssetAction::kept_original};
  AssetAssignmentId assignment{};
  AssetId asset{};
  SampleRegion region{};
};

class MusicAssetOwner {
public:
  [[nodiscard]] MusicAssetResult enter(const MusicVoiceOwner owner,
                                       const SequenceNodeId node,
                                       const MusicAssetBinding &binding,
                                       const bool entry_safe) {
    if (!entry_safe)
      return {owner_ ? MusicAssetAction::kept_active
                     : MusicAssetAction::kept_original};

    const AssetAssignment *selected = &binding.general;
    for (const auto &[candidate_node, assignment] : binding.individual) {
      if (candidate_node != node)
        continue;
      if (!assignment)
        return {MusicAssetAction::invalid_individual};
      selected = &*assignment;
      break;
    }
    if (!selected->id || !selected->asset || !selected->region.valid())
      return {MusicAssetAction::kept_original};

    const bool continuing = owner_ && *owner_ == owner;
    owner_ = owner;
    assignment_ = selected->id;
    source_cursor_ = selected->region.begin;
    return {continuing ? MusicAssetAction::continued
                       : MusicAssetAction::started,
            selected->id, selected->asset, selected->region};
  }

  void update_source_cursor(const std::uint64_t cursor) noexcept {
    if (owner_)
      source_cursor_ = cursor;
  }

  [[nodiscard]] std::size_t voice_count() const noexcept {
    return owner_ ? 1U : 0U;
  }
  [[nodiscard]] MusicVoiceOwner current_owner() const noexcept {
    return owner_.value_or(MusicVoiceOwner{});
  }
  [[nodiscard]] std::uint64_t source_cursor() const noexcept {
    return source_cursor_;
  }

private:
  std::optional<MusicVoiceOwner> owner_;
  AssetAssignmentId assignment_{};
  std::uint64_t source_cursor_{};
};

} // namespace ayther::engine
