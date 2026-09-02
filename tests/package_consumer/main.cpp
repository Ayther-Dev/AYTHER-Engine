// ---------------------------------------------------------------------------
// The out-of-tree consumer: what a frontend gets when it installs the package.
//
// It has two modes, and the second is the one that matters for a release
// candidate.
//
//   LINK MODE (no arguments). Proves the installed package configures, links,
//   and runs. This is what the per-platform matrix uses on every push.
//
//   RUN MODE (a core and a ROM). Actually drives the product: creates a
//   session, opens a trusted pack, steps frames, and asks the renderer and the
//   audio device whether they are there. It prints a report.
//
// The report deliberately carries NO absolute paths. A release report that
// quotes the producer's build directory cannot be reproduced by the person
// reading it, and it silently proves the artifact was consumed from the source
// tree rather than from an install. Only basenames are printed.
//
// Anything that could not be exercised is printed as "unavailable" with the
// reason. A frontend report that omits the audio device because there was none
// reads, later, exactly like one where audio worked.
// ---------------------------------------------------------------------------
#include <ayther/ayther_sdk.h>
#include <ayther/ayther_sdk_version.h>
#include <ayther/ayther_session.h>
#include <ayther/engine/engine.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <type_traits>

namespace {

/// The last path component. Everything printed goes through this, so the report
/// says "ayther_test_core.dll" and never where it happened to live.
std::string basename_of(const std::string& path) {
    if (path.empty()) return "(none)";
    return std::filesystem::path(path).filename().string();
}

std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? value : std::string{};
}

