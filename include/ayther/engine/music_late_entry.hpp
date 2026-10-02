#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace ayther::engine {

struct LateEntryRequest {
  std::uint64_t anchor_source_frame{};
  std::uint64_t anchor_music_ns{};
  std::uint64_t effective_start_music_ns{};
  std::uint64_t verified_pause_ns{};
  std::uint32_t source_rate_hz{};
  std::uint32_t output_rate_hz{};
  bool appearance_valid{};
  bool region_valid{};
  bool membership_preserves_unrelated_effects{};
  bool pause_is_verified{};
};

enum class LateEntryStatus : std::uint8_t {
  waiting,
  ready,
  membership_unconfirmed,
};

struct LateEntryResult {
  LateEntryStatus status{LateEntryStatus::waiting};
  std::uint64_t source_offset{};
  std::uint32_t delivery_crossfade_frames{};
  bool suppress_original{};
  bool keep_hd_mix{};
  std::string diagnostic;
};

[[nodiscard]] inline LateEntryResult
calculate_late_entry(const LateEntryRequest &request) {
  if (!request.appearance_valid || !request.region_valid ||
      request.source_rate_hz == 0 || request.output_rate_hz == 0 ||
      request.effective_start_music_ns < request.anchor_music_ns)
    return {};

  auto elapsed = request.effective_start_music_ns - request.anchor_music_ns;
  if (request.pause_is_verified)
    elapsed -= std::min(elapsed, request.verified_pause_ns);
  const auto advanced = static_cast<std::uint64_t>(
      std::llround(static_cast<long double>(elapsed) * request.source_rate_hz /
                   1'000'000'000.0L));
  const auto fade = static_cast<std::uint32_t>(
      std::llround(static_cast<long double>(request.output_rate_hz) * 0.005L));
  if (!request.membership_preserves_unrelated_effects)
    return {LateEntryStatus::membership_unconfirmed,
            request.anchor_source_frame + advanced,
            fade,
            false,
            true,
            "membership_unconfirmed"};
  return {LateEntryStatus::ready,
          request.anchor_source_frame + advanced,
          fade,
          true,
          true,
          {}};
}

} // namespace ayther::engine
