#pragma once

#include "audio_observation_ids.h"

#include <ayther/audio_hd_mixer.h>

namespace ayther::audio_qa {

class PositionObservation final {
public:
  [[nodiscard]] static bool emit(observation::Observer observer,
                                 IdentitySource &ids,
                                 const HdMixer::PositionSpan &span) noexcept {
    if (!observer.on_fact)
      return true;
    const auto id = ids.next_fact(Producer::mixer);
    const bool linked =
        span.identity.cause_producer != 0 && span.identity.cause_sequence != 0;
    const bool valid = span.identity.occurrence != 0 && linked &&
                       span.output_begin < span.output_end &&
                       span.source_begin < span.source_end &&
                       span.source_end <= span.source_limit &&
                       span.output_end - span.output_begin ==
                           span.source_end - span.source_begin;
    if (!id)
      return false;
    const std::array fields{
        known("occurrence",
              observation::OccurrenceId{span.identity.occurrence}),
        known("key", span.key),
        known_samples("output_begin", span.output_begin),
        known_samples("output_end", span.output_end),
        known_samples("source_begin", span.source_begin),
        known_samples("source_end", span.source_end),
        known_samples("source_limit", span.source_limit),
        known("sample_rate", std::uint64_t{44100}),
        known("links_complete", valid)};
    std::array<observation::Cause, 1> causes{};
    if (linked)
      causes[0] = observation::FactId{span.identity.cause_producer,
                                      span.identity.cause_sequence};
    observer.observe(observation::FactView{
        *id,
        "hd_voice_position_span",
        {observation::Availability::unknown, 0, "mixer_sample_timeline_only"},
        std::span{causes}.first(linked ? 1 : 0),
        {},
        fields});
    return valid;
  }

  [[nodiscard]] static bool
  emit_loop(observation::Observer observer, IdentitySource &ids,
            const HdMixer::LoopCrossing &crossing) noexcept {
    if (!observer.on_fact)
      return true;
    const auto id = ids.next_fact(Producer::mixer);
    const bool linked = crossing.identity.cause_producer != 0 &&
                        crossing.identity.cause_sequence != 0;
    const bool valid = crossing.identity.occurrence != 0 && linked &&
                       crossing.loop_begin < crossing.loop_end &&
                       crossing.loop_end <= crossing.source_limit &&
                       crossing.source_before == crossing.loop_end &&
                       crossing.source_after == crossing.loop_begin;
    if (!id)
      return false;
    const std::array fields{
        known("occurrence",
              observation::OccurrenceId{crossing.identity.occurrence}),
        known("key", crossing.key),
        known_samples("output_position", crossing.output_position),
        known_samples("source_before", crossing.source_before),
        known_samples("source_after", crossing.source_after),
        known_samples("loop_begin", crossing.loop_begin),
        known_samples("loop_end", crossing.loop_end),
        known_samples("source_limit", crossing.source_limit),
        known("links_complete", valid)};
    std::array<observation::Cause, 1> causes{};
    if (linked)
      causes[0] = observation::FactId{crossing.identity.cause_producer,
                                      crossing.identity.cause_sequence};
    observer.observe(observation::FactView{
        *id,
        "hd_voice_loop_crossing",
        {observation::Availability::unknown, 0, "mixer_sample_timeline_only"},
        std::span{causes}.first(linked ? 1 : 0),
        {},
        fields});
    return valid;
  }

