#include <ayther/engine/capabilities.hpp>

#include <array>
#include <cstdio>
#include <thread>
#include <type_traits>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("  [%s] %s\n", condition ? " OK " : "FAIL", message);
    if (!condition) {
        ++failures;
    }
}

}  // namespace

int main() {
    static_assert(std::is_standard_layout_v<ayther::engine::Version>);
    static_assert(std::is_trivially_copyable_v<ayther::engine::Capabilities>);
    static_assert(noexcept(ayther::engine::version()));
    static_assert(noexcept(ayther::engine::core_abi_revision()));
    static_assert(noexcept(ayther::engine::probe_capabilities()));

    const auto linked_version = ayther::engine::version();
    check(ayther::engine::core_abi_revision() != 0U,
          "the linked core ABI revision is available without raw FFI");
    check(linked_version.major == AYTHER_EXPECTED_VERSION_MAJOR &&
              linked_version.minor == AYTHER_EXPECTED_VERSION_MINOR &&
              linked_version.patch == AYTHER_EXPECTED_VERSION_PATCH,
          "version comes from the linked Engine artifact");
    const auto first = ayther::engine::probe_capabilities();
    const auto second = ayther::engine::probe_capabilities();

#if AYTHER_EXPECTED_VULKAN
    check(first.renderer == ayther::engine::RendererBackend::vulkan,
          "the native artifact reports its compiled Vulkan backend");
    check(first.hardware_acceleration,
          "the compiled Vulkan backend is hardware accelerated");
#else
    check(first.renderer == ayther::engine::RendererBackend::none,
          "an artifact without a renderer reports none");
    check(!first.hardware_acceleration,
          "an artifact without a renderer reports no hardware acceleration");
#endif

    check(!first.external_image_import,
          "external image import is unavailable until an interop contract exists");
    check(first.libretro_video && first.libretro_audio,
          "the Engine artifact exposes both libretro media callback paths");
    check(first.renderer == second.renderer &&
              first.hardware_acceleration == second.hardware_acceleration &&
              first.external_image_import == second.external_image_import &&
              first.libretro_video == second.libretro_video &&
              first.libretro_audio == second.libretro_audio,
          "probing is deterministic and independent of process environment");

    constexpr std::size_t kConcurrentCallers = 8;
    std::array<ayther::engine::Capabilities, kConcurrentCallers> concurrent{};
    std::array<std::thread, kConcurrentCallers> callers{};
    for (std::size_t index = 0; index < callers.size(); ++index) {
        callers[index] = std::thread([&concurrent, index] {
            concurrent[index] = ayther::engine::probe_capabilities();
        });
    }
    for (auto& caller : callers) {
        caller.join();
    }
    bool concurrent_results_match = true;
    for (const auto& result : concurrent) {
        concurrent_results_match = concurrent_results_match &&
            result.renderer == first.renderer &&
            result.hardware_acceleration == first.hardware_acceleration &&
            result.external_image_import == first.external_image_import &&
            result.libretro_video == first.libretro_video &&
            result.libretro_audio == first.libretro_audio;
    }
    check(concurrent_results_match,
          "probing is safe and deterministic across concurrent callers");

    std::printf("%s\n", failures == 0 ? "=== PASS ===" : "=== FAIL ===");
    return failures == 0 ? 0 : 1;
}
