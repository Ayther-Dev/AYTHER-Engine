#include <ayther/engine/core_probe.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("  [%s] %s\n", condition ? " OK " : "FAIL", message);
    if (!condition) {
        ++failures;
    }
}

#if defined(_WIN32) || defined(RTLD_NOLOAD)
bool module_is_loaded(const std::filesystem::path& path) {
#if defined(_WIN32)
    return GetModuleHandleW(path.c_str()) != nullptr;
#else
    void* handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_NOLOAD);
    if (handle == nullptr) {
        return false;
    }
    static_cast<void>(dlclose(handle));
    return true;
#endif
}
#endif

}  // namespace

int main() {
    static_assert(!std::is_copy_constructible_v<ayther::engine::CoreProbe>);
    static_assert(std::is_nothrow_move_constructible_v<ayther::engine::CoreProbe>);
    static_assert(std::is_nothrow_move_assignable_v<ayther::engine::CoreProbe>);

    auto missing = ayther::engine::probe_core(AYTHER_MISSING_CORE_PATH);
    check(!missing && missing.error.code == ayther::ErrorCode::Io,
          "a platform loader failure is returned as an owned error");

#if defined(_WIN32) || defined(RTLD_NOLOAD)
    const std::filesystem::path not_a_core_path{AYTHER_NOT_A_CORE_PATH};
    check(!module_is_loaded(not_a_core_path),
          "the invalid library starts without a loader reference");
#endif
    auto not_a_core = ayther::engine::probe_core(AYTHER_NOT_A_CORE_PATH);
    check(!not_a_core && not_a_core.error.code == ayther::ErrorCode::BadFormat,
          "a library without Libretro information symbols is rejected");
    check(not_a_core.error.message.find("retro_api_version") != std::string::npos &&
              not_a_core.error.message.find("retro_get_system_info") !=
                  std::string::npos,
          "the missing Libretro symbols are named in the diagnostic");
#if defined(_WIN32) || defined(RTLD_NOLOAD)
    check(!module_is_loaded(not_a_core_path),
          "a rejected library releases its loader reference");
#endif

#if defined(_WIN32) || defined(RTLD_NOLOAD)
    const std::filesystem::path test_core_path{AYTHER_TEST_CORE_PATH};
    check(!module_is_loaded(test_core_path),
          "the test core starts without a process loader reference");
#endif
    {
        auto probed = ayther::engine::probe_core(AYTHER_TEST_CORE_PATH);
        check(static_cast<bool>(probed), "the in-repository Libretro core loads");
        if (probed) {
#if defined(_WIN32) || defined(RTLD_NOLOAD)
            check(module_is_loaded(test_core_path),
                  "CoreProbe keeps the dynamic library loaded");
#endif
            const auto& info = probed->info();
            check(info.api_version == 1U, "the Libretro API version is reported");
            check(info.library_name == "AYTHER Test Core",
                  "the library name is copied from the core");
            check(!info.library_version.empty(),
                  "the library version is copied from the core");
            check(info.valid_extensions == "md|bin|gen",
                  "the valid extension list is copied from the core");
            check(!info.need_fullpath && info.block_extract,
                  "the remaining system-info flags are retained");

            const std::string serialized = probed->serialize();
            check(serialized.find("\"api\":1") != std::string::npos &&
                      serialized.find("\"library_name\":\"AYTHER Test Core\"") !=
                          std::string::npos &&
                      serialized.find("\"valid_extensions\":\"md|bin|gen\"") !=
                          std::string::npos,
                  "the probe serializes as one compact JSON object");

            auto moved = std::move(*probed.value);
            check(moved.info().library_name == "AYTHER Test Core",
                  "moving the RAII probe retains its owned metadata");
#if defined(_WIN32) || defined(RTLD_NOLOAD)
            check(module_is_loaded(test_core_path),
                  "the moved CoreProbe retains the loader reference");
#endif
        }
    }
#if defined(_WIN32) || defined(RTLD_NOLOAD)
    check(!module_is_loaded(test_core_path),
          "destroying CoreProbe releases the dynamic library");
#endif

    const ayther::engine::CoreInfo hostile{
        .api_version = 1U,
        .library_name = "quote\"slash\\back\bform\ffeed\ncarriage\rreturn",
        .library_version = "tab\tvalue",
        .valid_extensions = std::string{"bin\x01", 4U},
    };
    const std::string escaped = hostile.serialize();
    check(escaped.find("quote\\\"slash\\\\back\\bform\\ffeed\\ncarriage\\rreturn") !=
              std::string::npos &&
              escaped.find("tab\\tvalue") != std::string::npos &&
              escaped.find("bin\\u0001") != std::string::npos,
          "serialization escapes quotes, slashes, and every control character");

    std::printf("%s\n", failures == 0 ? "=== PASS ===" : "=== FAIL ===");
    return failures == 0 ? 0 : 1;
}
