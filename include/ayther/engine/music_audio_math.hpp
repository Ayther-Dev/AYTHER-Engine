#pragma once

#include <ayther/engine/music_sequence.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace ayther::engine {

[[nodiscard]] constexpr std::uint64_t
region_frame_count(const SampleRegion region) noexcept {
  return region.valid() ? region.end - region.begin : 0;
}

[[nodiscard]] constexpr std::uint64_t
convert_frame_count(const std::uint64_t source_frames,
                    const std::uint32_t source_rate,
                    const std::uint32_t output_rate) noexcept {
  if (source_frames == 0 || source_rate == 0 || output_rate == 0)
    return 0;
  const std::uint64_t whole = source_frames / source_rate;
  const std::uint64_t remainder = source_frames % source_rate;
  if (whole > std::numeric_limits<std::uint64_t>::max() / output_rate)
    return std::numeric_limits<std::uint64_t>::max();
  const std::uint64_t fractional_numerator = remainder * output_rate;
  std::uint64_t result = whole * output_rate +
                         fractional_numerator / source_rate;
  const std::uint64_t fractional_remainder =
      fractional_numerator % source_rate;
  if (fractional_remainder >= (source_rate + 1ULL) / 2ULL)
    ++result;
  return std::max<std::uint64_t>(1, result);
}

[[nodiscard]] constexpr std::uint64_t
envelope_frames(const std::uint32_t sample_rate,
                const std::uint32_t duration_ms) noexcept {
  if (sample_rate == 0 || duration_ms == 0)
    return 0;
  const std::uint64_t numerator =
      static_cast<std::uint64_t>(sample_rate) * duration_ms;
  return std::max<std::uint64_t>(1, (numerator + 500ULL) / 1000ULL);
}

[[nodiscard]] constexpr std::uint64_t
effective_fade_frames(const std::uint64_t available_frames,
                      const std::uint64_t requested_frames) noexcept {
  return std::min(available_frames, requested_frames);
}

[[nodiscard]] constexpr double
linear_envelope(const double initial, const double final,
                const std::uint64_t elapsed_frames,
                const std::uint64_t duration_frames) noexcept {
  if (duration_frames == 0)
    return final;
  const double progress = std::clamp(
      static_cast<double>(elapsed_frames) /
          static_cast<double>(duration_frames),
      0.0, 1.0);
  return initial + (final - initial) * progress;
}

[[nodiscard]] inline bool gain_within_db(const double expected,
                                         const double actual,
                                         const double tolerance_db) noexcept {
  if (expected == 0.0)
    return actual == 0.0;
  if (actual == 0.0 || tolerance_db < 0.0)
    return false;
  const double delta =
      std::abs(20.0 * std::log10(std::abs(actual / expected)));
  return delta <= tolerance_db + 1e-12;
}

} // namespace ayther::engine
