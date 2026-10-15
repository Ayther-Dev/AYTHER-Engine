// A `continue_playback` retrigger continues a voice that is playing, but a
// voice already fading out (stopped by a window cut or a restart anchor)
// cannot carry the continuation: the retrigger must start a new voice while
// the old one drains. Golden Axe, 2026-10-14: the restart anchor cut
// Wilderness and the same-frame rising edge "continued" the dying voice —
// silence until the next anchor, then the track from zero.
#include <ayther/audio_hd_mixer.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

int g_fail = 0;

void check(bool ok, const char *what) {
  std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what);
  if (!ok)
    ++g_fail;
}

std::shared_ptr<const std::vector<std::int16_t>> pcm() {
  auto result = std::make_shared<std::vector<std::int16_t>>(64);
  for (std::size_t i = 0; i < result->size(); ++i)
    (*result)[i] = static_cast<std::int16_t>(1000 + 10 * i);
  return result;
}

bool start(HdMixer &mixer, HdMixPcm asset,
           const HdMixer::VoiceIdentity &identity,
           HdMixer::StartResult &result) {
  return mixer.start(
      101, asset, 0, 0, 1.0F, true, true, UINT64_MAX, UINT64_MAX, 0, 0, 0,
      &identity, &result, ayther::RepeatPolicy::continue_playback,
      ayther::TransitionPolicy::cut, ayther::AudioCategory::music);
}

} // namespace

int main() {
  std::printf(
      "=== restart_after_stop — a draining voice does not continue ===\n");
  HdMixer mixer;
  const auto asset = pcm();
  HdMixer::StartResult result{};
  const HdMixer::VoiceIdentity first{1, 5, 8};
  check(start(mixer, asset, first, result) &&
            result.action == HdMixer::StartAction::start &&
            mixer.voice_count() == 1,
        "first rising edge starts the voice");

  // Playing: the retrigger continues it, no new voice.
  const HdMixer::VoiceIdentity second{2, 5, 9};
  check(start(mixer, asset, second, result) &&
            result.action == HdMixer::StartAction::continue_playback &&
            result.previous_occurrence == 1 && result.new_occurrence == 1 &&
            mixer.voice_count() == 1,
        "retrigger while playing continues the same voice");

  // Stopped on purpose (restart anchor, window cut): it fades out.
  check(mixer.stop(101), "stop by key starts the fade of the voice");

  // The retrigger is a fresh start while the old voice drains.
  const HdMixer::VoiceIdentity third{3, 5, 10};
  check(start(mixer, asset, third, result) &&
            result.action == HdMixer::StartAction::restart &&
            result.previous_occurrence == 1 && result.new_occurrence == 3 &&
            mixer.voice_count() == 2,
        "retrigger of a draining voice starts a new one");

  std::printf("%s\n", g_fail ? "FAIL" : "OK");
  return g_fail ? 1 : 0;
}
