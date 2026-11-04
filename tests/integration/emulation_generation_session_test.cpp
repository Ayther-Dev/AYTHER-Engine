// Spec 008 (TP-008.6, D-4): a libretro session publishes the generation of its
// core. The in-repository test core is not Genesis Plus GX, so its id is the
// slug of its library name; the contract is the libretro (gpgx-family) one.
#include <ayther/ayther_session.h>
#include <ayther/emulation_generation.h>

#include "../../tools/common/synth_rom.h"

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
    return failures == 0 ? 0 : 1;
} catch (const std::exception &e) {
    std::fprintf(stderr, "[FAIL] %s\n", e.what());
    return 1;
}
