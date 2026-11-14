#pragma once

// Emulation generation (spec 008, D-4): which emulator produced the frames a
// session runs, so recordings, saves and packs can say where they came from and
// the Engine can refuse to mix incompatible generations.
//
//   libretro provider   id from the core's library name (`gpgx` for Genesis
//                       Plus GX), version = the core's own version string.
//   tessera provider    id `tessera`, version = hex of the backend build id.
//
// Header-only and free of I/O so it can be unit tested without a core.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ayther {

/// Determinism contract number of Genesis Plus GX (every recording up to .arp
/// v10 was made under it).
inline constexpr std::uint32_t kGpgxDeterminismContract = 1;
/// Determinism contract number of the native Tessera Mega Drive core.
inline constexpr std::uint32_t kTesseraDeterminismContract = 1;

/// Longest id / version the .arp v11 and `[emulator]` formats store.
inline constexpr std::size_t kEmulationIdMaxBytes = 32;
inline constexpr std::size_t kEmulationVersionMaxBytes = 64;

struct EmulationGeneration {
  std::string id;      ///< printable ASCII, at most 32 bytes
  std::string version; ///< printable ASCII, at most 64 bytes (may be empty)
  std::uint32_t determinism_contract = 0;

  friend bool operator==(const EmulationGeneration &,
                         const EmulationGeneration &) = default;
};

namespace detail {

inline std::string printable_ascii(std::string_view text, std::size_t limit) {
  std::string out;
  for (const char raw : text) {
    const auto ch = static_cast<unsigned char>(raw);
    if (ch >= 0x20 && ch <= 0x7E && out.size() < limit)
      out.push_back(raw);
  }
  return out;
}

inline std::string slug(std::string_view name) {
  std::string out;
  for (const char raw : name) {
    const auto ch = static_cast<unsigned char>(raw);
    if (ch >= 'A' && ch <= 'Z')
      out.push_back(static_cast<char>(ch - 'A' + 'a'));
    else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
      out.push_back(raw);
    if (out.size() == kEmulationIdMaxBytes)
      break;
  }
  return out;
}

} // namespace detail

/// Generation of a libretro core from its `retro_system_info` strings.
[[nodiscard]] inline EmulationGeneration
libretro_generation(std::string_view library_name,
                    std::string_view library_version) {
  EmulationGeneration generation;
  generation.id =
      library_name == "Genesis Plus GX" ? "gpgx" : detail::slug(library_name);
  if (generation.id.empty())
    generation.id = "libretro";
  generation.version =
      detail::printable_ascii(library_version, kEmulationVersionMaxBytes);
  generation.determinism_contract = kGpgxDeterminismContract;
  return generation;
}

/// Generation of the native Tessera provider from its backend build id.
[[nodiscard]] inline EmulationGeneration
tessera_generation(const std::array<std::uint8_t, 32> &backend_build_id) {
  static constexpr char kHex[] = "0123456789abcdef";
  EmulationGeneration generation;
  generation.id = "tessera";
  for (const std::uint8_t byte : backend_build_id) {
    generation.version.push_back(kHex[byte >> 4U]);
    generation.version.push_back(kHex[byte & 0x0FU]);
  }
  generation.determinism_contract = kTesseraDeterminismContract;
  return generation;
}

} // namespace ayther
