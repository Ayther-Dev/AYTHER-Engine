// The session configuration of the native megadrive machine in Tessera's wire
// format (spec 008, TP-209.1): system, backend, build id, 48 kHz audio, two
// pads of twelve controls and the hash of the initial persistent memory.
// Same layout as tests/support/tessera_native_config.hpp, without doctest.
#pragma once

#include <ayther/ayther_core_ffi.h>
#include <ayther/engine/native_emulation.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ayther::parity {

namespace native_wire {
using Bytes = std::vector<std::uint8_t>;
inline void put(Bytes &out, std::uint64_t value, unsigned count) {
  for (unsigned index = 0; index < count; ++index)
    out.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
}
inline void field(Bytes &out, std::uint32_t tag, std::uint32_t encoding,
                  std::span<const std::uint8_t> data) {
  put(out, tag, 4);
  put(out, encoding, 4);
  put(out, data.size(), 8);
  out.insert(out.end(), data.begin(), data.end());
}
inline void scalar(Bytes &out, std::uint32_t tag, std::uint32_t encoding,
                   std::uint32_t value) {
  Bytes data;
  put(data, value, 4);
  field(out, tag, encoding, data);
}
} // namespace native_wire

/// The megadrive configuration for `content` (a ROM image) with `memory` as
/// the initial persistent memory; nullopt when the installed Tessera has no
/// megadrive backend (or the Engine was built without Tessera).
inline std::optional<engine::NativeConfig>
megadrive_native_config(std::vector<std::uint8_t> content,
                        std::vector<std::uint8_t> memory) {
  using namespace native_wire;
  std::vector<engine::NativeBackendInfo> backends;
  if (engine::NativeEmulation::backends(backends).code != 0)
    return std::nullopt;
  const auto found = std::ranges::find(backends, std::string_view{"megadrive"},
                                       &engine::NativeBackendInfo::id);
  if (found == backends.end())
    return std::nullopt;
  engine::NativeConfig result;
  result.backend_id = found->id;
  result.content = std::move(content);
  result.initial_persistent_memory = std::move(memory);
  result.system_override = 1; // TESSERA_SYSTEM_MEGA_DRIVE
  scalar(result.config, 1, 1, 1);
  field(result.config, 2, 6,
        Bytes(result.backend_id.begin(), result.backend_id.end()));
  field(result.config, 3, 5, found->build_id);
  scalar(result.config, 4, 1, 1);
  scalar(result.config, 5, 1, 48000);
  Bytes controls;
  put(controls, 24, 8);
  for (unsigned index = 0; index < 24; ++index) {
    Bytes entry;
    scalar(entry, 1, 1, index / 12);
    scalar(entry, 2, 1, index % 12);
    scalar(entry, 3, 1, 1);
    scalar(entry, 4, 2, 0);
    scalar(entry, 5, 2, 1);
    put(controls, entry.size(), 8);
    controls.insert(controls.end(), entry.begin(), entry.end());
  }
  field(result.config, 6, 8, controls);
  std::array<char, 65> digest{};
  if (!ayther_sha256_hex(result.initial_persistent_memory.data(),
                         result.initial_persistent_memory.size(),
                         digest.data()))
    return std::nullopt;
  const auto nibble = [](char c) -> std::uint8_t {
    if (c >= '0' && c <= '9')
      return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f')
      return static_cast<std::uint8_t>(c - 'a' + 10);
    return static_cast<std::uint8_t>(c - 'A' + 10);
  };
  Bytes hash(32);
  for (std::size_t i = 0; i < hash.size(); ++i)
    hash[i] = static_cast<std::uint8_t>((nibble(digest[i * 2]) << 4) |
                                        nibble(digest[i * 2 + 1]));
  field(result.config, 7, 5, hash);
  return result;
}

} // namespace ayther::parity
