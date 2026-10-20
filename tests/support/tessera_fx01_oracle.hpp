#pragma once

#include <ayther/engine/native_emulation.hpp>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ayther::test::tessera_fx01 {
using Bytes = std::vector<std::uint8_t>;
using View = std::span<const std::uint8_t>;

// Original MIT project test sources; adapted without calling the Tessera C API.
// AYTHER-Tessera tests/support/canonical_hash.cpp SHA-256:
// 0651aad31eb02b6f9608dbfa825b5f300f537385e8ebc611db52022424fe8558
// AYTHER-Tessera tests/support/fx01.hpp SHA-256:
// 8f90f9a2732357a9f6369f852f02c9edb5f7d52c2f54f87de26e9de1f899ef76
inline constexpr const char *oracle_provenance =
    "AYTHER-Tessera FX-01 closed forms; MIT; canonical_hash.cpp "
    "0651aad31eb02b6f9608dbfa825b5f300f537385e8ebc611db52022424fe8558; "
    "fx01.hpp 8f90f9a2732357a9f6369f852f02c9edb5f7d52c2f54f87de26e9de1f899ef76";

struct Digests {
  std::string state, video, audio;
};
struct Expected {
  Bytes state, pixels, pcm, memory;
  unsigned mask{};
};
[[nodiscard]] std::string digest(View data);
[[nodiscard]] std::string hex_bytes(View data);
[[nodiscard]] Bytes initial_memory();
[[nodiscard]] Bytes content(bool complete, unsigned flags = 0);
[[nodiscard]] Bytes configuration(const engine::NativeBackendInfo &backend);
[[nodiscard]] Bytes neutral_trace(const engine::NativeConfig &configuration,
                                  std::uint64_t frames);
[[nodiscard]] Expected expected(const engine::NativeBackendInfo &backend,
                                bool action, std::uint64_t frame);
[[nodiscard]] Digests canonical(View state, const engine::NativeFrame &frame);
void verify(const engine::NativeBackendInfo &backend, bool action,
            std::uint64_t frame, View state,
            const engine::NativeFrame &original);
} // namespace ayther::test::tessera_fx01
