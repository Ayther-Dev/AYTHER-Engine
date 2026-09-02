#pragma once

#include <ayther/ayther_result.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace ayther::engine {

/// Owned metadata reported by a Libretro core.
///
/// Every string is copied while the core is loaded. No pointer returned by the
/// core crosses this boundary or depends on the lifetime of the shared library.
struct CoreInfo {
    std::uint32_t api_version{};
    std::string library_name;
    std::string library_version;
    std::string valid_extensions;
    bool need_fullpath{};
    bool block_extract{};

    /// Serializes this value as one compact JSON object.
    ///
    /// Text supplied by the core is escaped, including JSON control characters.
    [[nodiscard]] std::string serialize() const;
};

/// Move-only RAII owner used to inspect one dynamically loaded Libretro core.
///
/// The core remains loaded until this object is destroyed. The copied CoreInfo
/// remains valid independently of the library's borrowed metadata pointers.
/// Instances are created by probe_core(); moved-from instances must only be
/// destroyed or assigned a new value.
class CoreProbe {
public:
    ~CoreProbe();

    CoreProbe(const CoreProbe&) = delete;
    CoreProbe& operator=(const CoreProbe&) = delete;
    CoreProbe(CoreProbe&&) noexcept;
    CoreProbe& operator=(CoreProbe&&) noexcept;

    [[nodiscard]] const CoreInfo& info() const noexcept;
    [[nodiscard]] std::string serialize() const;

private:
    class Impl;

    CoreProbe(std::unique_ptr<Impl> impl, CoreInfo info) noexcept;

    std::unique_ptr<Impl> impl_;
    CoreInfo info_;

    friend Result<CoreProbe> probe_core(const std::filesystem::path& core_path);
};

/// Loads a core and reads its required Libretro information entry points.
///
/// ErrorCode::Io reports a platform loader failure. ErrorCode::BadFormat
/// reports a library that loaded but did not export the required Libretro
/// information symbols. The diagnostic message owns any platform error text.
/// This operation does not initialize SDL, load a ROM, or initialize the core.
[[nodiscard]] Result<CoreProbe> probe_core(
    const std::filesystem::path& core_path);

}  // namespace ayther::engine
