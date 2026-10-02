#pragma once

#include "audio_observation_ids.h"

#include <ayther/audio_player.h>

#include <array>
#include <optional>

namespace ayther::audio_qa {

class AuxiliaryObservation final {
public:
  [[nodiscard]] static std::optional<observation::FactId>
  emit_submission(observation::Observer observer, IdentitySource &ids,
                  const AudioPlayer::AuxiliarySubmission &submission) noexcept {
    if (!observer.on_fact)
      return std::nullopt;
    const auto id = ids.next_fact(Producer::auxiliary_output);
    if (!id || submission.submission == 0 ||
        submission.input_begin >= submission.input_end)
      return std::nullopt;
    const std::array fields{
        known("submission", submission.submission),
        known("input_timeline", std::string_view{"synth_input"}),
        known_samples("input_begin", submission.input_begin),
        known_samples("input_end", submission.input_end),
        known("sample_rate", std::uint64_t{44100}),
        known("inserted_silence", submission.inserted_silence)};
    observer.observe(observation::FactView{
        *id,
        "auxiliary_submission",
        {observation::Availability::unknown, 0, "audio_stream_input"},
        {},
        {},
        fields});
    return id;
  }

  [[nodiscard]] static bool
  emit_output(observation::Observer observer, IdentitySource &ids,
              const AudioPlayer::AuxiliaryOutputSpan &span,
              observation::FactId output_block) noexcept {
    if (!observer.on_fact)
      return true;
    const auto id = ids.next_fact(Producer::auxiliary_output);
    const bool valid = id && span.observation_sequence != 0 &&
                       span.input_begin < span.input_end &&
                       span.output_begin < span.output_end &&
                       span.input_sample_rate != 0 &&
                       span.output_sample_rate != 0;
    if (!valid)
      return false;
    const std::array<observation::Cause, 2> causes{
        observation::FactId{
            static_cast<std::uint32_t>(Producer::auxiliary_output),
            span.observation_sequence},
        output_block};
    const std::array fields{
        known("submission", span.submission),
        known("input_timeline", std::string_view{"synth_input"}),
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
        "auxiliary_output_span",
        {observation::Availability::unknown, 0, "audio_device_thread"},
        causes,
        {},
        fields});
    return true;
  }

  [[nodiscard]] static bool
  emit_loss(observation::Observer observer, IdentitySource &ids,
            const AudioPlayer::AuxiliaryLoss &loss) noexcept {
    if (!observer.on_fact)
      return true;
    const auto id = ids.next_fact(Producer::auxiliary_output);
    if (!id || loss.input_begin >= loss.input_end)
      return false;
    const auto reason =
        loss.reason == AudioPlayer::AuxiliaryLossReason::discarded
            ? std::string_view{"discarded"}
            : std::string_view{"delivery_failed"};
    const std::array fields{
        known("input_timeline", std::string_view{"synth_input"}),
        known_samples("input_begin", loss.input_begin),
        known_samples("input_end", loss.input_end), known("reason", reason)};
    observer.observe(
        observation::FactView{*id,
                              "auxiliary_delivery_loss",
                              {observation::Availability::known, 0, reason},
                              {},
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
