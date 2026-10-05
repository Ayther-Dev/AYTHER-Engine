// Spec 002, BR-020 (RF-7.2, RF-7.7): the render observation contract compiles
// alone and keeps its version and value semantics.
#include <ayther/engine/render_observer.hpp>

#include <type_traits>
#include <utility>

namespace observation = ayther::engine::render_observation;
static_assert(observation::supports({1, 0}));
static_assert(!observation::supports({2, 0}));
static_assert(observation::supports({1, 1}));
static_assert(!observation::supports({1, 2}));
static_assert(observation::max_occurrences == 256);
static_assert(observation::max_replacements == 256);
static_assert(std::is_trivially_copyable_v<observation::OccurrenceId>);
static_assert(std::is_trivially_copyable_v<observation::OccurrenceView>);
static_assert(std::is_trivially_copyable_v<observation::ReplacementView>);
static_assert(std::is_trivially_copyable_v<observation::RenderFrameView>);
static_assert(std::is_trivially_copyable_v<observation::DrawReport>);
static_assert(std::is_trivially_copyable_v<observation::LayerView>);
static_assert(std::is_abstract_v<observation::RenderObserver>);
static_assert(!std::is_copy_constructible_v<observation::RenderObserver>);
static_assert(std::is_same_v<observation::Availability,
                             ayther::engine::audio_observation::Availability>);
static_assert(
    noexcept(std::declval<observation::RenderObserver &>().on_render_frame(
        std::declval<const observation::RenderFrameView &>())));
