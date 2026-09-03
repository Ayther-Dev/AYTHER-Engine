#pragma once

#include <cstdint>

namespace ayther::engine {

/// Stable bit positions in the standard Libretro joypad mask consumed by
/// AytherSession::set_input(). No Libretro declaration crosses this boundary.
enum class RetroPadButton : std::uint8_t {
    b = 0,
    y = 1,
    select = 2,
    start = 3,
    up = 4,
    down = 5,
    left = 6,
    right = 7,
    a = 8,
    x = 9,
    l = 10,
    r = 11,
    l2 = 12,
    r2 = 13,
    l3 = 14,
    r3 = 15,
};

/// Converts a typed button position into the mask accepted by set_input().
[[nodiscard]] constexpr std::uint16_t input_mask(
    RetroPadButton button) noexcept {
    const auto position = static_cast<std::uint8_t>(button);
    return position < 16U
        ? static_cast<std::uint16_t>(std::uint32_t{1} << position)
        : std::uint16_t{0};
}

/// Compatibility spelling retained for the current Runtime input adapter.
using JoypadButton = RetroPadButton;

/// Type-safe per-port joypad bitmask.
class InputState final {
public:
    constexpr InputState() noexcept = default;
    constexpr InputState(JoypadButton button) noexcept
        : bits_(input_mask(button)) {}

    [[nodiscard]] static constexpr InputState from_bits(
        std::uint16_t bits) noexcept {
        return InputState(bits);
    }

    [[nodiscard]] constexpr std::uint16_t bits() const noexcept {
        return bits_;
    }

    [[nodiscard]] constexpr bool pressed(JoypadButton button) const noexcept {
        return (bits_ & input_mask(button)) != 0U;
    }

    friend constexpr InputState operator|(InputState lhs,
                                           InputState rhs) noexcept {
        return InputState(static_cast<std::uint16_t>(lhs.bits_ | rhs.bits_));
    }

private:
    explicit constexpr InputState(std::uint16_t bits) noexcept : bits_(bits) {}

    std::uint16_t bits_{};
};

}  // namespace ayther::engine
