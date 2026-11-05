#pragma once

#include <ayther/ayther_result.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

  CoreProbe(const CoreProbe &) = delete;
  CoreProbe &operator=(const CoreProbe &) = delete;
  CoreProbe(CoreProbe &&) noexcept;
  CoreProbe &operator=(CoreProbe &&) noexcept;

  [[nodiscard]] const CoreInfo &info() const noexcept;
  [[nodiscard]] std::string serialize() const;

private:
  class Impl;

  CoreProbe(std::unique_ptr<Impl> impl, CoreInfo info) noexcept;

  std::unique_ptr<Impl> impl_;
  CoreInfo info_;

  friend Result<CoreProbe> probe_core(const std::filesystem::path &core_path);
};

/// Loads a core and reads its required Libretro information entry points.
///
/// ErrorCode::Io reports a platform loader failure. ErrorCode::BadFormat
/// reports a library that loaded but did not export the required Libretro
/// information symbols. The diagnostic message owns any platform error text.
/// This operation does not initialize SDL, Vulkan, a session, or the Libretro
/// core, and it never loads content.
[[nodiscard]] Result<CoreProbe>
probe_core(const std::filesystem::path &core_path);

/// Capability mask the Engine requires from an external core (contract v2,
/// docs/CORE_PROBE_CONTRACT.md): bits 0 to 15 and 17 to 19 of `ayther_api.h`.
/// Bit 16, MODE4_CONTROLS, is not required.
inline constexpr std::uint64_t kRequiredCapabilities = 0xEFFFFULL;

/// Result of the core probe, one value per row of the contract's verdict table.
enum class ProbeVerdict {
  ok,
  missing_file,
  wrong_architecture,
  not_a_core,
  abi_missing,
  capabilities_incomplete,
  /// Set by the parent process that supervises the probe; probe_core_v2()
  /// never returns it.
  timeout,
  /// Set by the parent process when the child ended abnormally; probe_core_v2()
  /// never returns it.
  crashed,
};

/// The `verdict` text of the JSON object, for example "abi_missing".
[[nodiscard]] std::string_view verdict_name(ProbeVerdict verdict) noexcept;

/// The `reason` text for a verdict, for example "core.abi_missing". Empty for
/// ProbeVerdict::ok.
[[nodiscard]] std::string_view verdict_reason(ProbeVerdict verdict) noexcept;

/// The process exit code of the probe command for a verdict: 0 ok, 2 missing
/// file or wrong architecture, 3 not a core, 4 AYTHER ABI or capabilities, 5
/// timeout, 6 crashed. Exit code 1 is reserved for a usage error.
[[nodiscard]] int verdict_exit_code(ProbeVerdict verdict) noexcept;

/// Owned answer of probe_core_v2(). Every string is copied from the core.
struct ProbeReport {
  ProbeVerdict verdict{ProbeVerdict::not_a_core};
  /// True once the library loaded and exported the Libretro information
  /// entry points; `api`, `library_name` and `library_version` are valid.
  bool loaded{};
  std::uint32_t api{};
  std::string library_name;
  std::string library_version;
  /// "x64", "x86", "arm64" or "unknown"; empty when the file was not read.
  std::string architecture;
  /// "major.minor" of the AYTHER interface, empty when the core has none.
  std::string ayther_abi;
  std::string ayther_build_id;
  std::uint64_t capabilities{};
  /// Names of the required capabilities the core does not announce, in bit
  /// order and without the `AYTHER_CAP_` prefix.
  std::vector<std::string> missing_capabilities;
  /// Free text for logs; never parsed by a consumer.
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return verdict == ProbeVerdict::ok; }

  /// One compact JSON object of protocol version 2, without the
  /// `AYTHER_STATUS ` prefix.
  [[nodiscard]] std::string serialize() const;

  /// The line the probe command prints: `AYTHER_STATUS ` plus serialize().
  [[nodiscard]] std::string status_line() const;
};

/// Probes a core against the contract: file, architecture, Libretro entry
/// points, then the AYTHER interface and its capability mask.
///
/// The probe negotiates in process: it sets a no-op environment callback and
/// calls `retro_init`, `ayther_get_interface` and `retro_deinit`. It never
/// loads content and never initializes SDL, Vulkan or a session. A core can
/// hang or crash in those entry points, so a product runs this function in a
/// child process with a time limit (the `ayther_core_probe` tool does) and
/// never in its own process. Never throws.
[[nodiscard]] ProbeReport probe_core_v2(const std::filesystem::path &core_path);

} // namespace ayther::engine
