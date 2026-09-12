#pragma once
// The order in which plane SETS are tried within one frame.
//
// A set claims the cells it matches (`consumed`), and a member that lands on a
// consumed cell fails the whole set. Until 2026-09-11 the matcher walked
// `plane_sets` — an unordered_map — in bucket order: a one-member set with a
// lucky id could claim a cell before the seven-member set containing it was
// tried, and the larger set never matched. In Golden Axe, assigning HD to the
// one-tile "Magic bar - Empty" and "Magic bar - Border" Objects cancelled the
// seven-tile "Ax Battler - Magic bar". ayther_rank.h states the product rule
// — the more complex entity wins and claims — for the ladder BETWEEN domains;
// this applies it WITHIN the plane-set domain.
//
// Pure: no session, no GPU. Fixed by tests/unit/plane_set_order_test.cpp.
#include <algorithm>
#include <cstdint>
#include <vector>

namespace ayther::session {

/// What decides a set's turn: member count, then bbox area, then id.
struct PlaneSetOrderKey {
    std::uint64_t id;
    std::uint32_t members;
    std::uint32_t area; ///< w_cells * h_cells
};

/// Ids sorted by complexity: more members first, then larger bbox, then
/// ascending id, so the order is total and independent of the input order.
[[nodiscard]] inline std::vector<std::uint64_t> plane_set_order(std::vector<PlaneSetOrderKey> keys) {
    std::sort(keys.begin(), keys.end(), [](const PlaneSetOrderKey &a, const PlaneSetOrderKey &b) {
        if (a.members != b.members)
            return a.members > b.members;
        if (a.area != b.area)
            return a.area > b.area;
        return a.id < b.id;
    });
    std::vector<std::uint64_t> order;
    order.reserve(keys.size());
    for (const auto &key : keys)
        order.push_back(key.id);
    return order;
}

} // namespace ayther::session
