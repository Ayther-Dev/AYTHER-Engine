#pragma once

#include "audio_observation_ids.h"

#include <ayther/audio_player.h>

#include <array>
#include <optional>
#include <string_view>

namespace ayther::audio_qa {

struct FrameSampleObservationResult {
  std::optional<observation::FactId> id;
  bool complete = true;
};

class FrameSampleObservation final {
public:
  [[nodiscard]] static FrameSampleObservationResult
  emit(observation::Observer observer, IdentitySource &ids,
       const AudioPlayer::FrameSampleBoundary &boundary) noexcept {
    if (!observer.on_fact)
      return {};
    const auto id = ids.next_fact(Producer::mixer);
    if (!id)
      return {{}, false};
    const bool valid = boundary.output_position >= boundary.staged_offset;
    const std::array fields{
        known_samples("output_position", boundary.output_position),
        known_samples("staged_offset", boundary.staged_offset),
        known("sample_rate", std::uint64_t{44100}), known("valid", valid)};
    observer.observe(observation::FactView{
        *id,
        "audio_frame_sample_boundary",
        {observation::Availability::known, boundary.emulation_frame, {}},
        {},
        {},
        fields});
    return {id, valid};
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

// Spec 002 (P-9, DI-12): where an audible frame starts on the device line
// (`engine_main_output`), after rate control. Emitted from the device thread
// for every frame that reaches the device, whether or not HD voices sound.
class FrameOutputObservation final {
public:
  [[nodiscard]] static FrameSampleObservationResult
  emit(observation::Observer observer, IdentitySource &ids,
       const AudioPlayer::FrameOutputBoundary &boundary) noexcept {
    if (!observer.on_fact)
      return {};
    const auto id = ids.next_fact(Producer::postmix);
    if (!id)
      return {{}, false};
    const bool valid = boundary.complete && boundary.output_sample_rate != 0 &&
                       boundary.resample_rate_q32 != 0;
    const std::array fields{
        known("output_timeline", std::string_view{"engine_main_output"}),
        samples("output_position", boundary.output_position),
        known("sample_rate", std::uint64_t{boundary.output_sample_rate}),
        known("resample_rate_q32", boundary.resample_rate_q32),
        known("valid", valid)};
    observer.observe(observation::FactView{
        *id,
        "audio_frame_output_boundary",
        {observation::Availability::known, boundary.emulation_frame, {}},
        {},
        {},
        fields});
    return {id, valid};
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
  static observation::FieldView samples(std::string_view name,
                                        std::uint64_t value) noexcept {
    return {name,
            observation::Availability::known,
            observation::Unit::sample_frame,
            value,
            {}};
  }
};

} // namespace ayther::audio_qa
