#pragma once

#include "audio_observation_ids.h"

#include <ayther/audio_player.h>

#include <array>
#include <optional>

namespace ayther::audio_qa {

class OriginalAudioObservation final {
public:
  [[nodiscard]] static bool
  emit(observation::Observer observer, IdentitySource &ids,
       const AudioPlayer::OriginalAudioSpan &span,
       std::optional<observation::FactId> frame_boundary =
           std::nullopt) noexcept {
    if (!observer.on_fact)
      return true;
    const auto id = ids.next_fact(Producer::mixer);
    const bool has_range = span.output_begin < span.output_end;
    const bool absent = span.state == AudioPlayer::OriginalAudioState::absent;
    const bool unknown = span.state == AudioPlayer::OriginalAudioState::unknown;
    const bool valid =
        has_range &&
        (unknown ? !span.observation_complete : span.observation_complete) &&
        (absent == !span.hash_known) &&
        (!span.hash_known || span.hash != AudioPlayer::kRouterHash) &&
        (!span.frame_known ||
         (frame_boundary && span.frame_boundary <= span.output_begin));
    if (!id)
      return false;
    const std::array fields{
        known("mix_timeline", std::string_view{"engine_main_mix"}),
        known_samples("mix_begin", span.output_begin),
        known_samples("mix_end", span.output_end),
        span.frame_known
            ? known_samples("frame_boundary", span.frame_boundary)
            : unknown_samples("frame_boundary", "emulation_frame_unobserved"),
        known("sample_rate", std::uint64_t{44100}),
        span.hash_known ? known("source_hash", span.hash)
                        : not_applicable("source_hash", "no_native_batch"),
        known("state", state_name(span.state)),
        known("reason", reason_name(span.reason)),
        known("observation_complete", span.observation_complete),
        known("valid", valid)};
    const std::array<observation::Cause, 1> causes{
        frame_boundary.value_or(observation::FactId{})};
    observer.observe(observation::FactView{
        *id,
        "original_audio_span",
        span.frame_known
            ? observation::FramePosition{observation::Availability::known,
                                         span.emulation_frame,
                                         {}}
            : observation::FramePosition{observation::Availability::unknown, 0,
                                         "emulation_frame_unobserved"},
        std::span<const observation::Cause>{causes}.first(frame_boundary ? 1
                                                                         : 0),
        {},
        fields});
    return valid;
  }

private:
  static std::string_view
  state_name(AudioPlayer::OriginalAudioState state) noexcept {
    switch (state) {
    case AudioPlayer::OriginalAudioState::present:
      return "present";
    case AudioPlayer::OriginalAudioState::suppressed:
      return "suppressed";
    case AudioPlayer::OriginalAudioState::absent:
      return "absent";
    case AudioPlayer::OriginalAudioState::unknown:
      return "unknown";
    }
    return "unknown";
  }
  static std::string_view
  reason_name(AudioPlayer::OriginalAudioReason reason) noexcept {
    switch (reason) {
    case AudioPlayer::OriginalAudioReason::unmuted:
      return "unmuted";
    case AudioPlayer::OriginalAudioReason::explicit_suppression:
      return "explicit_suppression";
    case AudioPlayer::OriginalAudioReason::matched_hash:
      return "matched_hash";
    case AudioPlayer::OriginalAudioReason::no_native_batch:
      return "no_native_batch";
    case AudioPlayer::OriginalAudioReason::capacity_exceeded:
      return "capacity_exceeded";
    }
    return "capacity_exceeded";
  }
  static observation::FieldView known(std::string_view name,
                                      observation::Value value) noexcept {
    return {name,
            observation::Availability::known,
            observation::Unit::none,
            value,
            {}};
  }
  static observation::FieldView known_samples(std::string_view name,
                                              std::uint64_t value) noexcept {
    return {name,
            observation::Availability::known,
            observation::Unit::sample_frame,
            value,
            {}};
  }
  static observation::FieldView
  unknown_samples(std::string_view name, std::string_view reason) noexcept {
    return {name,
            observation::Availability::unknown,
            observation::Unit::sample_frame,
            {},
            reason};
  }
  static observation::FieldView
  not_applicable(std::string_view name, std::string_view reason) noexcept {
    return {name,
            observation::Availability::not_applicable,
            observation::Unit::none,
            {},
            reason};
  }
};

} // namespace ayther::audio_qa
