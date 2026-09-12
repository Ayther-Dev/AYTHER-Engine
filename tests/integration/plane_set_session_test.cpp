// The plane-set catalogue of a live session: define, undefine and clear keep
// the matcher's order in step and never break frame production.
//
// The ordering rule itself (more members first, then bbox, then id) is fixed
// by the pure unit oracle `plane_set_order_test`; the overlap on a real ROM by
// `tools/paint_set_smoke`. This suite drives the session API with the
// in-repository test core and the synthetic ROM, which is what CI can run:
// every mutation rebuilds the order, rejected definitions leave the catalogue
// untouched, and a set whose hashes match no visible cell emits nothing.
#include <ayther/ayther_session.h>

#include "../../tools/common/synth_rom.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
    if (!condition)
        ++failures;
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}
constexpr std::uint64_t kHashA = 0x1234567890ABCDEFull; // matches no synthetic cell
constexpr std::uint64_t kHashB = 0x0FEDCBA987654321ull;
} // namespace

int main() try {
    using ayther::AytherSession;
    const std::string rom = ayther::synth::canonical_rom_path();
    check(!rom.empty(), "the synthetic ROM is written to the temporary directory");
    if (rom.empty())
        return 1;

    AytherSession::Config config;
    config.core_path = AYTHER_TEST_CORE_PATH;
    config.rom_path = rom;
    config.enable_audio = false;
    auto created = AytherSession::create(config);
    check(static_cast<bool>(created), "the session opens with the in-repository test core");
    if (!created) {
        std::fprintf(stderr, "[FAIL] %s\n", created.error.message.c_str());
        return 1;
    }
    std::unique_ptr<AytherSession> &session = *created;

    const AytherSession::PlaneSetMember one[1] = {{kHashA, 0, 0}};
    const AytherSession::PlaneSetMember two[2] = {{kHashA, 0, 0}, {kHashB, 1, 0}};
    // Inserted small-first with the larger set in the middle: the order the
    // matcher walks is rebuilt on each definition, not taken from insertion.
    session->define_plane_set(0x0002, 0, 1, 1, one, 1, "one.png");
    session->define_plane_set(0x0001, 0, 2, 1, two, 2, "two.png");
    session->define_plane_set(0x0003, 0, 1, 1, one, 1, "three.png");
    // Rejected definitions: no id, no members, no asset. None of them may
    // touch the catalogue or the order.
    session->define_plane_set(0, 0, 1, 1, one, 1, "ignored.png");
    session->define_plane_set(0x0004, 0, 1, 1, one, 0, "ignored.png");
    session->define_plane_set(0x0005, 0, 1, 1, one, 1, "");

    bool produced = false;
    std::uint32_t subs = 0;
    for (int frame = 0; frame < 8; ++frame) {
        const ayther::FrameView &view = session->step();
        produced = produced || view.fb_pixels != nullptr;
        subs += view.plane_tile_sub_count;
    }
    check(produced, "frames are produced with three sets defined");
    check(subs == 0, "no plane sub is emitted while no visible cell carries a member hash");

    session->undefine_plane_set(0x0002);
    session->undefine_plane_set(0x9999); // unknown id: a no-op
    const ayther::FrameView &after_undefine = session->step();
    check(after_undefine.plane_tile_sub_count == 0, "undefining a set keeps the frame clean");

    session->clear_plane_sets();
    const ayther::FrameView &after_clear = session->step();
    check(after_clear.fb_pixels != nullptr && after_clear.plane_tile_sub_count == 0,
          "clearing the catalogue keeps producing frames without subs");

    // The catalogue is usable again after a clear.
    session->define_plane_set(0x0006, 1, 1, 1, one, 1, "again.png");
    const ayther::FrameView &after_redefine = session->step();
    check(after_redefine.fb_pixels != nullptr, "a set can be defined again after clearing");

    std::printf("plane_set_session: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
    std::fprintf(stderr, "[FAIL] Unexpected exception: %s\n", error.what());
    return 1;
} catch (...) {
    std::fprintf(stderr, "[FAIL] Unexpected non-standard exception\n");
    return 1;
}
