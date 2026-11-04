// Spec 008 (TP-008.6, D-4): a libretro session publishes the generation of its
// core. The in-repository test core is not Genesis Plus GX, so its id is the
// slug of its library name; the contract is the libretro (gpgx-family) one.
#include <ayther/ayther_recording.h>
#include <ayther/ayther_session.h>
#include <ayther/emulation_generation.h>

#include "../../tools/common/synth_rom.h"

#include <cstdio>
#include <filesystem>
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
    check(static_cast<bool>(created), "the session opens with the test core");
    if (!created) {
        std::fprintf(stderr, "[FAIL] %s\n", created.error.message.c_str());
        return 1;
    }
    const AytherSession &session = **created;
    const ayther::EmulationGeneration &generation = session.emulation_generation();
    check(generation.id == "aythertestcore",
          "the id is the slug of the core's library name");
    check(!generation.version.empty() &&
              generation.version.size() <= ayther::kEmulationVersionMaxBytes,
          "the version is the core's own version string");
    check(generation.determinism_contract == ayther::kGpgxDeterminismContract,
          "a libretro session carries the libretro determinism contract");

    // TP-008.7: a take recorded by this session is stamped with its generation.
    AytherSession &live = **created;
    live.record_start();
    for (int f = 0; f < 4; ++f) {
        live.set_input(0, static_cast<std::uint16_t>((f * 7) & 0xFF));
        (void)live.step();
    }
    live.record_stop();
    const ayther::AytherRecording take = live.take_recording();
    check(take.emulator_id == generation.id &&
              take.emulator_version == generation.version &&
              take.determinism_contract == generation.determinism_contract,
          "take_recording() carries the session's generation");
    const auto path = std::filesystem::temp_directory_path() /
                      "ayther_generation_take.arp";
    check(take.save(path.string()), "the stamped take saves");
    const auto peek = ayther::AytherRecording::peek_provenance(path.string());
    check(peek && peek->id == "aythertestcore" &&
              peek->version == generation.version &&
              peek->contract == ayther::kGpgxDeterminismContract,
          "the saved .arp carries the core's library name as emulator_id");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return failures == 0 ? 0 : 1;
} catch (const std::exception &e) {
    std::fprintf(stderr, "[FAIL] %s\n", e.what());
    return 1;
}
