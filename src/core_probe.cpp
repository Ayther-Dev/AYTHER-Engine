#include <ayther/engine/core_probe.hpp>

#include "libretro_host/ayther_api.h"
#include "libretro_host/libretro.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ayther::engine {
namespace {

#if defined(_WIN32)
using NativeLibrary = HMODULE;
#else
using NativeLibrary = void *;
#endif

std::string display_path(const std::filesystem::path &path) {
  return path.generic_string();
}

std::string json_escape(std::string_view value) {
  constexpr std::array<char, 16> kHexDigits{
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
  };

  std::string escaped;
  escaped.reserve(value.size());
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      escaped += "\\\"";
      break;
    case '\\':
      escaped += "\\\\";
      break;
    case '\b':
      escaped += "\\b";
      break;
    case '\f':
      escaped += "\\f";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      if (character < 0x20U) {
        escaped += "\\u00";
        escaped.push_back(kHexDigits[character >> 4U]);
        escaped.push_back(kHexDigits[character & 0x0fU]);
      } else {
        escaped.push_back(static_cast<char>(character));
      }
      break;
    }
  }
  return escaped;
}

class DynamicLibrary final {
public:
  DynamicLibrary() = default;
  ~DynamicLibrary() { close(); }

  DynamicLibrary(const DynamicLibrary &) = delete;
  DynamicLibrary &operator=(const DynamicLibrary &) = delete;

  DynamicLibrary(DynamicLibrary &&other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}

  DynamicLibrary &operator=(DynamicLibrary &&other) noexcept {
    if (this != &other) {
      close();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] std::string open(const std::filesystem::path &path) {
#if defined(_WIN32)
    handle_ = LoadLibraryW(path.c_str());
    if (handle_ == nullptr) {
      const DWORD error_code = GetLastError();
      return "LoadLibraryW failed for '" + display_path(path) +
             "' with Windows error " + std::to_string(error_code);
    }
#else
    dlerror(); // NOLINT(concurrency-mt-unsafe): POSIX specifies per-thread
               // error state.
    handle_ = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (handle_ == nullptr) {
      // POSIX specifies that dlerror state is local to the calling thread.
      const char *error = dlerror(); // NOLINT(concurrency-mt-unsafe)
      return "dlopen failed for '" + display_path(path) +
             "': " + (error != nullptr ? error : "unknown platform error");
    }
#endif
    return {};
  }

  template <typename FunctionPointer>
  [[nodiscard]] FunctionPointer symbol(const char *name) const noexcept {
#if defined(_WIN32)
    return reinterpret_cast<FunctionPointer>(GetProcAddress(handle_, name));
#else
    return reinterpret_cast<FunctionPointer>(dlsym(handle_, name));
#endif
  }

private:
  void close() noexcept {
    if (handle_ == nullptr) {
      return;
    }
#if defined(_WIN32)
    static_cast<void>(FreeLibrary(handle_));
#else
    static_cast<void>(dlclose(handle_));
#endif
    handle_ = nullptr;
  }

  NativeLibrary handle_{};
};

} // namespace

class CoreProbe::Impl final {
public:
  explicit Impl(DynamicLibrary library) noexcept
      : library_(std::move(library)) {}

private:
  DynamicLibrary library_;
};

std::string CoreInfo::serialize() const {
  std::string json = "{\"api\":" + std::to_string(api_version);
  json += ",\"library_name\":\"" + json_escape(library_name) + '"';
  json += ",\"library_version\":\"" + json_escape(library_version) + '"';
  json += ",\"valid_extensions\":\"" + json_escape(valid_extensions) + '"';
  json += ",\"need_fullpath\":";
  json += need_fullpath ? "true" : "false";
  json += ",\"block_extract\":";
  json += block_extract ? "true" : "false";
  json += '}';
  return json;
}

CoreProbe::CoreProbe(std::unique_ptr<Impl> impl, CoreInfo info) noexcept
    : impl_(std::move(impl)), info_(std::move(info)) {}

CoreProbe::~CoreProbe() = default;
CoreProbe::CoreProbe(CoreProbe &&) noexcept = default;
CoreProbe &CoreProbe::operator=(CoreProbe &&) noexcept = default;

const CoreInfo &CoreProbe::info() const noexcept { return info_; }

std::string CoreProbe::serialize() const { return info_.serialize(); }

