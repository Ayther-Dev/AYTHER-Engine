#pragma once

#include <cstdint>

namespace ayther {

enum class AudioCategory : std::uint8_t { music, ambient, effect, voice };

enum class PlaybackState : std::uint8_t {
  inactive,
  intro,
  body,
  loop,
  paused,
  fading_out,
  ended,
};

enum class PlaybackEvent : std::uint8_t {
  same_identity_trigger,
  different_identity_trigger,
  pause,
  resume,
  loop_boundary,
  source_end,
  authored_end,
  cancel,
  fade_complete,
  terminal_error,
  host_close,
};

enum class PlaybackAction : std::uint8_t {
  continue_playback,
  restart,
  overlap,
  replace,
  pause,
  resume,
  return_to_loop,
  finish,
};

enum class PlaybackReason : std::uint8_t {
  same_identity_continuity,
  configured_restart,
  configured_overlap,
  exclusive_bus_transition,
  distinct_key_overlap,
  host_paused,
  host_resumed,
  loop_region_boundary,
  source_exhausted,
  authored_window_exhausted,
  cancelled,
  fade_exhausted,
  unrecoverable_error,
  host_closed,
  invalid_policy,
};

enum class RepeatPolicy : std::uint8_t { continue_playback, restart, overlap };
enum class TransitionPolicy : std::uint8_t { cut, fade };

struct AudioPlaybackPolicy {
  AudioCategory category = AudioCategory::effect;
  RepeatPolicy repeat = RepeatPolicy::overlap;
  TransitionPolicy transition = TransitionPolicy::cut;
  std::uint16_t max_voices = 32;
  bool exclusive_bus = false;
};

struct PlaybackInput {
  PlaybackState current = PlaybackState::inactive;
  PlaybackEvent event = PlaybackEvent::same_identity_trigger;
  PlaybackState resume_state = PlaybackState::inactive;
};

struct PlaybackDecision {
  PlaybackAction action = PlaybackAction::finish;
  PlaybackReason reason = PlaybackReason::invalid_policy;
  PlaybackState next_state = PlaybackState::ended;
  bool preserve_occurrence = false;
  bool preserve_cursor = false;
};

/// Product routes are provenance only. They may normalize local clocks and
/// identities, but never own a second playback state machine.
enum class PlaybackRoute : std::uint8_t {
  authoring,
  preview,
  runtime_replay,
};

struct PlaybackAdapterInput {
  PlaybackRoute route = PlaybackRoute::runtime_replay;
  AudioPlaybackPolicy policy{};
  PlaybackState current = PlaybackState::inactive;
  PlaybackEvent event = PlaybackEvent::same_identity_trigger;
  PlaybackState resume_state = PlaybackState::inactive;
  std::uint64_t identity = 0;
  std::uint64_t timeline_frame = 0;
  std::uint64_t source_cursor = 0;
  bool members_present = false;
  bool silent = false;
};

struct NormalizedPlayback {
  PlaybackRoute route = PlaybackRoute::runtime_replay;
  std::uint64_t identity = 0;
  std::uint64_t timeline_frame = 0;
  std::uint64_t source_cursor = 0;
  bool members_present = false;
  bool silent = false;
  PlaybackDecision decision{};
};

[[nodiscard]] constexpr AudioPlaybackPolicy
default_audio_playback_policy(AudioCategory category) noexcept {
  switch (category) {
  case AudioCategory::music:
  case AudioCategory::ambient:
    return {category, RepeatPolicy::continue_playback, TransitionPolicy::cut, 1,
            true};
  case AudioCategory::effect:
    return {category, RepeatPolicy::overlap, TransitionPolicy::cut, 32, false};
  case AudioCategory::voice:
    return {category, RepeatPolicy::restart, TransitionPolicy::cut, 8, false};
  }
  return {};
}

[[nodiscard]] constexpr bool valid(const AudioPlaybackPolicy &policy) noexcept {
  return policy.max_voices >= 1 && policy.max_voices <= 256 &&
         (!policy.exclusive_bus || policy.max_voices == 1);
}

[[nodiscard]] constexpr PlaybackDecision
decide_audio_playback(const AudioPlaybackPolicy &policy,
                      PlaybackInput input) noexcept {
  if (!valid(policy))
    return {};

  switch (input.event) {
  case PlaybackEvent::same_identity_trigger:
    switch (policy.repeat) {
    case RepeatPolicy::continue_playback:
      return {PlaybackAction::continue_playback,
              PlaybackReason::same_identity_continuity, input.current, true,
              true};
    case RepeatPolicy::restart:
      return {PlaybackAction::restart, PlaybackReason::configured_restart,
              PlaybackState::body, false, false};
    case RepeatPolicy::overlap:
      return {PlaybackAction::overlap, PlaybackReason::configured_overlap,
              PlaybackState::body, false, false};
    }
    break;
  case PlaybackEvent::different_identity_trigger:
    if (policy.exclusive_bus)
      return {PlaybackAction::replace, PlaybackReason::exclusive_bus_transition,
              PlaybackState::body, false, false};
    return {PlaybackAction::overlap, PlaybackReason::distinct_key_overlap,
            PlaybackState::body, false, false};
  case PlaybackEvent::pause:
    return {PlaybackAction::pause, PlaybackReason::host_paused,
            PlaybackState::paused, true, true};
  case PlaybackEvent::resume:
    return {PlaybackAction::resume, PlaybackReason::host_resumed,
            input.resume_state, true, true};
  case PlaybackEvent::loop_boundary:
    return {PlaybackAction::return_to_loop,
            PlaybackReason::loop_region_boundary, PlaybackState::loop, true,
            false};
  case PlaybackEvent::source_end:
    return {PlaybackAction::finish, PlaybackReason::source_exhausted,
            PlaybackState::ended, true, true};
  case PlaybackEvent::authored_end:
    return {PlaybackAction::finish, PlaybackReason::authored_window_exhausted,
            PlaybackState::ended, true, true};
  case PlaybackEvent::cancel:
    return {PlaybackAction::finish, PlaybackReason::cancelled,
            PlaybackState::ended, true, true};
  case PlaybackEvent::fade_complete:
    return {PlaybackAction::finish, PlaybackReason::fade_exhausted,
            PlaybackState::ended, true, true};
  case PlaybackEvent::terminal_error:
    return {PlaybackAction::finish, PlaybackReason::unrecoverable_error,
            PlaybackState::ended, true, true};
  case PlaybackEvent::host_close:
    return {PlaybackAction::finish, PlaybackReason::host_closed,
            PlaybackState::ended, true, true};
  }
  return {};
}

[[nodiscard]] constexpr NormalizedPlayback
normalize_audio_playback(const PlaybackAdapterInput &input) noexcept {
  return {input.route,
          input.identity,
          input.timeline_frame,
          input.source_cursor,
          input.members_present,
          input.silent,
          decide_audio_playback(
              input.policy, {input.current, input.event, input.resume_state})};
}

} // namespace ayther
