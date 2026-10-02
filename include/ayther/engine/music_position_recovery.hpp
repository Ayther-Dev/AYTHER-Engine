#pragma once

#include <ayther/engine/music_sequence.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace ayther::engine {

enum class PositionRecoveryCertainty : std::uint8_t {
  confirmed,
  unconfirmed,
};

struct MusicPositionRecoveryState {
  PositionRecoveryCertainty certainty{PositionRecoveryCertainty::confirmed};
  std::string diagnostic;
  bool position_transitions_suspended{};
  bool hd_course_continues{true};
  bool seek_requested{};
};

class MusicPositionRecovery {
public:
  MusicPositionRecovery(AppearanceId expected_appearance,
                        std::uint64_t expected_position,
                        std::uint64_t tolerance) noexcept
      : expected_appearance_(expected_appearance),
        expected_position_(expected_position), tolerance_(tolerance) {}

  [[nodiscard]] MusicPositionRecoveryState
  observe(AppearanceId appearance,
          std::optional<std::uint64_t> observed_position,
          bool member_present) noexcept {
    if (!member_present && !observed_position)
      return state();

    const bool same_appearance = appearance == expected_appearance_;
    const bool within_tolerance =
        observed_position &&
        distance(*observed_position, expected_position_) <= tolerance_;
    certainty_ = same_appearance && within_tolerance
                     ? PositionRecoveryCertainty::confirmed
                     : PositionRecoveryCertainty::unconfirmed;
    return state();
  }

private:
  [[nodiscard]] static constexpr std::uint64_t
  distance(std::uint64_t left, std::uint64_t right) noexcept {
    return left >= right ? left - right : right - left;
  }

  [[nodiscard]] MusicPositionRecoveryState state() const {
    const bool unconfirmed =
        certainty_ == PositionRecoveryCertainty::unconfirmed;
    return {certainty_, unconfirmed ? "position_unconfirmed" : std::string{},
            unconfirmed, true, false};
  }

  AppearanceId expected_appearance_{};
  std::uint64_t expected_position_{};
  std::uint64_t tolerance_{};
  PositionRecoveryCertainty certainty_{PositionRecoveryCertainty::confirmed};
};

} // namespace ayther::engine
