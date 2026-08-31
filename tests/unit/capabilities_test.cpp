#include <ayther/engine/capabilities.hpp>

#include <cstdio>

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
    const auto linked_version = ayther::engine::version();
    check(linked_version.major == AYTHER_EXPECTED_VERSION_MAJOR &&
              linked_version.minor == AYTHER_EXPECTED_VERSION_MINOR &&
              linked_version.patch == AYTHER_EXPECTED_VERSION_PATCH,
          "version comes from the linked Engine artifact");
    check(linked_version.prerelease.empty(),
          "the 0.1.0 artifact has no prerelease identifier");

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

    std::printf("%s\n", failures == 0 ? "=== PASS ===" : "=== FAIL ===");
    return failures == 0 ? 0 : 1;
}