  [[nodiscard]] static bool emit_end(observation::Observer observer,
                                     IdentitySource &ids,
                                     const HdMixer::VoiceEnd &end) noexcept {
    if (!observer.on_fact)
      return true;
    const auto id = ids.next_fact(Producer::mixer);
    const bool linked =
        end.identity.cause_producer != 0 && end.identity.cause_sequence != 0;
    const bool valid = end.identity.occurrence != 0 && linked &&
                       end.source_position <= end.source_limit &&
                       (end.output_position_known != end.frame_position_known);
    if (!id)
      return false;
    std::string_view reason;
    switch (end.reason) {
    case HdMixer::EndReason::natural_end:
      reason = "natural_end";
      break;
    case HdMixer::EndReason::window_end:
      reason = "window_end";
      break;
    case HdMixer::EndReason::window_cut:
      reason = "window_cut";
      break;
    case HdMixer::EndReason::fade_complete:
      reason = "fade_complete";
      break;
    case HdMixer::EndReason::tail_complete:
      reason = "tail_complete";
      break;
    case HdMixer::EndReason::tail_cut:
      reason = "tail_cut";
      break;
    case HdMixer::EndReason::test_end:
      reason = "test_end";
      break;
    }
    const std::array fields{
        known("occurrence", observation::OccurrenceId{end.identity.occurrence}),
        known("key", end.key),
        known("reason", reason),
        known_samples("source_position", end.source_position),
        known_samples("source_limit", end.source_limit),
        end.output_position_known
            ? known_samples("output_position", end.output_position)
            : unknown("output_position", "frame_timeline_only"),
        end.frame_position_known
            ? known("frame_position", end.frame_position)
            : unknown("frame_position", "sample_timeline_only"),
        known("links_complete", valid)};
    std::array<observation::Cause, 1> causes{};
    if (linked)
      causes[0] = observation::FactId{end.identity.cause_producer,
                                      end.identity.cause_sequence};
    observer.observe(
        observation::FactView{*id,
                              "hd_voice_end",
                              {},
                              std::span{causes}.first(linked ? 1 : 0),
                              {},
                              fields});
    return valid;
  }

  [[nodiscard]] static bool
  emit_effect(observation::Observer observer, IdentitySource &ids,
              const HdMixer::LifetimeEffect &effect) noexcept {
    if (!observer.on_fact)
      return true;
    const auto id = ids.next_fact(Producer::mixer);
    const bool linked = effect.identity.cause_producer != 0 &&
                        effect.identity.cause_sequence != 0;
    const bool valid = effect.identity.occurrence != 0 && linked &&
                       (!effect.output_range_known ||
                        (effect.output_begin <= effect.output_end &&
                         effect.source_begin <= effect.source_end)) &&
                       (effect.kind != HdMixer::EffectKind::fade ||
                        effect.remaining_begin >= effect.remaining_end);
    if (!id)
      return false;
    const auto kind = effect.kind == HdMixer::EffectKind::tail
                          ? std::string_view{"tail"}
                          : std::string_view{"fade"};
    const auto stage = effect.stage == HdMixer::EffectStage::begin
                           ? std::string_view{"begin"}
                       : effect.stage == HdMixer::EffectStage::advance
                           ? std::string_view{"advance"}
                           : std::string_view{"end"};
    const auto reason = effect.kind == HdMixer::EffectKind::tail
                            ? std::string_view{"window_tail"}
                            : std::string_view{"authored_fade"};
    const std::array fields{
        known("occurrence",
              observation::OccurrenceId{effect.identity.occurrence}),
        known("key", effect.key),
        known("effect", kind),
        known("stage", stage),
        known("reason", reason),
        known_samples("source_begin", effect.source_begin),
        known_samples("source_end", effect.source_end),
        effect.output_range_known
            ? known_samples("output_begin", effect.output_begin)
            : unknown("output_begin", "frame_timeline_only"),
        effect.output_range_known
            ? known_samples("output_end", effect.output_end)
            : unknown("output_end", "frame_timeline_only"),
        effect.frame_position_known
            ? known("frame_position", effect.frame_position)
            : unknown("frame_position", "sample_timeline_only"),
        effect.kind == HdMixer::EffectKind::fade
            ? known("remaining_begin",
                    static_cast<std::uint64_t>(effect.remaining_begin))
            : unknown("remaining_begin", "not_applicable_tail"),
        effect.kind == HdMixer::EffectKind::fade
            ? known("remaining_end",
                    static_cast<std::uint64_t>(effect.remaining_end))
            : unknown("remaining_end", "not_applicable_tail"),
        known("links_complete", valid)};
    std::array<observation::Cause, 1> causes{};
    if (linked)
      causes[0] = observation::FactId{effect.identity.cause_producer,
                                      effect.identity.cause_sequence};
    observer.observe(
        observation::FactView{*id,
                              "hd_voice_lifetime_effect",
                              {},
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
  static observation::FieldView unknown(std::string_view name,
                                        std::string_view reason) noexcept {
    return {name,
            observation::Availability::unknown,
            observation::Unit::none,
            {},
            reason};
  }
};

} // namespace ayther::audio_qa
