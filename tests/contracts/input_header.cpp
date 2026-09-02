#include <ayther/engine/input.hpp>

#include <cstdint>
#include <type_traits>

using ayther::engine::RetroPadButton;
using ayther::engine::input_mask;

static_assert(std::is_same_v<std::underlying_type_t<RetroPadButton>,
                             std::uint8_t>);
static_assert(static_cast<std::uint8_t>(RetroPadButton::b) == 0U);
static_assert(static_cast<std::uint8_t>(RetroPadButton::y) == 1U);
static_assert(static_cast<std::uint8_t>(RetroPadButton::select) == 2U);
static_assert(static_cast<std::uint8_t>(RetroPadButton::start) == 3U);
static_assert(static_cast<std::uint8_t>(RetroPadButton::up) == 4U);
static_assert(static_cast<std::uint8_t>(RetroPadButton::down) == 5U);
static_assert(static_cast<std::uint8_t>(RetroPadButton::left) == 6U);
static_assert(static_cast<std::uint8_t>(RetroPadButton::right) == 7U);
static_assert(static_cast<std::uint8_t>(RetroPadButton::a) == 8U);
static_assert(static_cast<std::uint8_t>(RetroPadButton::x) == 9U);
static_assert(static_cast<std::uint8_t>(RetroPadButton::l) == 10U);
static_assert(static_cast<std::uint8_t>(RetroPadButton::r) == 11U);
static_assert(static_cast<std::uint8_t>(RetroPadButton::l2) == 12U);
static_assert(static_cast<std::uint8_t>(RetroPadButton::r2) == 13U);
static_assert(static_cast<std::uint8_t>(RetroPadButton::l3) == 14U);
static_assert(static_cast<std::uint8_t>(RetroPadButton::r3) == 15U);

static_assert(input_mask(RetroPadButton::b) == UINT16_C(0x0001));
static_assert(input_mask(RetroPadButton::y) == UINT16_C(0x0002));
static_assert(input_mask(RetroPadButton::select) == UINT16_C(0x0004));
static_assert(input_mask(RetroPadButton::start) == UINT16_C(0x0008));
static_assert(input_mask(RetroPadButton::up) == UINT16_C(0x0010));
static_assert(input_mask(RetroPadButton::down) == UINT16_C(0x0020));
static_assert(input_mask(RetroPadButton::left) == UINT16_C(0x0040));
static_assert(input_mask(RetroPadButton::right) == UINT16_C(0x0080));
static_assert(input_mask(RetroPadButton::a) == UINT16_C(0x0100));
static_assert(input_mask(RetroPadButton::x) == UINT16_C(0x0200));
static_assert(input_mask(RetroPadButton::l) == UINT16_C(0x0400));
static_assert(input_mask(RetroPadButton::r) == UINT16_C(0x0800));
static_assert(input_mask(RetroPadButton::l2) == UINT16_C(0x1000));
static_assert(input_mask(RetroPadButton::r2) == UINT16_C(0x2000));
static_assert(input_mask(RetroPadButton::l3) == UINT16_C(0x4000));
static_assert(input_mask(RetroPadButton::r3) == UINT16_C(0x8000));

static_assert((ayther::engine::InputState{ayther::engine::JoypadButton::start} |
               ayther::engine::JoypadButton::right)
                  .pressed(ayther::engine::JoypadButton::start));
