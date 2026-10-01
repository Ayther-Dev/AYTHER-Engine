// AytherSession::set_audio_events restores the events of an earlier
// analyze_audio_events of the same take — the frontend caches them next to
// the take so that opening it again does not replay it to analyse its audio.
//
// The suite drives a live session with the in-repository test core and the
// synthetic ROM: restored events read back unchanged, a second restore
// replaces the first (also with the same count, which used to key the
// derived caches), and null or zero clears them. Frame production keeps
// working afterwards.
#include <ayther/ayther_session.h>

#include "../../tools/common/synth_rom.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
    if (!condition)
        ++failures;
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

AytherAudioEvent event(uint64_t sig, uint32_t start, uint32_t end, uint8_t chip, uint8_t channel) {
    AytherAudioEvent e{};
    e.signature = sig;
    e.instrument = sig ^ 0xABCDu;
    e.start_frame = start;
    e.end_frame = end;
    e.chip = chip;
    e.channel = channel;
    e.pitch = 60;
    e.velocity = 100;
    return e;
}

bool same(const AytherAudioEvent *a, const std::vector<AytherAudioEvent> &b, uint32_t n) {
    if (n != b.size())
        return false;
    for (uint32_t i = 0; i < n; ++i)
        if (std::memcmp(&a[i], &b[i], sizeof(AytherAudioEvent)) != 0)
            return false;
    return true;
}
} // namespace

int main() try {
    using ayther::AytherSession;
    static_assert(AytherSession::kAudioEventAlgo >= 1, "the detector version starts at 1");

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

    check(session->audio_event_count() == 0 && session->audio_events() == nullptr,
          "a new session has no audio events");

    const std::vector<AytherAudioEvent> first = {event(0x1111, 10, 40, 0, 2), event(0x2222, 12, 12, 1, 3),
                                                 event(0x3333, 50, 90, 0, 5)};
    session->set_audio_events(first.data(), static_cast<uint32_t>(first.size()));
    check(same(session->audio_events(), first, session->audio_event_count()),
          "restored events read back unchanged, field by field");

    // The same count with other content: the derived caches used to be keyed
    // by the count alone.
    const std::vector<AytherAudioEvent> second = {event(0x4444, 1, 2, 0, 0), event(0x5555, 3, 4, 1, 1),
                                                  event(0x6666, 5, 6, 0, 4)};
    session->set_audio_events(second.data(), static_cast<uint32_t>(second.size()));
    check(same(session->audio_events(), second, session->audio_event_count()),
          "a second restore with the same count replaces the first");

    session->set_audio_events(nullptr, 3);
    check(session->audio_event_count() == 0 && session->audio_events() == nullptr,
          "restoring null clears the events");
    session->set_audio_events(first.data(), 0);
    check(session->audio_event_count() == 0, "restoring zero events clears them");

    session->set_audio_events(first.data(), static_cast<uint32_t>(first.size()));
    session->clear_audio_events();
    check(session->audio_event_count() == 0, "clear_audio_events still clears restored events");

    session->set_audio_events(first.data(), static_cast<uint32_t>(first.size()));
    bool produced = true;
    for (int i = 0; i < 3; ++i)
        produced = produced && session->step().fb_width > 0;
    check(produced && session->audio_event_count() == first.size(),
          "frames keep being produced and stepping does not drop the restored events");

    std::printf("%s\n", failures == 0 ? "audio_events_restore: PASS" : "audio_events_restore: FAIL");
    return failures == 0 ? 0 : 1;
} catch (const std::exception &e) {
    std::fprintf(stderr, "[FAIL] Unexpected exception: %s\n", e.what());
    return 1;
} catch (...) {
    std::fprintf(stderr, "[FAIL] Unexpected non-standard exception\n");
    return 1;
}
