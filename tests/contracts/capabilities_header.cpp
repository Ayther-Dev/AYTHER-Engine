#include <ayther/engine/capabilities.hpp>

#include <type_traits>

static_assert(std::is_standard_layout_v<ayther::engine::Version>);
static_assert(std::is_trivially_copyable_v<ayther::engine::Version>);
static_assert(std::is_standard_layout_v<ayther::engine::Capabilities>);
static_assert(std::is_trivially_copyable_v<ayther::engine::Capabilities>);
