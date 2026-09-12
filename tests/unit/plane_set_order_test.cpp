// Plane sets are tried by complexity: the larger set claims its cells first.
//
// Report 2026-09-11 (Golden Axe): a seven-tile Object ("Ax Battler - Magic
// bar") stopped being replaced as soon as the one-tile Objects "Magic bar -
// Empty" and "Magic bar - Border" — both members of it — received an HD asset.
// The matcher walked the sets in unordered_map bucket order, so the one-member
// set claimed the cell first and the seven-member set could never complete.
// Pure: no ROM, no GPU, no session.
#include "session/plane_set_order.h"
#include <cstdint>
#include <cstdio>
#include <vector>
namespace {
int failures = 0;
void check(bool condition, const char *message) {
    if (!condition)
        ++failures;
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}
using ayther::session::plane_set_order;
using ayther::session::PlaneSetOrderKey;
constexpr std::uint64_t kBar = 0x0000000000000A17ull;    // seven tiles, 7x1
constexpr std::uint64_t kEmpty = 0x0000000000000001ull;  // one tile
constexpr std::uint64_t kBorder = 0x0000000000000002ull; // one tile
} // namespace
int main() {
    // The reported case, inserted with the small sets FIRST and lowest ids.
    const std::vector<std::uint64_t> reported =
        plane_set_order({{kEmpty, 1, 1}, {kBorder, 1, 1}, {kBar, 7, 7}});
    check(reported.size() == 3 && reported[0] == kBar, "the seven-tile Object is tried before its one-tile members");
    check(reported[1] == kEmpty && reported[2] == kBorder, "the one-tile Objects follow, by ascending id");
    // Any insertion order gives the same result.
    const std::vector<std::uint64_t> shuffled =
        plane_set_order({{kBar, 7, 7}, {kBorder, 1, 1}, {kEmpty, 1, 1}});
    check(shuffled == reported, "the order does not depend on the input order");
    // Same member count: the larger bbox wins (a sparse 3x3 over a compact 2x1).
    const std::vector<std::uint64_t> area = plane_set_order({{0x10, 2, 2}, {0x20, 2, 9}});
    check(area.size() == 2 && area[0] == 0x20 && area[1] == 0x10, "equal members: the larger bbox goes first");
    // Full tie: ascending id, so the order is total.
    const std::vector<std::uint64_t> tie = plane_set_order({{0x30, 2, 2}, {0x21, 2, 2}, {0x2F, 2, 2}});
    check(tie.size() == 3 && tie[0] == 0x21 && tie[1] == 0x2F && tie[2] == 0x30, "full tie: ascending id");
    // A set with more members outranks a set with a larger bbox.
    const std::vector<std::uint64_t> members = plane_set_order({{0x40, 2, 100}, {0x41, 3, 3}});
    check(members[0] == 0x41, "member count outranks bbox area");
    check(plane_set_order({}).empty(), "no sets: empty order");
    std::printf("plane_set_order: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