namespace {

// A library that exports the Libretro information entry points, with the
// metadata already copied out of it.
struct LoadedCore {
  DynamicLibrary library;
  CoreInfo info;
};

Result<LoadedCore> load_libretro_core(const std::filesystem::path &core_path) {
  DynamicLibrary library;
  if (const std::string load_error = library.open(core_path);
      !load_error.empty()) {
    return Error{ErrorCode::Io, load_error};
  }

  using ApiVersionFunction = unsigned (*)(void);
  using SystemInfoFunction = void (*)(retro_system_info *);
  const auto api_version =
      library.symbol<ApiVersionFunction>("retro_api_version");
  const auto system_info =
      library.symbol<SystemInfoFunction>("retro_get_system_info");
  if (api_version == nullptr || system_info == nullptr) {
    std::string missing;
    if (api_version == nullptr) {
      missing = "retro_api_version";
    }
    if (system_info == nullptr) {
      if (!missing.empty()) {
        missing += ", ";
      }
      missing += "retro_get_system_info";
    }
    return Error{
        ErrorCode::BadFormat,
        "The loaded library is not a Libretro core; missing symbol(s): " +
            missing,
    };
  }

  retro_system_info raw_info{};
  system_info(&raw_info);
  CoreInfo info{
      .api_version = api_version(),
      .library_name =
          raw_info.library_name != nullptr ? raw_info.library_name : "",
      .library_version =
          raw_info.library_version != nullptr ? raw_info.library_version : "",
      .valid_extensions =
          raw_info.valid_extensions != nullptr ? raw_info.valid_extensions : "",
      .need_fullpath = raw_info.need_fullpath,
      .block_extract = raw_info.block_extract,
  };
  return LoadedCore{std::move(library), std::move(info)};
}

} // namespace

Result<CoreProbe> probe_core(const std::filesystem::path &core_path) {
  auto loaded = load_libretro_core(core_path);
  if (!loaded) {
    return loaded.error;
  }
  return CoreProbe{
      std::make_unique<CoreProbe::Impl>(std::move(loaded->library)),
      std::move(loaded->info),
  };
}

// ---------------------------------------------------------------------------
// Probe contract v2
// ---------------------------------------------------------------------------

std::string_view verdict_name(ProbeVerdict verdict) noexcept {
  switch (verdict) {
  case ProbeVerdict::ok:
    return "ok";
  case ProbeVerdict::missing_file:
    return "missing_file";
  case ProbeVerdict::wrong_architecture:
    return "wrong_architecture";
  case ProbeVerdict::not_a_core:
    return "not_a_core";
  case ProbeVerdict::abi_missing:
    return "abi_missing";
  case ProbeVerdict::capabilities_incomplete:
    return "capabilities_incomplete";
  case ProbeVerdict::timeout:
    return "timeout";
  case ProbeVerdict::crashed:
    return "crashed";
  }
  return "not_a_core";
}

std::string_view verdict_reason(ProbeVerdict verdict) noexcept {
  switch (verdict) {
  case ProbeVerdict::ok:
    return "";
  case ProbeVerdict::missing_file:
    return "core.missing";
  case ProbeVerdict::wrong_architecture:
    return "core.wrong_architecture";
  case ProbeVerdict::not_a_core:
    return "core.invalid";
  case ProbeVerdict::abi_missing:
    return "core.abi_missing";
  case ProbeVerdict::capabilities_incomplete:
    return "core.capabilities_incomplete";
  case ProbeVerdict::timeout:
    return "core.probe_timeout";
  case ProbeVerdict::crashed:
    return "core.probe_crashed";
  }
  return "core.invalid";
}

int verdict_exit_code(ProbeVerdict verdict) noexcept {
  switch (verdict) {
  case ProbeVerdict::ok:
    return 0;
  case ProbeVerdict::missing_file:
  case ProbeVerdict::wrong_architecture:
    return 2;
  case ProbeVerdict::not_a_core:
    return 3;
  case ProbeVerdict::abi_missing:
  case ProbeVerdict::capabilities_incomplete:
    return 4;
  case ProbeVerdict::timeout:
    return 5;
  case ProbeVerdict::crashed:
    return 6;
  }
  return 3;
}

