#pragma once

#include "audio_observation_ids.h"
#include <ayther/ayther_core_ffi.h>

#include <array>
#include <span>

namespace ayther::audio_qa {

enum class DetectorPath { live, analysis };
enum class WriteOrigin { abi_snapshot, legacy_core };

// Borrow exactly the arrays about to be passed to the detector. Observation
// does not poll a source, decode a write, or invoke the detector itself.
struct DetectorInput {
  std::uint64_t session_frame = 0;
  std::uint32_t detector_frame = 0;
  DetectorPath detector = DetectorPath::live;
  WriteOrigin write_origin = WriteOrigin::legacy_core;
  std::span<const AytherAudioWrite> writes;
  std::span<const AytherPcmEvent> pcm;
};

struct InputObservation {
  std::optional<observation::FactId> batch;
  bool complete = true;
};

namespace input_detail {
inline observation::FieldView
known(std::string_view name, observation::Value value,
      observation::Unit unit = observation::Unit::none) noexcept {
  return {name, observation::Availability::known, unit, value, {}};
}

inline std::string_view event_type(std::uint8_t kind) noexcept {
  switch (kind) {
  case AYTHER_PCM_KEY_ON:
    return "key_on";
  case AYTHER_PCM_KEY_OFF:
    return "key_off";
  case AYTHER_PCM_PITCH:
    return "pitch";
  case AYTHER_PCM_VOLUME:
    return "volume";
  default:
    return "unknown";
  }
}
} // namespace input_detail

// Constant working storage; O(writes + pcm), with no allocation or retained
// history. Producer IDs express emission order, not cross-stream causality.
[[nodiscard]] inline InputObservation
observe_detector_input(observation::Observer observer,
                       IdentitySource &identities,
                       const DetectorInput &input) noexcept {
  using input_detail::known;
  using observation::Cause;
  using observation::Unit;
  InputObservation result;
  if (observer.on_fact == nullptr)
    return result;
  const observation::FramePosition frame{
      observation::Availability::known, input.session_frame, {}};
  result.batch = identities.next_fact(Producer::detector_input);
  if (!result.batch) {
    result.complete = false;
    return result;
  }
  const auto detector = input.detector == DetectorPath::live
                            ? std::string_view{"live"}
                            : std::string_view{"analysis"};
  const auto origin = input.write_origin == WriteOrigin::abi_snapshot
                          ? std::string_view{"abi_snapshot"}
                          : std::string_view{"legacy_core"};
  const std::array batch_fields{
      known("detector", detector),
      known("detector_frame", static_cast<std::uint64_t>(input.detector_frame),
            Unit::emulation_frame),
      known("raw_write_origin", origin),
      known("raw_write_count", static_cast<std::uint64_t>(input.writes.size()),
            Unit::count),
      known("pcm_event_count", static_cast<std::uint64_t>(input.pcm.size()),
            Unit::count),
      known("cross_stream_order", std::string_view{"independent"})};
  observer.observe(observation::FactView{
      *result.batch, "detector_input_batch", frame, {}, {}, batch_fields});
  const std::array<Cause, 1> cause{*result.batch};
  const auto emit =
      [&](std::span<const observation::FieldView> fields) noexcept {
        const auto id = identities.next_fact(Producer::detector_input);
        if (!id) {
          result.complete = false;
          return;
        }
        observer.observe(observation::FactView{
            *id, "detector_input", frame, cause, {}, fields});
      };
  for (std::size_t index = 0; index < input.writes.size(); ++index) {
    const auto &write = input.writes[index];
    const std::array fields{
        known("input_type", std::string_view{"chip_write"}),
        known("origin", origin),
        known("source_index", static_cast<std::uint64_t>(index), Unit::count),
        known("chip", static_cast<std::uint64_t>(write.chip)),
        known("cycle", static_cast<std::uint64_t>(write.cycle)),
        known("cycle_unit", std::string_view{"cpu_m_cycle_within_frame"}),
        known("address", static_cast<std::uint64_t>(write.addr)),
        known("data", static_cast<std::uint64_t>(write.data))};
    emit(fields);
  }
  for (std::size_t index = 0; index < input.pcm.size(); ++index) {
    const auto &event = input.pcm[index];
    const std::array fields{
        known("input_type", std::string_view{"pcm_event"}),
        known("origin", std::string_view{"core_event_queue"}),
        known("source_index", static_cast<std::uint64_t>(index), Unit::count),
        known("event_kind", static_cast<std::uint64_t>(event.kind)),
        known("event_type", input_detail::event_type(event.kind)),
        known("channel", static_cast<std::uint64_t>(event.channel)),
        known("envelope", static_cast<std::uint64_t>(event.env)),
        known("pan", static_cast<std::uint64_t>(event.pan)),
        known("start", static_cast<std::uint64_t>(event.st)),
        known("loop", static_cast<std::uint64_t>(event.ls)),
        known("rate", static_cast<std::uint64_t>(event.fd))};
    emit(fields);
  }
  return result;
}

} // namespace ayther::audio_qa
