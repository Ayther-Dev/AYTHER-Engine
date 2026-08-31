#pragma once

#include <cstdint>
#include <string_view>

namespace ayther::engine {

/// Version embedded in the linked Engine artifact.
///
/// `prerelease` refers to immutable storage owned by Engine and remains valid
/// for the lifetime of the process. Callers must not attempt to free it.
struct Version {
    std::uint32_t major;
    std::uint32_t minor;
    std::uint32_t patch;
    std::string_view prerelease;
};

enum class RendererBackend : std::uint8_t {
    none,
    vulkan,
};

/// Features compiled into the linked Engine artifact.
///
/// This value does not claim that a display, audio device, Vulkan loader, or
/// compatible GPU is present on the current machine. Device availability is
/// determined only when the owning Engine object is created.
struct Capabilities {
    RendererBackend renderer;
    bool hardware_acceleration;
    bool external_image_import;
    bool libretro_video;
    bool libretro_audio;
};

/// Returns the version embedded when the linked Engine artifact was built.
///
/// Thread-safe, allocation-free, side-effect-free, and callable before any
/// Engine object exists. This function never throws.
[[nodiscard]] Version version() noexcept;

/// Returns the immutable feature set compiled into the linked artifact.
///
/// This function performs no environment probing. In particular, it does not
/// load or initialize Vulkan, create a window, open an audio device, inspect a
/// registry, read configuration, or start a thread. It is safe to call
/// concurrently and never throws.
[[nodiscard]] Capabilities probe_capabilities() noexcept;

}  // namespace ayther::engine
