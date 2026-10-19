#pragma once

#include <ayther/engine/native_emulation.hpp>

namespace ayther::engine::detail {

// Owns only a native session token and the last structured result. It neither
// emulates a machine nor translates observation into the legacy core ABI.
class TesseraBackend {
public:
  TesseraBackend() = default;
  ~TesseraBackend();
  TesseraBackend(const TesseraBackend &) = delete;
  TesseraBackend &operator=(const TesseraBackend &) = delete;
  std::uint64_t handle{};
  NativeResult last;
};

} // namespace ayther::engine::detail
