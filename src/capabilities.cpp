#include <ayther/engine/capabilities.hpp>

#include "ayther_core_ffi.h"
#include "ayther_version.h"

namespace ayther::engine {

Version version() noexcept {
    return Version{
        AYTHER_VERSION_MAJOR,
        AYTHER_VERSION_MINOR,
        AYTHER_VERSION_PATCH,
    };
}

std::uint32_t core_abi_revision() noexcept {
    return ayther_core_version();
}

Capabilities probe_capabilities() noexcept {
#if defined(AYTHER_ENGINE_HAS_VULKAN)
    constexpr auto kRenderer = RendererBackend::vulkan;
    constexpr bool kHardwareAcceleration = true;
#else
    constexpr auto kRenderer = RendererBackend::none;
    constexpr bool kHardwareAcceleration = false;
#endif

#if defined(AYTHER_ENGINE_HAS_LIBRETRO)
    constexpr bool kLibretroMedia = true;
#else
    constexpr bool kLibretroMedia = false;
#endif

    return Capabilities{
        .renderer = kRenderer,
        .hardware_acceleration = kHardwareAcceleration,
        .external_image_import = false,
        .libretro_video = kLibretroMedia,
        .libretro_audio = kLibretroMedia,
    };
}

}  // namespace ayther::engine
