#pragma once

#include <ayther/engine/music_audio_math.hpp>
#include <ayther/engine/music_transition_queue.hpp>

#include <cstddef>
#include <cstdint>

namespace ayther::engine {

enum class TransitionCancelCause : std::uint8_t {
  external_cancel,
  external_replace,
  restore_generation,
};

struct MusicTransitionCancelDecision {
  std::size_t closed_voices{};
  std::uint64_t fade_frames{};
  bool pcm_synthesized{};
  bool destination_deferred{};
  bool resume_allowed{};
  bool transaction_boundary{};
};

[[nodiscard]] inline MusicTransitionCancelDecision cancel_music_transition(
    MusicTransitionQueue &queue, std::size_t active_voices,
    std::uint32_t sample_rate, bool paused, TransitionCancelCause cause) {
  queue.cancel();
  const bool restore = cause == TransitionCancelCause::restore_generation;
  const bool synthesize = active_voices != 0 && !paused && !restore;
  return {active_voices,
          synthesize ? envelope_frames(sample_rate, 5) : 0,
          synthesize,
          cause == TransitionCancelCause::external_replace,
          false,
          restore};
}

} // namespace ayther::engine
