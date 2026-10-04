// Builds the per-frame render observation (spec 002, contracts.md C3) from the
// session's frame data. Pure: no session, renderer, Vulkan or filesystem; the
// session feeds it and owns the builder so its storage is reused per frame.
#pragma once

#include <ayther/ayther_core_ffi.h>
#include <ayther/engine/render_observer.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ayther::session {

/// Owner value of an occurrence that no pose substitution claimed.
inline constexpr std::uint32_t kNoPoseOwner = UINT32_MAX;

struct RenderObservationInput {
  std::uint64_t emulation_frame = 0;
  bool frame_known = false;
  engine::render_observation::Composability composability =
      engine::render_observation::Composability::composable;
  std::span<const AytherSpriteOccurrence> occurrences;
  /// Per occurrence, claimed by a pose substitution (empty = none claimed).
  std::span<const std::uint8_t> claimed;
  /// Per occurrence, hidden on purpose by an authoring tool (empty = none).
  std::span<const std::uint8_t> hidden;
  /// Per SAT slot (0..79), the link-chain position (0xFF = unknown).
  std::span<const std::uint8_t> chain_by_slot;
  /// The frame's substitutions: pose substitutions first, then per-sprite.
  std::span<const AytherSpriteSub> subs;
  std::uint32_t pose_sub_count = 0;
  /// Per occurrence, the pose substitution that claimed it or kNoPoseOwner.
  std::span<const std::uint32_t> pose_owner;
  /// How the renderer drew each substitution; null when the host did not
  /// render (or rendered another frame).
  const engine::render_observation::DrawReport *draw = nullptr;
};

class RenderObservationBuilder {
public:
  /// Builds the view for `in`. The returned view and its spans stay valid
  /// until the next call.
  const engine::render_observation::RenderFrameView &
  build(const RenderObservationInput &in);

private:
  std::vector<engine::render_observation::OccurrenceView> occurrences_;
  std::vector<engine::render_observation::ReplacementView> replacements_;
  std::vector<engine::render_observation::OccurrenceId> members_;
  std::vector<std::string> pose_keys_;
  std::vector<std::uint32_t>
      owner_; // per occurrence: substitution or kNoPoseOwner
  std::vector<std::uint32_t> member_count_;
  engine::render_observation::RenderFrameView view_;
};

} // namespace ayther::session