namespace {

// Capability names by bit, `AYTHER_CAP_` prefix removed. Bit 16 is named even
// though it is not required, so a future mask change needs no new table.
// clang-format off
constexpr std::array<std::string_view, 20> kCapabilityNames{
    "LEGACY_MEMORY", "REGION_QUERY", "REGION_READ", "CONTROL_WRITE",
    "FRAME_SNAPSHOT", "PARSED_SPRITES_V1", "AUDIO_WRITES_V1",
    "RASTER_FALLBACK_V1", "RECOMPOSE_V1", "AUDIO_PROBE_V1",
    "SUBSCRIPTIONS_V1", "FRAME_DELTA_V1", "RECOMPOSE_STATS_V1",
    "ATTRIBUTION_V1", "FRAME_DELTA_SINCE_V1", "SYSTEM_V1",
    "MODE4_CONTROLS", "LINE_STATE_V1", "OBSERVABILITY_V1",
    "SPRITE_OUTCOME_V1",
};
// clang-format on

constexpr std::uint32_t kMinimumAytherMinor = 10;

#if defined(_M_X64) || defined(__x86_64__)
constexpr std::string_view kHostArchitecture = "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
constexpr std::string_view kHostArchitecture = "arm64";
#elif defined(_M_IX86) || defined(__i386__)
constexpr std::string_view kHostArchitecture = "x86";
#else
constexpr std::string_view kHostArchitecture = "unknown";
#endif

std::uint32_t read_le(const std::array<unsigned char, 64> &bytes,
                      std::size_t offset, std::size_t width) {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < width; ++i) {
    value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8U * i);
  }
  return value;
}

/// Architecture of a PE or ELF file read from its headers, without loading it.
/// Returns an empty string for any other file: the loader then decides.
std::string binary_architecture(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  std::array<unsigned char, 64> head{};
  file.read(reinterpret_cast<char *>(head.data()),
            static_cast<std::streamsize>(head.size()));
  if (static_cast<std::size_t>(file.gcount()) < head.size()) {
    return {};
  }

  if (head[0] == 0x7F && head[1] == 'E' && head[2] == 'L' && head[3] == 'F') {
    switch (read_le(head, 18, 2)) {
    case 62:
      return "x64";
    case 183:
      return "arm64";
    case 3:
      return "x86";
    default:
      return "unknown";
    }
  }

  if (head[0] == 'M' && head[1] == 'Z') {
    const std::uint32_t pe_offset = read_le(head, 0x3C, 4);
    std::array<unsigned char, 64> pe{};
    file.clear();
    file.seekg(static_cast<std::streamoff>(pe_offset), std::ios::beg);
    file.read(reinterpret_cast<char *>(pe.data()), 6);
    if (file.gcount() < 6 || pe[0] != 'P' || pe[1] != 'E' || pe[2] != 0 ||
        pe[3] != 0) {
      return {};
    }
    switch (read_le(pe, 4, 2)) {
    case 0x8664:
      return "x64";
    case 0xAA64:
      return "arm64";
    case 0x014C:
      return "x86";
    default:
      return "unknown";
    }
  }
  return {};
}

bool null_environment(unsigned /*cmd*/, void * /*data*/) { return false; }

std::string hex_mask(std::uint64_t mask) {
  std::array<char, 24> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "0x%llX",
                static_cast<unsigned long long>(mask));
  return buffer.data();
}

