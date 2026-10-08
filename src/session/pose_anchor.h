// Spec 002, R1 (RF-8.1, RF-8.4): the anchor of each pose substitution — the
// member occurrence at whose depth its HD is drawn. Membership is the owner
// the pose resolve reports per occurrence, the same for pack catalog poses
// and Lab preview overrides; no occurrence outside the pose can anchor it.
#pragma once

#include <ayther/ayther_core_ffi.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace ayther::session {

/// Anchor value of a pose substitution with no member on screen.
inline constexpr std::uint32_t kNoPoseAnchor = UINT32_MAX;

/// For each pose substitution `s < anchor.size()`, the index of its anchor
/// occurrence, or kNoPoseAnchor. `owner[i]` is the pose substitution that
/// claimed occurrence `i` (any value >= anchor.size() means none).
/// `chain_by_occurrence[i]` is the link-chain position of occurrence `i`
/// (0xFF = unknown). It is deliberately not indexed by SAT slot: a slot can
/// be reused by distinct parsed records within one frame.
///
/// The anchor is the frontmost member: the lowest chain position, which the
/// VDP draws in front; an unknown chain sorts behind every known one, and a
/// tie falls to the lowest SAT slot.
inline void pose_anchors(std::span<const AytherSpriteOccurrence> occs,
                         std::span<const std::uint32_t> owner,
                         std::span<const std::uint8_t> chain_by_occurrence,
                         std::span<std::uint32_t> anchor) {
  const auto depth = [&](std::size_t index) {
    const AytherSpriteOccurrence &o = occs[index];
    const unsigned chain =
        index < chain_by_occurrence.size() ? chain_by_occurrence[index] : 0xFFU;
    return (chain << 8U) | o.slot;
  };
  for (std::uint32_t &a : anchor)
    a = kNoPoseAnchor;
  for (std::size_t i = 0; i < occs.size() && i < owner.size(); ++i) {
    const std::uint32_t s = owner[i];
    if (s >= anchor.size())
      continue;
    std::uint32_t &a = anchor[s];
    if (a == kNoPoseAnchor || depth(i) < depth(a))
      a = static_cast<std::uint32_t>(i);
  }
}

} // namespace ayther::session
