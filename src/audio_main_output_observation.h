#pragma once

#include "audio_observation_ids.h"

#include <ayther/audio_player.h>

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
    return emit_with_id(observer, ids, block).complete;
  }

  [[nodiscard]] static MainOutputObservationResult
  emit_with_id(observation::Observer observer, IdentitySource &ids,
               const AudioPlayer::MainOutputBlock &block) noexcept {
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
                             {}});
    return {id, true};
  }
};

} // namespace ayther::audio_qa
