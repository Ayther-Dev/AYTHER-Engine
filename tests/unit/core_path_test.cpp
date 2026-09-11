#include "../../tools/common/core_path.h"
#include "trust_scratch.h"

#include <cstdio>
#include <exception>
#include <fstream>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
    if (!condition)
        ++failures;
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}
} // namespace

int main() try {
    const ayther::test::TrustScratch scratch{"core_path"};
    const std::filesystem::path root{scratch.path_for("source")};
    const auto directory = root / "third_party/cores";
    std::filesystem::create_directories(directory);
    const auto lock = directory / "core.lock";
    const auto core = directory / "renamed-core.bin";
    const auto override_core = root / "override-core.bin";
    std::ofstream{lock} << "# core filename\n vram_file = renamed-core.bin # pinned\n";
    const auto missing = ayther::test::resolve_core_path(root);
    check(!missing.available() && missing.failure_exit_code == 77,
          "an absent optional locked binary is an explicit skip");
    std::ofstream{core} << "fixture";
    check(ayther::test::resolve_core_path(root).path == core,
          "the resolver follows core.lock rather than a hardcoded DLL name");
    std::ofstream{override_core} << "override";
    const auto override_name = override_core.string();
    check(ayther::test::resolve_core_path(root, override_name).path == override_core,
          "an explicit override takes precedence over the installed core");
    const auto invalid = ayther::test::resolve_core_path(root, "absent-core.bin");
    check(!invalid.available() && invalid.failure_exit_code == 1,
          "an invalid override fails without falling back to the locked core");
    std::ofstream{lock} << "vram_file = ../outside.bin\n";
    check(ayther::test::resolve_core_path(root).failure_exit_code == 1,
          "a lock entry must contain a filename");
    std::ofstream{lock} << "tag = no-filename\n";
    check(ayther::test::resolve_core_path(root).failure_exit_code == 1,
          "an incomplete lock is a configuration failure");
    return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
    std::fprintf(stderr, "[FAIL] Unexpected exception: %s\n", error.what());
    return 1;
} catch (...) {
    std::fprintf(stderr, "[FAIL] Unexpected non-standard exception\n");
    return 1;
}
