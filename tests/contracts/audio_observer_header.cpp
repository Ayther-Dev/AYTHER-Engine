#include <ayther/engine/audio_observer.hpp>

#include <type_traits>
#include <utility>

namespace observation = ayther::engine::audio_observation;
static_assert(observation::supports({1, 0}));
static_assert(!observation::supports({2, 0}));
static_assert(!observation::supports({1, 1}));
static_assert(std::is_standard_layout_v<observation::Observer>);
static_assert(std::is_trivially_copyable_v<observation::Observer>);
static_assert(std::is_trivially_copyable_v<observation::FactId>);
static_assert(!std::is_convertible_v<observation::OccurrenceId, std::uint64_t>);
static_assert(noexcept(std::declval<observation::Observer>().observe(
    std::declval<const observation::FactView &>())));
static_assert(noexcept(std::declval<observation::Observer>().observe(
    std::declval<const observation::PcmView &>())));
