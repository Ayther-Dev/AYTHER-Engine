#pragma once

#include "ayther_session.h"

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace ayther::session {

/// Position and plane of one visible occurrence, independent of capture metadata.
struct PlaneSetMatch {
    std::uint8_t plane;
    int origin_x;
    int origin_y;
};

[[nodiscard]] inline std::uint64_t plane_cell_position_key(std::uint8_t plane, int x,
                                                           int y) noexcept {
    return (static_cast<std::uint64_t>(plane) << 32) |
           (static_cast<std::uint64_t>(static_cast<std::uint16_t>(x)) << 16) |
           static_cast<std::uint16_t>(y);
}

/// Borrows one frame's visible cells and indexes them for repeated set queries.
/// Cell positions must fit in signed 16-bit coordinates, as PlaneCellHit requires.
class PlaneSetMatcher final {
  public:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the visible size is (width, height) by convention.
    PlaneSetMatcher(std::span<const PlaneCellHit> cells, int width, int height)
        : cells_{cells}, width_{width}, height_{height} {
        positions_.reserve(cells.size());
        for (std::size_t index = 0; index < cells.size(); ++index) {
            const auto &cell = cells[index];
            positions_.emplace(plane_cell_position_key(cell.plane, cell.screen_x, cell.screen_y),
                               index);
        }
    }

    /// Verifies all visible members in the anchor's plane. Offscreen members
    /// are excused; consumed cells cannot participate. The caller owns claiming
    /// and deduplication. matched_cells is scratch storage, valid only on success.
    [[nodiscard]] std::optional<PlaneSetMatch>
    match(std::span<const AytherSession::PlaneSetMember> members, std::size_t anchor_member,
          std::size_t anchor_cell, std::span<const std::uint8_t> consumed,
          std::vector<std::uint32_t> &matched_cells) const {
        matched_cells.clear();
        if (anchor_member >= members.size() || anchor_cell >= cells_.size() ||
            consumed.size() != cells_.size()) {
            return std::nullopt;
        }
        const auto &cell = cells_[anchor_cell];
        const auto &anchor = members[anchor_member];
        if (cell.plane > 2 || cell.hash != anchor.hash || consumed[anchor_cell]) {
            return std::nullopt;
        }
        constexpr int kCellSize = 8;
        const PlaneSetMatch occurrence{cell.plane, cell.screen_x - anchor.cx * kCellSize,
                                       cell.screen_y - anchor.cy * kCellSize};
        for (const auto &member : members) {
            const int x = occurrence.origin_x + member.cx * kCellSize;
            const int y = occurrence.origin_y + member.cy * kCellSize;
            if (x < 0 || x >= width_ || y < 0 || y >= height_) {
                continue;
            }
            const auto position = positions_.find(plane_cell_position_key(occurrence.plane, x, y));
            if (position == positions_.end() || cells_[position->second].hash != member.hash ||
                consumed[position->second]) {
                return std::nullopt;
            }
            matched_cells.push_back(static_cast<std::uint32_t>(position->second));
        }
        return matched_cells.empty() ? std::nullopt : std::optional{occurrence};
    }

  private:
    std::span<const PlaneCellHit> cells_;
    int width_;
    int height_;
    std::unordered_map<std::uint64_t, std::size_t> positions_;
};

} // namespace ayther::session
