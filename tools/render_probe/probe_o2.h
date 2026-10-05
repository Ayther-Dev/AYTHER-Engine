// O2, the structural auditor of the render probe with a pack (spec 002, plan
// §5.13). Pure: the probe fills an O2Frame from the session and the renderer.
#pragma once

#include <cstdint>
#include <vector>

namespace ayther::probe {

struct O2Occurrence {
  std::uint32_t index = 0;
  bool core_drawn = true;      ///< the core drew this sprite
  bool claimed = false;        ///< a replacement suppresses its original
  bool original_drawn = false; ///< the composed frame shows the original
  std::uint8_t depth = 0;      ///< link-chain position (lower = in front)
  /// Layout: screen position and flips (bit0 h, bit1 v). The identity
  /// (`index`) is flip-invariant; 6b compares the layout too.
  std::int16_t x = 0;
  std::int16_t y = 0;
  std::uint8_t flips = 0;
};

struct O2Replacement {
  std::uint32_t index = 0;
  std::uint64_t key = 0; ///< identity across frames (pose key or asset hash)
  bool drawn = false;    ///< the renderer drew it
  bool texture_ready = false;
  std::vector<std::uint32_t> members; ///< occurrence indices
  /// Depth at which each member's partition was drawn, parallel to members
  /// (empty = not drawn by partitions).
  std::vector<std::uint8_t> partition_depths;
};

struct O2Frame {
  std::vector<O2Occurrence> occurrences;
  std::vector<O2Replacement> replacements;
};

enum class O2Invariant : std::uint8_t {
  claimed_in_applied,     ///< claimed ⊆ members of applied replacements
  claimed_drawn_resident, ///< each claimed member drawn with a resident texture
  partition_depth,        ///< each partition at its member's depth
  unclaimed_drawn,        ///< every unclaimed sprite the core drew is drawn
  no_double,              ///< nothing shows as original and as replacement
  transition,             ///< a replacement leaves with its members
};

struct O2Violation {
  O2Invariant invariant = O2Invariant::claimed_in_applied;
  std::uint32_t subject = 0; ///< occurrence or replacement index
};

/// Violations of the six invariants in `frame`; `previous` is the frame
/// before it in the take (nullptr at the first one).
inline std::vector<O2Violation> check_o2(const O2Frame &frame,
                                         const O2Frame *previous) {
  std::vector<O2Violation> out;
  const auto add = [&](O2Invariant inv, std::uint32_t subject) {
    out.push_back({inv, subject});
  };
  const auto occurrence = [](const O2Frame &f,
                             std::uint32_t index) -> const O2Occurrence * {
    for (const O2Occurrence &o : f.occurrences)
      if (o.index == index)
        return &o;
    return nullptr;
  };
  // The drawn replacement whose member `index` is, if any.
  const auto drawn_owner = [&](std::uint32_t index) -> const O2Replacement * {
    for (const O2Replacement &r : frame.replacements)
      if (r.drawn)
        for (const std::uint32_t m : r.members)
          if (m == index)
            return &r;
    return nullptr;
  };

  for (const O2Occurrence &o : frame.occurrences) {
    const O2Replacement *owner = drawn_owner(o.index);
    // 1. claimed ⊆ members of applied (drawn) replacements.
    if (o.claimed && owner == nullptr)
      add(O2Invariant::claimed_in_applied, o.index);
    // 4. Every unclaimed sprite the core drew is drawn.
    if (o.core_drawn && !o.claimed && !o.original_drawn)
      add(O2Invariant::unclaimed_drawn, o.index);
    // 5. Never both the original and its replacement.
    if (o.original_drawn && owner != nullptr)
      add(O2Invariant::no_double, o.index);
  }
  for (const O2Replacement &r : frame.replacements) {
    if (!r.drawn)
      continue;
    // 2. Claimed members drawn with a resident texture, one partition each.
    bool claims = false;
    for (const std::uint32_t m : r.members) {
      const O2Occurrence *o = occurrence(frame, m);
      claims = claims || (o != nullptr && o->claimed);
    }
    if (claims &&
        (!r.texture_ready || r.partition_depths.size() != r.members.size()))
      add(O2Invariant::claimed_drawn_resident, r.index);
    // 3. Each partition at its member's depth.
    for (std::size_t i = 0;
         i < r.members.size() && i < r.partition_depths.size(); ++i) {
      const O2Occurrence *o = occurrence(frame, r.members[i]);
      if (o != nullptr && r.partition_depths[i] != o->depth) {
        add(O2Invariant::partition_depth, r.index);
        break;
      }
    }
    // 6a. A replacement does not outlive its members.
    bool any_member = false;
    for (const std::uint32_t m : r.members) {
      const O2Occurrence *o = occurrence(frame, m);
      any_member = any_member || (o != nullptr && o->core_drawn);
    }
    if (!any_member)
      add(O2Invariant::transition, r.index);
  }
  // 6b. A replacement does not leave before its members: drawn in the
  //     previous frame and not drawn now, while its members are still the
  //     same sprites in the same layout (flips and positions relative to
  //     each other) and still claimed. A hand-off is not a departure: when
  //     every member now belongs to one other drawn replacement, the pose
  //     changed with the game (same sprites, other pose; or a pose that
  //     grows as more sprites enter).
  if (previous != nullptr)
    for (const O2Replacement &before : previous->replacements) {
      if (!before.drawn || before.members.empty())
        continue;
      bool still_drawn = false;
      for (const O2Replacement &now : frame.replacements)
        still_drawn = still_drawn || (now.drawn && now.key == before.key);
      if (still_drawn)
        continue;
      const O2Occurrence *first_was = occurrence(*previous, before.members[0]);
      const O2Occurrence *first_now = occurrence(frame, before.members[0]);
      bool members_stay = first_was != nullptr && first_now != nullptr;
      for (const std::uint32_t m : before.members) {
        const O2Occurrence *was = occurrence(*previous, m);
        const O2Occurrence *o = occurrence(frame, m);
        members_stay = members_stay && was != nullptr && o != nullptr &&
                       o->core_drawn && o->claimed && o->flips == was->flips &&
                       o->x - first_now->x == was->x - first_was->x &&
                       o->y - first_now->y == was->y - first_was->y;
      }
      if (!members_stay)
        continue;
      const O2Replacement *heir = drawn_owner(before.members[0]);
      bool hand_off = heir != nullptr;
      for (const std::uint32_t m : before.members)
        hand_off = hand_off && drawn_owner(m) == heir;
      if (!hand_off)
        add(O2Invariant::transition, before.index);
    }
  return out;
}

} // namespace ayther::probe
