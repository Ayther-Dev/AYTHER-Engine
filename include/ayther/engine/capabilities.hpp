#pragma once

#include <cstdint>

namespace ayther::engine {

/// Version embedded in the linked Engine artifact.
///
/// This is the numeric SemVer release. A release-candidate suffix belongs to
/// the distribution tag and is deliberately not duplicated in this value.
struct Version {
    std::uint32_t major;
    std::uint32_t minor;
    std::uint32_t patch;
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

/// Returns the revision of the linked Rust core C ABI.
///
/// This is a diagnostic compatibility number, not the semantic Engine version.
/// It is queried through Engine so C++ consumers never call the raw core FFI.
/// Maintainers must increment it whenever a breaking Engine/Core function or
/// data-layout contract changes.
[[nodiscard]] std::uint32_t core_abi_revision() noexcept;

/// Returns the immutable feature set compiled into the linked artifact.
///
/// This function performs no environment probing. In particular, it does not
/// load or initialize Vulkan, create a window, open an audio device, inspect a
/// registry, read configuration, or start a thread. It is safe to call
/// concurrently and never throws.
[[nodiscard]] Capabilities probe_capabilities() noexcept;

}  // namespace ayther::engine
