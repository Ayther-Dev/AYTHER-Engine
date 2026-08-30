// ---------------------------------------------------------------------------
// Rotation, revocation, and per-game scope, through the C FFI.
//
// core/src/pack_trust.rs and core/src/pack_builder.rs pin this in Rust. That
// leaves the question a Rust test cannot answer: does the boundary a native
// frontend actually calls -- ayther_pack_open_trusted -- refuse in the same
// cases? A policy enforced in the library and lost at the FFI is not enforced.
//
// Every refusal here is checked as a NULL handle: the C surface has no partial
// or degraded open, and that is the property under test.
// ---------------------------------------------------------------------------
#include <ayther/ayther_core_ffi.h>

#include "trust_scratch.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("  [%s] %s\n", condition ? " OK " : "FAIL", message);
    if (!condition) ++failures;
}

// The pack under test declares game_id = "sonic2".
const char* const kManifest =
    "[pack]\n"
    "name       = \"trust_ffi\"\n"
    "version    = \"1.0.0\"\n"
    "game_id    = \"sonic2\"\n"
    "ayther_min = \"0.1.0\"\n"
    "\n[regions]\n"
    "default = \"NTSC\"\n"
    "supported = [\"NTSC\"]\n";

constexpr unsigned char kOutgoingSeed = 7;
constexpr unsigned char kIncomingSeed = 9;
constexpr const char* kOutgoingId = "hub-outgoing";
constexpr const char* kIncomingId = "hub-incoming";

// 2100-01-01 and 2001-09-09: comfortably either side of any test clock.
constexpr uint64_t kFarFuture = 4102444800ull;
constexpr uint64_t kLongPast = 1000000000ull;

/// Opens `pack` under `registry` and closes it again. Returns whether the
/// pack opened at all, which is the entire C-level contract.
bool opens(const std::string& pack, const std::string& registry) {
    AyArchive* archive = ayther_pack_open_trusted(pack.c_str(), registry.c_str());
    if (archive == nullptr) return false;
    ayther_pack_close(archive);
    return true;
}

}  // namespace

