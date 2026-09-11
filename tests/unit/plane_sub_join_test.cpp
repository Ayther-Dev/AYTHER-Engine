// The cell→sub join for 1×1 plane-tile subs must respect the plane.
//
// Report 2026-09-11 (Golden Axe, Stage 1): the HD glyphs of "MAGIC" on plane
// A erased the tree trunk of plane B under them and showed the backdrop. The
// plane-B cell at the same screen position received the glyph's sub (the
// lookup was by x,y only), was marked claimed, and the compose skipped it.
// Pure: no ROM, no GPU, no session.
#include "session/plane_sub_join.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
namespace {
int failures = 0;
void check(bool condition, const char *message) {
    if (!condition)
        ++failures;
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}
/// A sub covering `cells` (as w_tiles x h_tiles) at the cell's screen position.
AytherSpriteSub sub_at(const ayther::PlaneCellHit &at, std::uint8_t cells_wide = 1) {
    AytherSpriteSub s{};
    std::memset(&s, 0, sizeof(s));
    s.screen_x = at.screen_x;
    s.screen_y = at.screen_y;
    s.w_tiles = cells_wide;
    s.h_tiles = 1;
    return s;
}
ayther::PlaneCellHit cell(std::uint8_t plane, const std::array<std::int16_t, 2> &xy) {
    ayther::PlaneCellHit c{};
    c.plane = plane;
    c.screen_x = xy[0];
    c.screen_y = xy[1];
    return c;
}
} // namespace
int main() {
    using ayther::session::plane_sub_at;
    constexpr std::uint8_t A = 0, B = 1, W = 2; // PlaneCellHit plane codes
    // The reported case: glyph "M" on A at (80,8); trunk on B at (80,8). Plus a
    // direct HD tile on B at (16,24) and a 2×1 set on A at (40,24).
    const std::array<AytherSpriteSub, 3> subs{
        sub_at(cell(A, {80, 8})), sub_at(cell(B, {16, 24})), sub_at(cell(A, {40, 24}), 2)};
    const std::array<std::uint8_t, 3> sub_plane{A, B, A};
    check(plane_sub_at(subs, sub_plane, cell(A, {80, 8})) == 0, "the A cell at (80,8) finds its glyph");
    check(plane_sub_at(subs, sub_plane, cell(B, {80, 8})) == -1,
          "the B cell under the glyph is not the glyph's: the trunk is drawn");
    check(plane_sub_at(subs, sub_plane, cell(W, {80, 8})) == -1, "nor the Window cell at that position");
    check(plane_sub_at(subs, sub_plane, cell(B, {16, 24})) == 1, "a sub emitted on B is found by the B cell");
    check(plane_sub_at(subs, sub_plane, cell(A, {16, 24})) == -1, "and not by the A cell above it");
    check(plane_sub_at(subs, sub_plane, cell(A, {40, 24})) == -1,
          "a sub wider than one cell (a set) is not joined by this path");
    check(plane_sub_at(subs, sub_plane, cell(A, {88, 8})) == -1, "another position of the same plane: none");
    check(plane_sub_at({}, {}, cell(A, {80, 8})) == -1, "no subs: -1");
    check(plane_sub_at(subs, std::span<const std::uint8_t>{sub_plane}.first(1), cell(B, {16, 24})) == -1,
          "a sub without a recorded plane is not joined");
    // Two 1×1 subs at the same position on different planes: each cell finds its own.
    const std::array<AytherSpriteSub, 2> twins{sub_at(cell(B, {8, 8})), sub_at(cell(A, {8, 8}))};
    const std::array<std::uint8_t, 2> twin_plane{B, A};
    check(plane_sub_at(twins, twin_plane, cell(A, {8, 8})) == 1 &&
              plane_sub_at(twins, twin_plane, cell(B, {8, 8})) == 0,
          "same position on two planes: each cell joins the sub of its plane");
    std::printf("plane_sub_join: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