ProbeReport probe_loaded_core(ProbeReport report, LoadedCore &core) {
  report.loaded = true;
  report.api = core.info.api_version;
  report.library_name = core.info.library_name;
  report.library_version = core.info.library_version;

  using SetEnvironment = void (*)(retro_environment_t);
  using LifeCycle = void (*)(void);
  const auto get_interface =
      core.library.symbol<ayther_get_interface_fn>("ayther_get_interface");
  if (get_interface == nullptr) {
    report.verdict = ProbeVerdict::abi_missing;
    report.message = "The core does not export ayther_get_interface";
    return report;
  }

  // The order a frontend follows: environment, init, negotiate, deinit. No
  // content is loaded.
  const auto set_environment =
      core.library.symbol<SetEnvironment>("retro_set_environment");
  const auto init = core.library.symbol<LifeCycle>("retro_init");
  const auto deinit = core.library.symbol<LifeCycle>("retro_deinit");
  if (set_environment != nullptr) {
    set_environment(&null_environment);
  }
  if (init != nullptr) {
    init();
  }
  const ayther_interface_v1 *iface = get_interface(0);
  if (iface == nullptr) {
    if (deinit != nullptr) {
      deinit();
    }
    report.verdict = ProbeVerdict::abi_missing;
    report.message = "ayther_get_interface returned no interface";
    return report;
  }

  const std::uint32_t version = iface->abi_version;
  report.ayther_abi =
      std::to_string(version >> 16U) + '.' + std::to_string(version & 0xFFFFU);
  if (AYTHER_IFACE_HAS(iface, capabilities)) {
    report.capabilities = iface->capabilities;
  }
  if (AYTHER_IFACE_HAS(iface, build_id_size) && iface->build_id != nullptr) {
    constexpr std::uint32_t kMaxBuildId = 256;
    report.ayther_build_id.assign(
        iface->build_id, (std::min)(iface->build_id_size, kMaxBuildId));
  }
  if (deinit != nullptr) {
    deinit();
  }

  const std::uint64_t missing = kRequiredCapabilities & ~report.capabilities;
  for (std::size_t bit = 0; bit < kCapabilityNames.size(); ++bit) {
    if ((missing & (std::uint64_t{1} << bit)) != 0) {
      report.missing_capabilities.emplace_back(kCapabilityNames[bit]);
    }
  }

  const bool abi_too_old =
      (version >> 16U) != 1U || (version & 0xFFFFU) < kMinimumAytherMinor;
  if (abi_too_old || missing != 0) {
    report.verdict = ProbeVerdict::capabilities_incomplete;
    if (abi_too_old) {
      report.message = "The AYTHER interface " + report.ayther_abi +
                       " is older than 1." +
                       std::to_string(kMinimumAytherMinor);
    }
    return report;
  }
  report.verdict = ProbeVerdict::ok;
  return report;
}

} // namespace

std::string ProbeReport::serialize() const {
  std::string json = "{\"protocol_version\":2,\"event\":\"probe\",\"ok\":";
  json += ok() ? "true" : "false";
  json += ",\"verdict\":\"" + std::string{verdict_name(verdict)} + '"';
  if (!ok()) {
    json += ",\"reason\":\"" + std::string{verdict_reason(verdict)} + '"';
  }
  if (loaded) {
    json += ",\"api\":" + std::to_string(api);
    json += ",\"library_name\":\"" + json_escape(library_name) + '"';
    json += ",\"library_version\":\"" + json_escape(library_version) + '"';
  }
  if (!architecture.empty()) {
    json += ",\"architecture\":\"" + json_escape(architecture) + '"';
  }
  if (!ayther_abi.empty()) {
    json += ",\"ayther_abi\":\"" + json_escape(ayther_abi) + '"';
    json += ",\"ayther_build_id\":\"" + json_escape(ayther_build_id) + '"';
    json += ",\"capabilities\":\"" + hex_mask(capabilities) + '"';
    json +=
        ",\"required_capabilities\":\"" + hex_mask(kRequiredCapabilities) + '"';
    json += ",\"missing_capabilities\":[";
    for (std::size_t i = 0; i < missing_capabilities.size(); ++i) {
      if (i != 0) {
        json += ',';
      }
      json += '"' + json_escape(missing_capabilities[i]) + '"';
    }
    json += ']';
  }
  if (!message.empty()) {
    json += ",\"message\":\"" + json_escape(message) + '"';
  }
  json += '}';
  return json;
}

std::string ProbeReport::status_line() const {
  return "AYTHER_STATUS " + serialize();
}

ProbeReport probe_core_v2(const std::filesystem::path &core_path) {
  ProbeReport report;
  try {
    std::error_code error;
    if (!std::filesystem::is_regular_file(core_path, error)) {
      report.verdict = ProbeVerdict::missing_file;
      report.message = "No file at '" + display_path(core_path) + "'";
      return report;
    }

    report.architecture = binary_architecture(core_path);
    if (!report.architecture.empty() &&
        report.architecture != kHostArchitecture) {
      report.verdict = ProbeVerdict::wrong_architecture;
      report.message = "The core is " + report.architecture +
                       " and this process is " + std::string{kHostArchitecture};
      return report;
    }
    if (report.architecture.empty()) {
      report.architecture = std::string{kHostArchitecture};
    }

    auto loaded = load_libretro_core(core_path);
    if (!loaded) {
      report.verdict = ProbeVerdict::not_a_core;
      report.message = loaded.error.message;
      return report;
    }
    return probe_loaded_core(std::move(report), *loaded);
  } catch (const std::exception &failure) {
    report.verdict = ProbeVerdict::not_a_core;
    report.message = failure.what();
    return report;
  }
}

} // namespace ayther::engine