int main() {
    using ayther::test::TrustScratch;

    TrustScratch scratch{"pack_trust_ffi"};

    std::string error;
    const std::string old_pack =
        scratch.bake_pack("old.ay", kOutgoingId, kOutgoingSeed, kManifest, &error);
    const std::string new_pack =
        scratch.bake_pack("new.ay", kIncomingId, kIncomingSeed, kManifest, &error);
    if (old_pack.empty() || new_pack.empty()) {
        std::printf("  [FAIL] could not bake the fixture packs: %s\n", error.c_str());
        return 1;
    }

    const std::string outgoing_live = TrustScratch::registry_entry(
        kOutgoingId, kOutgoingSeed, 0, kFarFuture, false, "\"sonic2\"");
    const std::string outgoing_retired = TrustScratch::registry_entry(
        kOutgoingId, kOutgoingSeed, 0, kLongPast, false, "\"sonic2\"");
    const std::string outgoing_revoked = TrustScratch::registry_entry(
        kOutgoingId, kOutgoingSeed, 0, kFarFuture, true, "\"sonic2\"");
    const std::string incoming_live = TrustScratch::registry_entry(
        kIncomingId, kIncomingSeed, 0, kFarFuture, false, "\"sonic2\"");

    // --- Rotation ---------------------------------------------------------
    {
        // Before the changeover only the outgoing key is published.
        const std::string before = scratch.write_registry("before.toml", outgoing_live);
        check(opens(old_pack, before), "the outgoing key opens its pack");
        check(!opens(new_pack, before),
              "a pack signed by a key the registry does not list is refused");

        // The overlap an operator publishes so nobody's pack breaks.
        const std::string overlap =
            scratch.write_registry("overlap.toml", outgoing_live + incoming_live);
        check(opens(old_pack, overlap),
              "inside the rotation window the old pack still opens");
        check(opens(new_pack, overlap),
              "inside the rotation window the new pack opens too");

        // The outgoing window has closed.
        const std::string rotated =
            scratch.write_registry("rotated.toml", outgoing_retired + incoming_live);
        check(!opens(old_pack, rotated),
              "past its window the retired key no longer opens its pack");
        check(opens(new_pack, rotated), "while the incoming key keeps working");

        // The entry removed entirely, which is where rotation ends up.
        const std::string dropped = scratch.write_registry("dropped.toml", incoming_live);
        check(!opens(old_pack, dropped),
              "a key dropped from the registry stops opening its packs");
        check(opens(new_pack, dropped), "and the surviving key is undisturbed");
    }

    // --- Revocation -------------------------------------------------------
    {
        // The operator flow: a pack opens, the registry is reissued with
        // revoked = true, and the same bytes stop opening.
        const std::string live = scratch.write_registry("revoke-before.toml", outgoing_live);
        check(opens(old_pack, live), "the pack opens under the live registry");

        const std::string revoked =
            scratch.write_registry("revoke-after.toml", outgoing_revoked);
        check(!opens(old_pack, revoked),
              "the same pack is refused once its key is revoked");

        // Revocation outranks a still-open validity window: the entry above is
        // inside [0, 2100] and is refused anyway.
        check(!opens(old_pack, revoked),
              "revocation beats an otherwise valid window");

        // Revoking one key of a pair leaves the other alone.
        const std::string mixed =
            scratch.write_registry("revoke-mixed.toml", outgoing_revoked + incoming_live);
        check(!opens(old_pack, mixed), "the revoked key stays refused beside a live one");
        check(opens(new_pack, mixed), "and the live key still opens its pack");
    }

    // --- Per-game scope ---------------------------------------------------
    {
        // The signature verifies either way; scope is a separate question, and
        // a key trusted for one title must not vouch for another.
        const std::string wrong_game = scratch.write_registry(
            "scope-wrong.toml",
            TrustScratch::registry_entry(kOutgoingId, kOutgoingSeed, 0, kFarFuture,
                                         false, "\"streets_of_rage\""));
        check(!opens(old_pack, wrong_game),
              "a key scoped to another game does not open this pack");

        const std::string right_game = scratch.write_registry(
            "scope-right.toml",
            TrustScratch::registry_entry(kOutgoingId, kOutgoingSeed, 0, kFarFuture,
                                         false, "\"sonic2\""));
        check(opens(old_pack, right_game),
              "the same key scoped to this game does open it");

        // A key listing several titles covers the one it names.
        const std::string several = scratch.write_registry(
            "scope-several.toml",
            TrustScratch::registry_entry(kOutgoingId, kOutgoingSeed, 0, kFarFuture,
                                         false, "\"streets_of_rage\", \"sonic2\""));
        check(opens(old_pack, several),
              "a multi-title scope covers a title it lists");

        const std::string wildcard = scratch.write_registry(
            "scope-wildcard.toml",
            TrustScratch::registry_entry(kOutgoingId, kOutgoingSeed, 0, kFarFuture,
                                         false, "\"*\""));
        check(opens(old_pack, wildcard), "a wildcard scope covers it");

        // Scope is not a way around rotation: the right game with a retired
        // key is still refused.
        const std::string right_game_retired = scratch.write_registry(
            "scope-retired.toml",
            TrustScratch::registry_entry(kOutgoingId, kOutgoingSeed, 0, kLongPast,
                                         false, "\"sonic2\""));
        check(!opens(old_pack, right_game_retired),
              "the correct game scope does not revive a retired key");
    }

    // --- A registry the runtime must refuse outright ----------------------
    {
        check(!opens(old_pack, scratch.path_for("absent.toml")),
              "a registry file that does not exist opens nothing");

        const std::string empty = scratch.write_registry("empty.toml", "");
        check(!opens(old_pack, empty),
              "a registry with no keys vouches for nothing");
    }

    std::printf("%s\n", failures == 0 ? "=== PASS ===" : "=== FAIL ===");
    return failures == 0 ? 0 : 1;
}
