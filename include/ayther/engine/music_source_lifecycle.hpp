#pragma once

#include <ayther/engine/music_audio_math.hpp>

#include <cstdint>
#include <string>

namespace ayther::engine {

enum class MusicSourceFailure : std::uint8_t { none, decoder };

struct MusicSourceSituation {
  std::uint64_t available_frames{};
  std::uint64_t required_frames{};
  bool authored_loop{};
  bool transition_pending{};
  std::uint64_t requested_fade_frames{};
  MusicSourceFailure failure{MusicSourceFailure::none};
};

enum class MusicSourceAction : std::uint8_t {
  play,
  reject,
  repeat_authored_loop,
  return_to_original,
  close_contribution,
};

struct MusicSourceDecision {
  MusicSourceAction action{MusicSourceAction::reject};
  std::uint64_t fade_frames{};
  std::string diagnostic;
  bool restart_requested{};
};

[[nodiscard]] inline MusicSourceDecision
admit_music_source(const MusicSourceSituation &source) {
  const auto fade = effective_fade_frames(source.available_frames,
                                          source.requested_fade_frames);
  if (source.failure != MusicSourceFailure::none)
    return {MusicSourceAction::close_contribution, fade,
            "asset_playback_failed", false};
  if (source.available_frames >= source.required_frames)
    return {MusicSourceAction::play, fade, {}, false};
  if (source.transition_pending && source.authored_loop)
    return {MusicSourceAction::repeat_authored_loop, fade, {}, false};
  if (source.transition_pending)
    return {MusicSourceAction::return_to_original, fade,
            "source_exhausted_before_transition", false};
  if (source.authored_loop)
    return {MusicSourceAction::play, fade, {}, false};
  return {MusicSourceAction::reject, fade, "source_too_short", false};
}

} // namespace ayther::engine
