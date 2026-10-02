#include <ayther/audio_hd_mixer.h>
#include <ayther/audio_player.h>
#include <ayther/engine/audio_initial_snapshot.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace obs = ayther::engine::audio_observation;

namespace {

struct SdlLifetime {
  SdlLifetime() {
    if (!SDL_Init(SDL_INIT_AUDIO)) {
      throw 1;
    }
  }
  ~SdlLifetime() { SDL_Quit(); }
};

bool voice_snapshot_is_owned_and_read_only() {
  HdMixer mixer;
  auto pcm = std::make_shared<const std::vector<int16_t>>(
      std::vector<int16_t>{1, 2, 3, 4, 5, 6, 7, 8});
  const HdMixer::VoiceIdentity identity{9, 2, 7};
  if (!mixer.start(77, pcm, 100, 2, 0.5F, true, true, 40, 50, 6, 1, 4,
                   &identity)) {
    return false;
  }

  obs::AudioInitialSnapshot first;
  mixer.append_initial_snapshot(first);
  obs::AudioInitialSnapshot second;
  mixer.append_initial_snapshot(second);
  if (mixer.voice_count() != 1 || first.voices.size() != 1 ||
      second.voices.size() != 1) {
    return false;
  }
  const auto &voice = first.voices.front();
  return voice.occurrence == 9 && voice.business_key == 77 &&
         voice.source_position == 2 && voice.source_limit == 4 &&
         voice.output_start == 100 && voice.gain == 0.5F && voice.looping &&
         voice.event && voice.end_frame == 40 && voice.cut_frame == 50 &&
         voice.loop_begin == 1 && voice.loop_end == 4 &&
         second.voices.front().source_position == voice.source_position;
}

bool pending_snapshot_does_not_consume_staging() {
  AudioPlayer player;
  if (!player.init()) {
    return false;
  }
  const std::array<int16_t, 8> pcm{1, 2, 3, 4, 5, 6, 7, 8};
  player.buffer_emulator(123, pcm.data(), 4);

  obs::AudioInitialSnapshot first;
  player.append_initial_snapshot(first);
  obs::AudioInitialSnapshot second;
  player.append_initial_snapshot(second);
  if (player.pending_frames() != 4 || first.pending_audio.size() != 2 ||
      second.pending_audio.size() != 2) {
    return false;
  }
  const auto &staging = first.pending_audio[0];
  const auto &batch = first.pending_audio[1];
  return first.complete && second.complete &&
         staging.kind == obs::AudioInitialPendingKind::main_staging &&
         staging.frames == 4 &&
         batch.kind == obs::AudioInitialPendingKind::main_batch &&
         batch.identity == 123 && batch.position == 0 && batch.frames == 4 &&
         second.pending_audio[1].frames == batch.frames;
}

bool window_values_are_owned() {
  obs::AudioInitialSnapshot snapshot;
  snapshot.windows.push_back(
      {obs::AudioInitialWindowKind::sequence, 8, 9, 10, 20, 3});
  const auto copy = snapshot;
  snapshot.windows.clear();
  return copy.windows.size() == 1 && copy.windows[0].key == 8 &&
         copy.windows[0].signature == 9 && copy.windows[0].start_frame == 10 &&
         copy.windows[0].end_frame == 20 && copy.windows[0].channel_mask == 3;
}

} // namespace

int main() try {
  const SdlLifetime sdl;
  return voice_snapshot_is_owned_and_read_only() &&
                 pending_snapshot_does_not_consume_staging() &&
                 window_values_are_owned()
             ? 0
             : 1;
} catch (...) {
  return 2;
}
