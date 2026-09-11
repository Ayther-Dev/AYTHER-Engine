#include "session/plane_set_match.h"

#include <array>
#include <cstdio>
#include <exception>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
    if (!condition)
        ++failures;
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}
} // namespace

int main() try {
    using ayther::AytherSession;
    using ayther::PlaneCellHit;
    using ayther::session::PlaneSetMatcher;
    const std::array<AytherSession::PlaneSetMember, 2> members{{{11, 0, 0}, {22, 1, 0}}};
    std::vector<std::uint32_t> matched_cells;
    for (std::uint8_t plane = 0; plane < 3; ++plane) {
        std::array<PlaneCellHit, 2> cells{};
        cells[0].hash = 11;
        cells[0].screen_x = 16;
        cells[0].screen_y = 24;
        cells[1].hash = 22;
        cells[1].screen_x = 24;
        cells[1].screen_y = 24;
        cells[0].plane = cells[1].plane = plane;
        std::array<std::uint8_t, 2> consumed{};
        const PlaneSetMatcher matcher{cells, 320, 224};
        const auto occurrence = matcher.match(members, 0, 0, consumed, matched_cells);
        check(occurrence && occurrence->plane == plane && occurrence->origin_x == 16 &&
                  occurrence->origin_y == 24 && matched_cells == std::vector<std::uint32_t>{0, 1},
              "a complete set matches in each plane and reports that plane");
        check(matcher.match(members, 1, 1, consumed, matched_cells).has_value(),
              "any matching member can anchor the same occurrence");
        consumed[1] = 1;
        check(!matcher.match(members, 0, 0, consumed, matched_cells),
              "a cell claimed by higher-priority content cannot be reused");
        consumed[1] = 0;
        cells[1].plane = static_cast<std::uint8_t>((plane + 1) % 3);
        const PlaneSetMatcher split_matcher{cells, 320, 224};
        check(!split_matcher.match(members, 0, 0, consumed, matched_cells),
              "matching hashes split across planes do not form a set");
    }
    std::array<PlaneCellHit, 1> clipped_cells{};
    clipped_cells[0].hash = 22;
    clipped_cells[0].plane = 2;
    const std::array<std::uint8_t, 1> consumed{};
    const PlaneSetMatcher clipped_matcher{clipped_cells, 320, 224};
    const auto clipped = clipped_matcher.match(members, 1, 0, consumed, matched_cells);
    check(clipped && clipped->origin_x == -8 && matched_cells.size() == 1,
          "an offscreen member does not prevent a partially visible occurrence");
    clipped_cells[0].plane = 3;
    const PlaneSetMatcher invalid_matcher{clipped_cells, 320, 224};
    check(!invalid_matcher.match(members, 1, 0, consumed, matched_cells),
          "sprite and invalid planes cannot anchor plane sets");
    check(ayther::session::plane_cell_position_key(0, 16, 24) !=
              ayther::session::plane_cell_position_key(1, 16, 24),
          "deduplication distinguishes simultaneous occurrences across planes");
    return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
    std::fprintf(stderr, "[FAIL] Unexpected exception: %s\n", error.what());
    return 1;
} catch (...) {
    std::fprintf(stderr, "[FAIL] Unexpected non-standard exception\n");
    return 1;
}
