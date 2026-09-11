#pragma once
// The cell→sub join of the scene inventory for 1×1 plane-tile subs.
//
// A 1×1 plane-tile sub (a glyph, a tile with a direct HD asset) is emitted in
// the plane where its cell appeared; `plane_tile_sub_plane` records that plane
// in parallel with `plane_tile_subs`. Until 2026-09-11 the inventory looked
// up a cell's sub by screen position only: a plane-B cell at the same
// position as a plane-A glyph received the glyph's sub, was marked `claimed`,
// the compose skipped it and drew the glyph in the B pass. In Golden Axe,
// Stage 1, the "MAGIC" letters over the tree trunk erased the trunk and showed
// the backdrop. The cell→set join already required the same plane; the 1×1
// join did not.
//
// Pure: no session, no GPU. Fixed by tests/unit/plane_sub_join_test.cpp.
#include "ayther_session.h" // PlaneCellHit; AytherSpriteSub comes with it

#include <cstdint>
#include <span>

namespace ayther::session {

/// Index of the 1×1 sub replacing `cell` (its plane and screen position), or
/// -1. `subs` and `sub_plane` are parallel: screen position and emitting plane
/// of each sub; only the first `min(size)` entries are consulted. Subs wider
/// than one cell (sets) are joined elsewhere, by rect and same plane.
[[nodiscard]] inline std::int32_t plane_sub_at(std::span<const ::AytherSpriteSub> subs,
                                               std::span<const std::uint8_t> sub_plane,
                                               const PlaneCellHit &cell) noexcept {
    const std::size_t n = subs.size() < sub_plane.size() ? subs.size() : sub_plane.size();
    for (std::size_t i = 0; i < n; ++i) {
        const ::AytherSpriteSub &q = subs[i];
        if (q.w_tiles == 1 && q.h_tiles == 1 && q.screen_x == cell.screen_x &&
            q.screen_y == cell.screen_y && sub_plane[i] == cell.plane)
            return static_cast<std::int32_t>(i);
    }
    return -1;
}

} // namespace ayther::session
