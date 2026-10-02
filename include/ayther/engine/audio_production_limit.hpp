#pragma once

#include <cstdint>

namespace ayther::engine::audio_observation {

/// Immutable boundary established after the final requested game frame.
/// `main_sample_limit` names the first sample frame that may not be produced.
struct AudioProductionLimit {
  bool frozen = false;
  bool complete = true;
  std::uint64_t last_emulation_frame = 0;
  std::uint64_t main_sample_limit = 0;
  std::uint64_t pending_main_frames = 0;
  std::uint64_t delivered_output_samples_at_freeze = 0;
  std::uint64_t auxiliary_input_limit = 0;

  friend constexpr bool operator==(AudioProductionLimit,
                                   AudioProductionLimit) = default;
};

/// Result of closing every still-active HD voice at the end of a QA replay.
/// The operation is accepted only after production has been frozen. When the
/// sample boundary is incomplete, the closing facts use `frame_position`
/// instead of claiming an exact output position.
struct AudioVoiceFinalizationResult {
  bool accepted = false;
  bool output_position_known = false;
  std::uint64_t finalized_voices = 0;
  std::uint64_t output_position = 0;
  std::uint64_t frame_position = 0;

  friend constexpr bool operator==(AudioVoiceFinalizationResult,
                                   AudioVoiceFinalizationResult) = default;
};

/// Delivery of main staging that was already inside a frozen production
/// boundary. `complete == false` leaves `remaining_main_frames` available for
/// diagnosis or a later retry; the operation never extends the boundary.
struct AudioFrozenDrainResult {
  bool accepted = false;
  bool complete = false;
  std::uint64_t drained_main_frames = 0;
  std::uint64_t remaining_main_frames = 0;
  std::uint64_t main_sample_limit = 0;
  bool output_complete = false;
  std::uint64_t output_sample_limit = 0;

  friend constexpr bool operator==(AudioFrozenDrainResult,
                                   AudioFrozenDrainResult) = default;
};

} // namespace ayther::engine::audio_observation
