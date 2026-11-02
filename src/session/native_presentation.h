#pragma once

#include <ayther/engine/native_emulation.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

struct AyArchive;

namespace ayther::session {

enum class NativeComposeResult {
  presented,
  no_matching_rule,
  incomplete,
  unsupported,
  invalid_data,
  invalid_asset
};

NativeComposeResult compose_native_frame(
    const engine::NativeFrame &original, const engine::NativeRegion &temporal,
    const AyArchive *pack,
    const std::unordered_map<std::uint64_t, std::string> &overrides,
    std::vector<std::uint8_t> &presented);

} // namespace ayther::session
