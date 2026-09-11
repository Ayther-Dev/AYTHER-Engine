// The cell→sub join for 1×1 plane-tile subs must respect the plane.
//
// Report 2026-09-11 (Golden Axe, Stage 1): the HD glyphs of "MAGIC" on plane
// A erased the tree trunk of plane B under them and showed the backdrop. The
// plane-B cell at the same screen position received the glyph's sub (the
// lookup was by x,y only), was marked claimed, and the compose skipped it.
// Pure: no ROM, no GPU, no session.
#include "session/plane_sub_join.h"
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
AytherSpriteSub sub_at(std::int16_t x, std::int16_t y, std::uint8_t w = 1, std::uint8_t h = 1) {
    AytherSpriteSub s{};
    std::memset(&s, 0, sizeof(s));
    s.screen_x = x;
    s.screen_y = y;
    s.w_tiles = w;
    s.h_tiles = h;
    return s;
}
} // namespace
int main() {
    using ayther::session::plane_sub_at;
    constexpr std::uint8_t A = 0, B = 1, W = 2; // PlaneCellHit plane codes
    // The reported case: glyph "M" on A at (80,8); trunk on B at (80,8). Plus a
    // direct HD tile on B at (16,24) and a 2×1 set on A at (40,24).
    AytherSpriteSub subs[3] = {sub_at(80, 8), sub_at(16, 24), sub_at(40, 24, 2, 1)};
    const std::uint8_t sub_plane[3] = {A, B, A};
    check(plane_sub_at(subs, sub_plane, 3, A, 80, 8) == 0, "the A cell at (80,8) finds its glyph");
    check(plane_sub_at(subs, sub_plane, 3, B, 80, 8) == -1,
          "the B cell under the glyph is not the glyph's: the trunk is drawn");
    check(plane_sub_at(subs, sub_plane, 3, W, 80, 8) == -1, "nor the Window cell at that position");
    check(plane_sub_at(subs, sub_plane, 3, B, 16, 24) == 1, "a sub emitted on B is found by the B cell");
    check(plane_sub_at(subs, sub_plane, 3, A, 16, 24) == -1, "and not by the A cell above it");
    check(plane_sub_at(subs, sub_plane, 3, A, 40, 24) == -1,
          "a sub wider than one cell (a set) is not joined by this path");
    check(plane_sub_at(subs, sub_plane, 3, A, 88, 8) == -1, "another position of the same plane: none");
    check(plane_sub_at(subs, sub_plane, 0, A, 80, 8) == -1, "no subs: -1");
    check(plane_sub_at(nullptr, sub_plane, 3, A, 80, 8) == -1 &&
              plane_sub_at(subs, nullptr, 3, A, 80, 8) == -1,
          "null pointers: -1");
    // Two 1×1 subs at the same position on different planes: each cell finds its own.
    AytherSpriteSub twins[2] = {sub_at(8, 8), sub_at(8, 8)};
    const std::uint8_t twin_plane[2] = {B, A};
    check(plane_sub_at(twins, twin_plane, 2, A, 8, 8) == 1 &&
              plane_sub_at(twins, twin_plane, 2, B, 8, 8) == 0,
          "same position on two planes: each cell joins the sub of its plane");
    std::printf("plane_sub_join: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
