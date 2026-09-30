#pragma once

#include "audio_observation_ids.h"

#include <ayther/audio_player.h>

#include <array>
#include <optional>

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

} // namespace ayther::audio_qa
