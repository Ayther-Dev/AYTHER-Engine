#include <ayther/engine/engine.hpp>

#include <type_traits>

static_assert(std::is_same_v<decltype(ayther::engine::version()),
                             ayther::engine::Version>);
static_assert(std::is_same_v<decltype(ayther::engine::probe_capabilities()),
                             ayther::engine::Capabilities>);
