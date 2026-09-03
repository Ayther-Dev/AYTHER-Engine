#include <ayther/engine/core_probe.hpp>

#include <filesystem>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<ayther::engine::CoreProbe>);
static_assert(!std::is_copy_assignable_v<ayther::engine::CoreProbe>);
static_assert(std::is_nothrow_move_constructible_v<ayther::engine::CoreProbe>);
static_assert(std::is_nothrow_move_assignable_v<ayther::engine::CoreProbe>);
static_assert(std::is_same_v<
              decltype(ayther::engine::probe_core(std::filesystem::path{})),
              ayther::Result<ayther::engine::CoreProbe>>);
