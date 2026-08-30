// ---------------------------------------------------------------------------
// PackRuntime: activation, profiles, the trust registry, assets, validation.
//
// The whole point of the extraction is that none of this needs a machine any
// more. This test boots no emulator core, opens no audio device, and creates
// no Vulkan context: it bakes a signed pack into a temporary directory and asks
// the runtime questions about it.
// ---------------------------------------------------------------------------
#include "session/pack_runtime.h"

#include "trusted_pack_fixture.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("  [%s] %s\n", condition ? " OK " : "FAIL", message);
    if (!condition) ++failures;
}

// Two declared profiles plus the implicit "original" the core always prepends.
const char* const kManifest =
    "[pack]\n"
    "name       = \"pack_runtime_test\"\n"
    "version    = \"1.0.0\"\n"
    "game_id    = \"crc32:0000beef\"\n"
    "ayther_min = \"0.1.0\"\n"
    "schema     = 2\n"
    "\n[regions]\n"
    "default = \"NTSC\"\n"
    "supported = [\"NTSC\"]\n"
    "\n[systems]\n"
    "included = [\"sprites\", \"tiles\"]\n"
    "\n[[profile]]\n"
    "id      = \"faithful\"\n"
    "name    = \"Fiel\"\n"
    "systems = [\"sprites\"]\n"
    "\n[[profile]]\n"
    "id      = \"enhanced\"\n"
    "name    = \"Mejorado\"\n"
    "systems = [\"sprites\", \"tiles\"]\n"
    "default = true\n";

bool bake(ayther::test::TrustedPackFixture& fixture) {
    const std::string manifest = kManifest;
    const std::string asset = "not really audio, but it has a size";
    char error[512] = {};
    const bool staged =
        fixture.add_bytes("manifest.toml",
                          reinterpret_cast<const uint8_t*>(manifest.data()),
                          manifest.size()) &&
        fixture.add_bytes("audio/tone.wav",
                          reinterpret_cast<const uint8_t*>(asset.data()),
                          asset.size());
    if (!staged || !fixture.finish(error, sizeof(error))) {
        std::printf("  [FAIL] could not bake the fixture pack: %s\n", error);
        return false;
    }
    return true;
}

}  // namespace

