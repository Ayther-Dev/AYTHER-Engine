#pragma once

#include "audio_observation_ids.h"

#include <ayther/audio_hd_mixer.h>

#include <array>
#include <cmath>

namespace ayther::audio_qa {

class MixObservation final {
public:
  [[nodiscard]] static bool
  emit_participant(observation::Observer observer, IdentitySource &ids,
                   const HdMixer::PositionSpan &span) noexcept {
    if (!observer.on_fact)
      return true;
    const auto id = ids.next_fact(Producer::mixer);
    const bool linked =
        span.identity.cause_producer != 0 && span.identity.cause_sequence != 0;
    const bool valid =
        span.identity.occurrence != 0 && linked &&
        span.output_begin < span.output_end &&
        span.source_begin < span.source_end &&
        span.source_end <= span.source_limit &&
        span.output_end - span.output_begin ==
            span.source_end - span.source_begin &&
        std::isfinite(span.effective_gain_begin) &&
        std::isfinite(span.effective_gain_end) &&
        (!span.muted_by_gain ||
         (!span.nonzero_contribution && span.effective_gain_begin == 0.0F &&
          span.effective_gain_end == 0.0F));
    if (!id)
      return false;
    const std::array fields{
        known("occurrence",
              observation::OccurrenceId{span.identity.occurrence}),
        known("key", span.key),
        known("mix_timeline", std::string_view{"engine_main_mix"}),
        known_samples("mix_begin", span.output_begin),
        known_samples("mix_end", span.output_end),
        known("mix_sample_rate", std::uint64_t{44100}),
        known("track_timeline", std::string_view{"hd_asset_pcm"}),
        known_samples("track_begin", span.source_begin),
        known_samples("track_end", span.source_end),
        known_samples("track_limit", span.source_limit),
        known("track_sample_rate", std::uint64_t{44100}),
        known_gain("effective_gain_begin", span.effective_gain_begin),
        known_gain("effective_gain_end", span.effective_gain_end),
        known("muted_by_gain", span.muted_by_gain),
        known("nonzero_contribution", span.nonzero_contribution),
        known("links_complete", valid)};
    std::array<observation::Cause, 1> causes{};
    if (linked)
      causes[0] = observation::FactId{span.identity.cause_producer,
                                      span.identity.cause_sequence};
    observer.observe(observation::FactView{
        *id,
        "hd_mix_participant",
        {observation::Availability::unknown, 0, "mixer_sample_timeline_only"},
        std::span{causes}.first(linked ? 1 : 0),
        {},
        fields});
    return valid;
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
  static observation::FieldView known_gain(std::string_view name,
                                           float value) noexcept {
    return {name,
            observation::Availability::known,
            observation::Unit::linear_gain,
            static_cast<double>(value),
            {}};
  }
};

} // namespace ayther::audio_qa
