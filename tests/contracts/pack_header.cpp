#include <ayther/engine/pack.hpp>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<ayther::engine::PackRenderTiers>);
static_assert(std::is_trivially_copyable_v<ayther::engine::PackView>);
static_assert(!std::is_copy_constructible_v<ayther::engine::PackWatcher>);
static_assert(std::is_nothrow_move_constructible_v<ayther::engine::PackWatcher>);
static_assert(noexcept(ayther::engine::PackView{}.is_valid()));
static_assert(noexcept(ayther::engine::PackValidationResult{}.has_errors()));
