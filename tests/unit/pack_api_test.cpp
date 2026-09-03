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
    const volatile std::uint8_t runtime_tiers = 0b11010U;
    const PackRenderTiers dynamic_tiers{runtime_tiers};
    check(dynamic_tiers.bits() == 0b11010U &&
              !dynamic_tiers.is_legacy() &&
              dynamic_tiers.contains(PackRenderTier::full_hd) &&
              dynamic_tiers.contains(PackRenderTier::four_k) &&
              dynamic_tiers.contains(PackRenderTier::eight_k),
          "runtime tier masks preserve every advertised bit");

    PackValidationResult clean_report;
    clean_report.findings.push_back(
        {PackFindingSeverity::warning, "warning", "non-blocking"});
    clean_report.findings.push_back(
        {PackFindingSeverity::recommendation, "recommendation", "advisory"});
    check(!clean_report.has_errors() &&
              !clean_report.findings.front().is_error(),
          "warnings and recommendations do not block a pack");
    clean_report.findings.push_back(
        {PackFindingSeverity::error, "error", "blocking"});
    check(clean_report.has_errors() && clean_report.findings.back().is_error(),
          "an error finding blocks a pack");

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

    const auto inspected_without_registry = inspect_pack(fixture.pack_path());
    check(!inspected_without_registry &&
              inspected_without_registry.error.code == ayther::ErrorCode::BadFormat,
          "ordinary inspection rejects a signed pack without its trust registry");

    const auto wrong_file = inspect_pack(fixture.registry_path());
    check(!wrong_file && wrong_file.error.code == ayther::ErrorCode::BadFormat,
          "a regular file that is not a pack is rejected as bad format");

    PackValidationContext context;
    context.platform = "megadrive";
    context.core_build_id = "test-core";
    context.rom_crc32 = 0x0000cafeU;
    context.has_rom = true;
    context.release_build = true;
    const auto validation = validate_pack(fixture.pack_path(), context);
    check(static_cast<bool>(validation), "validation returns an owned report");

    const auto empty_validation = validate_pack({});
    check(!empty_validation &&
              empty_validation.error.code == ayther::ErrorCode::NotFound,
          "validation distinguishes an empty path");
    const auto missing_validation = validate_pack("does-not-exist.ay", context);
    check((!missing_validation &&
              missing_validation.error.code == ayther::ErrorCode::BadFormat) ||
              (missing_validation && missing_validation->has_errors()),
          "validation reports a missing unreadable pack as blocking");

    auto watcher = PackWatcher::create(fixture.pack_path());
    check(static_cast<bool>(watcher), "the typed watcher owns a live handle");
    if (watcher) {
        PackWatcher moved{std::move(*watcher)};
        check(moved.is_active(), "the watcher is movable without losing ownership");
        check(!moved.poll(), "an unchanged pack produces no watcher event");
        auto replacement = PackWatcher::create(fixture.pack_path());
        check(static_cast<bool>(replacement), "a second watcher can be created");
        if (replacement.value.has_value()) {
            replacement.value.value() = std::move(moved);
            check(replacement->is_active(),
                  "move assignment transfers watcher ownership");
        }
    }

    const auto empty_watcher = PackWatcher::create({});
    check(!empty_watcher && empty_watcher.error.code == ayther::ErrorCode::NotFound,
          "a watcher rejects an empty path");

    check(!inspect_pack("does-not-exist.ay"), "inspection reports a missing pack as an error");

    std::printf("%s\n", failures == 0 ? "=== PASS ===" : "=== FAIL ===");
    return failures == 0 ? 0 : 1;
}