int main() {
    using ayther::session::PackRuntime;

    ayther::test::TrustedPackFixture fixture{"pack_runtime"};
    if (!bake(fixture)) return 1;

    // --- Activation -------------------------------------------------------
    {
        PackRuntime pack;
        check(!pack.loaded(), "a fresh runtime holds no pack");
        check(pack.get() == nullptr, "and hands out no archive");
        check(!static_cast<bool>(pack), "and is falsy");

        // "No pack" is a state, not a failure: callers clear a pack this way.
        const auto cleared = pack.open("");
        check(static_cast<bool>(cleared), "opening an empty path succeeds");
        check(!pack.loaded(), "and leaves nothing loaded");

        const auto missing = pack.open("does_not_exist_anywhere.ay");
        check(static_cast<bool>(missing), "a missing path is not an error");
        check(!pack.loaded(), "and still leaves nothing loaded");
    }

    // --- Trust ------------------------------------------------------------
    {
        PackRuntime pack;
        const auto untrusted = pack.open_trusted(fixture.pack_path(),
                                                 "no_such_registry.toml");
        check(!untrusted, "a pack does not open under an unknown registry");
        check(!pack.loaded(), "and nothing is left loaded after a trust refusal");
    }

    PackRuntime pack;
    const auto opened = pack.open_trusted(fixture.pack_path(),
                                          fixture.registry_path());
    if (!opened) {
        std::printf("  [FAIL] the signed fixture pack did not open\n");
        return 1;
    }
    check(pack.loaded(), "a signed pack opens under its own registry");
    check(pack.get() != nullptr, "and exposes the archive");
    check(pack.path() == fixture.pack_path(), "the runtime remembers its path");
    check(pack.trust_registry() == fixture.registry_path(),
          "and the registry it was trusted against");
    check(std::string(pack.game_id()) == "crc32:0000beef",
          "the pack's game id is readable without a session");

    // --- Profiles ---------------------------------------------------------
    {
        // The core always prepends an implicit "original" with an empty mask,
        // so a pack declaring two profiles reports three.
        check(pack.profile_count() == 3,
              "the implicit 'original' is counted alongside the declared ones");

        const auto faithful = pack.profile_by_id("faithful");
        check(faithful.has_value(), "a declared profile is found by id");
        check(faithful && faithful->name == "Fiel",
              "and carries the name the manifest gave it");
        check(faithful && faithful->systems != 0,
              "and a non-empty systems mask");

        const auto original = pack.profile_by_id("original");
        check(original && original->systems == 0,
              "'original' turns nothing on");

        // An id that does not exist is not approximated.
        check(!pack.profile_by_id("does-not-exist").has_value(),
              "an unknown profile id yields nothing, not the nearest match");
        check(!pack.profile_by_id("").has_value(), "an empty id yields nothing");
        check(!pack.profile(999).has_value(), "an out-of-range index is refused");

        const auto fallback = pack.default_profile();
        check(fallback && fallback->id == "enhanced",
              "the manifest's default profile is the one reported");

        check(pack.profiles().size() == pack.profile_count(),
              "enumerating profiles agrees with the count");
    }

    // --- Which profile the live state corresponds to ----------------------
    {
        const auto enhanced = pack.profile_by_id("enhanced");
        const auto faithful = pack.profile_by_id("faithful");
        check(enhanced && faithful, "both declared profiles resolved");

        check(pack.active_profile(enhanced->systems, enhanced->muted_buses) ==
                  "enhanced",
              "a state matching a profile reports that profile");
        check(pack.active_profile(faithful->systems, faithful->muted_buses) ==
                  "faithful",
              "and the same holds for the other one");

        // Arrived-at state that no profile describes is legitimately nameless.
        check(pack.active_profile(0xDEADu, 0u).empty(),
              "a state no profile describes reports no profile");

        // The hint is preferred only while the state still supports it.
        pack.set_profile_hint("faithful");
        check(pack.active_profile(faithful->systems, faithful->muted_buses) ==
                  "faithful",
              "a hint that the state supports is honoured");
        check(pack.active_profile(enhanced->systems, enhanced->muted_buses) ==
                  "enhanced",
              "a hint the state contradicts is not believed");
        pack.clear_profile_hint();
        check(pack.profile_hint().empty(), "the hint can be cleared");
    }

    // --- Declared systems and assets --------------------------------------
    {
        check(pack.declares_systems(), "the manifest's [systems] is visible");
        check(pack.systems() != 0, "and reports a non-empty mask");

        check(pack.has_asset("audio/tone.wav"), "a staged asset is found");
        check(pack.asset_size("audio/tone.wav") > 0, "and reports a size");
        check(!pack.has_asset("audio/absent.wav"), "a missing asset is not found");
        check(!pack.has_asset(""), "an empty logical path is not an asset");
    }

    // --- Reload and close -------------------------------------------------
    {
        pack.set_profile_hint("faithful");
        const auto reloaded = pack.reload();
        check(static_cast<bool>(reloaded), "a loaded pack reloads");
        check(pack.loaded() && pack.path() == fixture.pack_path(),
              "and comes back at the same path");
        check(pack.profile_hint().empty(),
              "reloading drops the hint: it belonged to the previous open");

        pack.close();
        check(!pack.loaded(), "close releases the pack");
        check(pack.path().empty() && pack.trust_registry().empty(),
              "and forgets where it came from");
        check(pack.profile_count() == 0 && pack.game_id()[0] == '\0',
              "a closed runtime answers emptily instead of dereferencing");
        check(!pack.has_asset("audio/tone.wav"),
              "and resolves no assets once closed");

        const auto after_close = pack.reload();
        check(static_cast<bool>(after_close) && !pack.loaded(),
              "reloading with nothing open succeeds and stays empty");
    }

    // --- Validation, with a context no emulator supplied ------------------
    {
        PackRuntime::ValidateContext context;
        context.platform = "megadrive";
        context.core_build_id = "unit-test-core";
        context.release_build = false;

        const auto findings = PackRuntime::validate(fixture.pack_path(), context);
        // The fixture pack is well-formed, so the interesting assertion is that
        // validation ran and reported nothing blocking.
        bool blocking = false;
        for (const auto& finding : findings) blocking = blocking || finding.blocking;
        check(!blocking, "a well-formed pack reports no blocking finding");

        check(PackRuntime::validate("", context).empty(),
              "validating an empty path reports nothing rather than crashing");

        const auto absent =
            PackRuntime::validate("does_not_exist_anywhere.ay", context);
        bool absent_blocks = false;
        for (const auto& finding : absent) {
            absent_blocks = absent_blocks || finding.blocking;
        }
        check(absent.empty() || absent_blocks,
              "a pack that is not there either reports nothing or reports a "
              "blocking finding, never a silent pass");
    }

    std::printf("%s\n", failures == 0 ? "=== PASS ===" : "=== FAIL ===");
    return failures == 0 ? 0 : 1;
}
