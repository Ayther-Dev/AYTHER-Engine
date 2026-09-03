#include <ayther/engine/pack.hpp>

#include <type_traits>

using ayther::engine::PackRenderTier;
using ayther::engine::PackRenderTiers;

static_assert(static_cast<std::uint8_t>(PackRenderTier::hd) == 0U);
static_assert(static_cast<std::uint8_t>(PackRenderTier::full_hd) == 1U);
static_assert(static_cast<std::uint8_t>(PackRenderTier::two_k) == 2U);
static_assert(static_cast<std::uint8_t>(PackRenderTier::four_k) == 3U);
static_assert(static_cast<std::uint8_t>(PackRenderTier::eight_k) == 4U);

constexpr PackRenderTiers kInstalledTiers{0b00010101U};
static_assert(kInstalledTiers.contains(PackRenderTier::hd));
static_assert(kInstalledTiers.contains(PackRenderTier::two_k));
static_assert(kInstalledTiers.contains(PackRenderTier::eight_k));
static_assert(!kInstalledTiers.contains(PackRenderTier::full_hd));
static_assert(!kInstalledTiers.contains(PackRenderTier::four_k));

static_assert(std::is_trivially_copyable_v<ayther::engine::PackRenderTiers>);
static_assert(std::is_trivially_copyable_v<ayther::engine::PackView>);
static_assert(!std::is_copy_constructible_v<ayther::engine::PackWatcher>);
static_assert(std::is_nothrow_move_constructible_v<ayther::engine::PackWatcher>);
static_assert(std::is_nothrow_move_assignable_v<ayther::engine::PackWatcher>);
static_assert(noexcept(ayther::engine::PackView{}.is_valid()));
static_assert(noexcept(ayther::engine::PackValidationResult{}.has_errors()));
