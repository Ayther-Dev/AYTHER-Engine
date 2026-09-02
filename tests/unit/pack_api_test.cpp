#include <ayther/engine/pack.hpp>

#include "trusted_pack_fixture.h"

#include <cstdio>
#include <string>
#include <utility>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("  [%s] %s\n", condition ? " OK " : "FAIL", message);
    if (!condition) {
        ++failures;
    }
}

const char* const kManifest = "[pack]\n"
                              "name       = \"typed_pack_api\"\n"
                              "version    = \"1.0.0\"\n"
                              "game_id    = \"crc32:0000cafe\"\n"
                              "ayther_min = \"0.1.0\"\n"
                              "schema     = 2\n"
                              "output     = \"pixel-perfect\"\n"
                              "\n[systems]\n"
                              "included = [\"sprites\"]\n";

bool bake(ayther::test::TrustedPackFixture& fixture) {
    const std::string manifest = kManifest;
    char error[512]{};
    return fixture.add_bytes("manifest.toml",
                             reinterpret_cast<const std::uint8_t*>(manifest.data()),
                             manifest.size()) &&
           fixture.finish(error, sizeof(error));
}

}  // namespace

int main() {
    using namespace ayther::engine;

    const PackView empty;
    check(!empty.is_valid(), "an empty pack view is invalid");
    check(empty.info().game_id.empty(), "an empty view has empty metadata");
    check(empty.render_tiers().is_legacy(), "an empty view exposes no render tiers");

    constexpr PackRenderTiers tiers{0b00101U};
    static_assert(tiers.contains(PackRenderTier::hd));
    static_assert(tiers.contains(PackRenderTier::two_k));
    static_assert(!tiers.contains(PackRenderTier::full_hd));

    ayther::test::TrustedPackFixture fixture{"typed_pack_api"};
    if (!bake(fixture)) {
        std::printf("  [FAIL] could not bake fixture\n");
        return 1;
    }

    const auto inspected = inspect_pack(fixture.pack_path(), fixture.registry_path());
    check(static_cast<bool>(inspected), "a valid pack can be inspected");
    if (inspected) {
        check(inspected->game_id == "crc32:0000cafe", "inspection copies the game id");
        check(inspected->name == "typed_pack_api", "inspection copies the pack name");
        check(inspected->recommended_output_profile == "pixel-perfect",
              "inspection copies the output recommendation");
        check(!inspected->build_id.empty(), "inspection copies the derived build id");
    }

    PackValidationContext context;
    context.platform = "megadrive";
    const auto validation = validate_pack(fixture.pack_path(), context);
    check(static_cast<bool>(validation), "validation returns an owned report");

    auto watcher = PackWatcher::create(fixture.pack_path());
    check(static_cast<bool>(watcher), "the typed watcher owns a live handle");
    if (watcher) {
        PackWatcher moved{std::move(*watcher)};
        check(moved.is_active(), "the watcher is movable without losing ownership");
    }

    check(!inspect_pack("does-not-exist.ay"), "inspection reports a missing pack as an error");

    std::printf("%s\n", failures == 0 ? "=== PASS ===" : "=== FAIL ===");
    return failures == 0 ? 0 : 1;
}
