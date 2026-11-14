#pragma once
// Native Tessera session configurations for the Engine's tests: the wire
// format Tessera expects, shared by the controls and observation tests.
#include <ayther/ayther_core_ffi.h>
#include <ayther/engine/native_emulation.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ayther::test::tessera {

using Bytes = std::vector<std::uint8_t>;
namespace native = ayther::engine;

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
inline Bytes bytes(std::string_view value) {
  return {value.begin(), value.end()};
}
inline Bytes array(const std::vector<Bytes> &values) {
  Bytes out;
  put(out, values.size(), 8);
  for (const auto &value : values) {
    put(out, value.size(), 8);
    out.insert(out.end(), value.begin(), value.end());
  }
  return out;
}
/// A session configuration in Tessera's wire format: system, backend, its
/// build id, 48 kHz audio, the control map and the hash of the initial
/// persistent memory. The synthetic backends take eight controls on one port;
/// the native megadrive machine (system 1), two pads of twelve.
inline native::NativeConfig
native_configuration(std::string_view backend_id, Bytes content, Bytes memory,
                     std::uint32_t system_override = 0) {
  std::vector<native::NativeBackendInfo> backends;
  REQUIRE(native::NativeEmulation::backends(backends).code == 0);
  const auto found =
      std::ranges::find(backends, backend_id, &native::NativeBackendInfo::id);
  REQUIRE(found != backends.end());
  native::NativeConfig result;
  result.backend_id = found->id;
  result.content = std::move(content);
  result.initial_persistent_memory = std::move(memory);
  result.system_override = system_override;
  const bool megadrive = backend_id == "megadrive";
  scalar(result.config, 1, 1, megadrive ? 1U : 0U);
  field(result.config, 2, 6, bytes(result.backend_id));
  field(result.config, 3, 5, found->build_id);
  scalar(result.config, 4, 1, megadrive ? 1U : 0U);
  scalar(result.config, 5, 1, 48000);
  std::vector<Bytes> controls;
  const unsigned ports = megadrive ? 2 : 1;
  const unsigned per_port = megadrive ? 12 : 8;
  for (unsigned index = 0; index < ports * per_port; ++index) {
    Bytes entry;
    scalar(entry, 1, 1, index / per_port);
    scalar(entry, 2, 1, index % per_port);
    scalar(entry, 3, 1, 1);
    scalar(entry, 4, 2, 0);
    scalar(entry, 5, 2, 1);
    controls.push_back(std::move(entry));
  }
  field(result.config, 6, 8, array(controls));
  std::array<char, 65> digest{};
  REQUIRE(ayther_sha256_hex(result.initial_persistent_memory.data(),
                            result.initial_persistent_memory.size(),
                            digest.data()));
  const auto nibble = [](char value) -> std::uint8_t {
    if (value >= '0' && value <= '9')
      return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f')
      return static_cast<std::uint8_t>(value - 'a' + 10);
    return static_cast<std::uint8_t>(value - 'A' + 10);
  };
  Bytes hash(32);
  for (std::size_t index = 0; index < hash.size(); ++index)
    hash[index] = static_cast<std::uint8_t>((nibble(digest[index * 2]) << 4) |
                                            nibble(digest[index * 2 + 1]));
  field(result.config, 7, 5, hash);
  return result;
}
/// The synthetic backends' content (TSSYN001) and 16 bytes of memory.
inline native::NativeConfig configuration(std::string_view backend_id,
                                          std::uint32_t flags = 0) {
  Bytes content = bytes("TSSYN001");
  for (auto value :
       {backend_id == "synthetic.complete" ? 2U : 1U, 7U, flags, 0U})
    put(content, value, 4);
  Bytes memory;
  for (unsigned index = 0; index < 16; ++index)
    memory.push_back(static_cast<std::uint8_t>(3 + 17 * index));
  return native_configuration(backend_id, std::move(content),
                              std::move(memory));
}

} // namespace ayther::test::tessera