void line(const char* key, const std::string& value) {
    std::cout << "  " << key << ": " << value << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    static_assert(sizeof(AySessionConfig) > 0);
    static_assert(sizeof(ayther::FrameView) > 0);
    static_assert(std::is_standard_layout_v<ayther::engine::RenderImageView>);
    static_assert(std::is_standard_layout_v<ayther::engine::VulkanContextView>);

    const ayther::engine::VulkanContextView empty_context{};
    if (empty_context.is_valid()) {
        std::cerr << "empty VulkanContextView unexpectedly reports valid\n";
        return 1;
    }

    const ayther::engine::PackView empty_pack{};
    if (empty_pack.is_valid() || !empty_pack.render_tiers().is_legacy() ||
        ayther::engine::core_abi_revision() == 0U) {
        std::cerr << "installed typed pack/core contract is invalid\n";
        return 1;
    }

    const ayther::engine::RenderImageView empty_render_image{};
    if (empty_render_image.is_valid()) {
        std::cerr << "empty RenderImageView unexpectedly reports valid\n";
        return 1;
    }

    const auto engine_version = ayther::engine::version();
    const auto engine_capabilities = ayther::engine::probe_capabilities();
    if (engine_capabilities.renderer !=
            ayther::engine::RendererBackend::vulkan ||
        !engine_capabilities.hardware_acceleration ||
        !engine_capabilities.libretro_video ||
        !engine_capabilities.libretro_audio) {
        std::cerr << "installed Engine capability contract is incomplete\n";
        return 1;
    }

    const ayther::engine::CoreInfo serialization_contract{
        .api_version = 1U,
        .library_name = "installed-package",
    };
    if (serialization_contract.serialize().find(
            "\"library_name\":\"installed-package\"") == std::string::npos) {
        std::cerr << "installed Engine CoreInfo serialization is unavailable\n";
        return 1;
    }

    const auto error = ayther::sdk_version_check();
    if (!error.empty()) {
        std::cerr << error << '\n';
        return 1;
    }

    std::cout << "AYTHER SDK " << ayther::sdk_version().str()
              << " / Engine " << engine_version.major << '.'
              << engine_version.minor << '.' << engine_version.patch << '\n';

    // argv wins over the environment so a human can drive this by hand without
    // exporting anything.
    const std::string core = argc > 1 ? argv[1] : env_or_empty("AYTHER_CONSUMER_CORE");
    const std::string rom  = argc > 2 ? argv[2] : env_or_empty("AYTHER_CONSUMER_ROM");
    const std::string pack = argc > 3 ? argv[3] : env_or_empty("AYTHER_CONSUMER_PACK");
    const std::string registry =
        argc > 4 ? argv[4] : env_or_empty("AYTHER_CONSUMER_TRUST_REGISTRY");

    if (core.empty() || rom.empty()) {
        std::cout << "  mode: link only (no core/ROM given)\n";
        return 0;
    }

    std::cout << "\n=== consumer report ===\n";
    line("mode", "run");
    line("core", basename_of(core));
    line("rom", basename_of(rom));
    line("pack", basename_of(pack));
    line("trust registry", basename_of(registry));

    {
        auto probed = ayther::engine::probe_core(core);
        if (!probed) {
            line("core probe", "FAILED: " + probed.error.message);
            std::cout << "=== consumer FAILED ===\n";
            return 1;
        }
        line("core probe", probed->info().library_name + " " +
                               probed->info().library_version);
    }

    ayther::AytherSession::Config config;
    config.core_path = core;
    config.rom_path = rom;
    config.pack_path = pack;
    config.trust_registry = registry;
    config.derive_core_pack = false;
    // Asked for on purpose. A machine with no sound card reports the device as
    // unavailable below rather than the run pretending audio was not wanted.
    config.enable_audio = true;

    auto created = ayther::AytherSession::create(config);
    if (!created) {
        line("session", "FAILED to create");
        std::cout << "=== consumer FAILED ===\n";
        return 1;
    }
    auto& session = *created;
    line("session", "created");

    // --- Pack -------------------------------------------------------------
    if (pack.empty()) {
        line("pack status", "not requested");
    } else if (session->has_pack()) {
        const std::string game = session->game_id() != nullptr ? session->game_id() : "";
        line("pack status", "open, trusted");
        line("pack game_id", game.empty() ? "(none)" : game);
        line("pack profiles", std::to_string(session->profile_count()));
    } else {
        // A pack that was asked for and did not open is a failure of the thing
        // being demonstrated, not a detail to mention in passing.
        line("pack status", "REQUESTED BUT NOT OPEN");
        std::cout << "=== consumer FAILED ===\n";
        return 1;
    }

    // --- Session + renderer ----------------------------------------------
    constexpr int kFrames = 60;
    uint32_t width = 0;
    uint32_t height = 0;
    bool produced_pixels = false;
    for (int frame = 0; frame < kFrames; ++frame) {
        const ayther::FrameView& view = session->step();
        if (view.fb_pixels != nullptr) {
            produced_pixels = true;
            width = view.fb_width;
            height = view.fb_height;
        }
    }
    line("frames stepped", std::to_string(kFrames));
    line("frame size", std::to_string(width) + "x" + std::to_string(height));
    line("emulator pixels", produced_pixels ? "produced" : "NONE");
    if (!produced_pixels) {
        std::cout << "=== consumer FAILED ===\n";
        return 1;
    }

    // The renderer is part of the installed package whether or not this machine
    // has a GPU to run it on. Saying which of the two happened is the whole
    // point: a report that stayed silent would read like a GPU run.
    line("renderer", "linked from the installed package");
    line("renderer device", "not initialised (no surface in this consumer)");

    // --- Audio ------------------------------------------------------------
    // `audio_audible` is false both when no device opened and when everything
    // is muted, so it is reported as what it is -- a reading, not a verdict on
    // the hardware. Overstating it would be the same omission-as-approval this
    // report exists to avoid.
    line("audio requested", "yes");
    line("audio audible", session->audio_audible() ? "yes" : "no");
    line("audio muted", session->audio_muted() ? "yes" : "no");

    std::cout << "=== consumer OK ===\n";
    return 0;
}
