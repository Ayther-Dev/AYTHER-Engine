#pragma once

#include "audio_observation_ids.h"

#include <ayther/audio_player.h>

#include <algorithm>
#include <array>
#include <optional>

namespace ayther::audio_qa {

struct MainOutputObservationResult {
  std::optional<observation::FactId> id;
  bool complete = true;
};

class MainOutputObservation final {
public:
  [[nodiscard]] static bool
  emit(observation::Observer observer, IdentitySource &ids,
       const AudioPlayer::MainOutputBlock &block) noexcept {
    return emit_with_id(observer, ids, block, {}).complete;
  }

  [[nodiscard]] static MainOutputObservationResult
  emit_with_id(observation::Observer observer, IdentitySource &ids,
               const AudioPlayer::MainOutputBlock &block,
               std::span<const observation::Cause> causes = {}) noexcept {
    if (!observer.on_pcm)
      return {};
    const auto id = ids.next_fact(Producer::main_output);
    const bool valid =
        id && block.bytes && block.sample_begin < block.sample_end &&
        block.sample_rate > 0 &&
        block.sample_rate <= observation::max_sample_rate &&
        block.channels > 0 && block.channels <= observation::max_channels &&
        block.byte_count == (block.sample_end - block.sample_begin) *
                                block.channels * sizeof(float) &&
        block.byte_count <= observation::max_pcm_bytes;
    if (!valid)
      return {{}, false};
    observer.observe(
        observation::PcmView{*id,
                             "sdl_logical_device_postmix",
                             {"engine_main_output", block.sample_rate,
                              block.sample_begin, block.sample_end},
                             observation::PcmFormat::f32_le,
                             block.channels,
                             {block.bytes, block.byte_count},
                             causes});
    return {id, true};
  }
};

class MainMixObservation final {
public:
  [[nodiscard]] static bool
  emit_output(observation::Observer observer, IdentitySource &ids,
              const AudioPlayer::MainMixOutputSpan &span,
              std::span<const observation::Cause> participant_causes,
              observation::FactId output_block) noexcept {
    if (!observer.on_fact)
      return true;
    const auto id = ids.next_fact(Producer::postmix);
    const bool valid = id && !participant_causes.empty() &&
                       participant_causes.size() < observation::max_causes &&
                       span.input_begin < span.input_end &&
                       span.output_begin < span.output_end &&
                       span.input_sample_rate != 0 &&
                       span.output_sample_rate != 0;
    if (!valid)
      return false;
    std::array<observation::Cause, observation::max_causes> causes{};
    std::copy(participant_causes.begin(), participant_causes.end(),
              causes.begin());
    causes[participant_causes.size()] = output_block;
    const std::array fields{
        known("submission", span.submission),
        known("input_timeline", std::string_view{"engine_main_mix"}),
        known_samples("input_begin", span.input_begin),
        known_samples("input_end", span.input_end),
        known("output_timeline", std::string_view{"engine_main_output"}),
        known_samples("output_begin", span.output_begin),
        known_samples("output_end", span.output_end),
        known("input_sample_rate", std::uint64_t{span.input_sample_rate}),
        known("output_sample_rate", std::uint64_t{span.output_sample_rate}),
        known("resample_rate_q32", span.resample_rate_q32),
        known("resample_phase_begin_q32", span.resample_phase_begin_q32),
        known("filter_support_left", std::uint64_t{span.filter_support_left}),
        known("filter_support_right", std::uint64_t{span.filter_support_right}),
        known("inserted_silence", span.inserted_silence)};
    observer.observe(observation::FactView{
        *id,
        "main_mix_output_span",
        {observation::Availability::unknown, 0, "audio_device_thread"},
        std::span{causes}.first(participant_causes.size() + 1),
        {},
        fields});
    return true;
  }

private:
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
};

} // namespace ayther::audio_qa
