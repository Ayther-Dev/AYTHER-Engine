// A `continue_playback` retrigger is a new pass of the same voice, so the
// voice must live until the NEW window ends. Golden Axe «The Battle»,
// 2026-10-14: the loop has span 576 and window 577; every other pass the
// continuation kept the old end and the voice died one frame later as a
// window cut — silence and the original until the next pass, which then
// started from zero.
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

bool start(HdMixer &mixer, HdMixPcm asset, uint64_t end_frame,
           uint64_t cut_frame, const HdMixer::VoiceIdentity &identity,
           HdMixer::StartResult &result) {
  return mixer.start(
      101, asset, 0, 0, 1.0F, true, true, end_frame, cut_frame, 0, 0, 0,
      &identity, &result, ayther::RepeatPolicy::continue_playback,
      ayther::TransitionPolicy::cut, ayther::AudioCategory::music);
}

} // namespace

int main() {
  std::printf("=== continue_extends_window — the continued voice lives "
              "until the new window ends ===\n");
  HdMixer mixer;
  const auto asset = pcm();
  HdMixer::StartResult result{};
  const HdMixer::VoiceIdentity first{1, 5, 8};
  check(start(mixer, asset, 100, 100, first, result) &&
            result.action == HdMixer::StartAction::start &&
            mixer.voice_count() == 1,
        "first pass opens the voice with window [0, 100]");

  mixer.tick_frame(90);
  check(mixer.voice_count() == 1, "alive inside its window");

  // The next pass of the loop: same key, same asset, window [90, 190].
  const HdMixer::VoiceIdentity second{2, 5, 9};
  check(start(mixer, asset, 190, 190, second, result) &&
            result.action == HdMixer::StartAction::continue_playback &&
            result.previous_occurrence == 1 && mixer.voice_count() == 1,
        "retrigger inside the window continues the same voice");

  // Past the OLD window end: the voice carries the new one.
  mixer.tick_frame(101);
  check(mixer.voice_count() == 1,
        "the old window end does not cut the continued voice");

  mixer.tick_frame(150);
  check(mixer.voice_count() == 1, "still alive in the middle of the new pass");

  // Past the NEW window end: the voice ends there, and only there.
  mixer.tick_frame(191);
  check(mixer.voice_count() == 0, "the new window end ends the voice");

  std::printf("%s\n", g_fail ? "FAIL" : "OK");
  return g_fail ? 1 : 0;
}
