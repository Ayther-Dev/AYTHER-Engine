// Spec 002, BR-032 (RF-5.3, RF-5.4): silent production (contracts.md C4).
// With the audio device on SDL's dummy driver, 300 frames produced silently
// leave the same exportable HD audio state as 300 audible frames, and deliver
// no PCM to the device.
#include <ayther/ayther_session.h>

#include "../../tools/common/synth_rom.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

using ayther::AudioOutputMode;
using ayther::AytherSession;
namespace obs = ayther::engine::audio_observation;

struct Result {
  bool ran = false;
  bool audio_open = false;
  std::uint64_t device_frames = 0;
  obs::AudioHdDetectorWindowsState detector;
  obs::AudioHdVoicesState voices;
  obs::AudioHdRequestsPendingState requests;
  ayther::AudioDrainResult drain;
};

bool same_windows(const obs::AudioHdDetectorWindowsState &a,
                  const obs::AudioHdDetectorWindowsState &b) {
  if (a.complete != b.complete || a.detector != b.detector ||
      a.previous_active_signatures != b.previous_active_signatures ||
      a.runtime_channel_mask != b.runtime_channel_mask ||
      a.windows.size() != b.windows.size() ||
      a.next_anchors.size() != b.next_anchors.size() ||
      a.learned_signatures.size() != b.learned_signatures.size())
    return false;
  for (std::size_t i = 0; i < a.windows.size(); ++i) {
    const auto &x = a.windows[i];
    const auto &y = b.windows[i];
    if (x.kind != y.kind || x.key != y.key || x.signature != y.signature ||
        x.start_frame != y.start_frame || x.end_frame != y.end_frame ||
        x.last_seen_frame != y.last_seen_frame ||
        x.channel_mask != y.channel_mask)
      return false;
  }
  for (std::size_t i = 0; i < a.next_anchors.size(); ++i)
    if (a.next_anchors[i].key != b.next_anchors[i].key ||
        a.next_anchors[i].frame != b.next_anchors[i].frame)
      return false;
  for (std::size_t i = 0; i < a.learned_signatures.size(); ++i)
    if (a.learned_signatures[i].signature !=
            b.learned_signatures[i].signature ||
        a.learned_signatures[i].instrument !=
            b.learned_signatures[i].instrument)
      return false;
  return true;
}

Result run(const std::string &rom, AudioOutputMode mode) {
  Result r;
  AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = rom;
  config.enable_audio = true;
  config.derive_core_pack = false;
  auto created = AytherSession::create(config);
  if (!created) {
    std::fprintf(stderr, "[FAIL] %s\n", created.error.message.c_str());
    return r;
  }
  std::unique_ptr<AytherSession> s = std::move(*created);
  s->set_audio_output_mode(mode);
  s->set_audio_runtime_substitution(true);
  for (int f = 0; f < 300; ++f) {
    s->set_input(0, static_cast<std::uint16_t>(f & 3));
    (void)s->step();
  }
  r.ran = true;
  r.audio_open = s->audio_output_mode() == mode;
  r.device_frames = s->audio_device_frames();
  r.detector = s->audio_hd_detector_windows_state();
  r.voices = s->audio_hd_voices_state();
  r.requests = s->audio_hd_requests_pending_state();
  r.drain = s->pause_after_drain(std::chrono::milliseconds(15000));
  s->resume_transport();
  return r;
}
} // namespace

int main() try {
  const std::string rom = ayther::synth::canonical_rom_path();
  check(!rom.empty(),
        "the synthetic ROM is written to the temporary directory");
  const Result audible = run(rom, AudioOutputMode::audible);
  const Result silent = run(rom, AudioOutputMode::silent);
  check(audible.ran && silent.ran && audible.audio_open && silent.audio_open,
        "300 frames run audible and silent, each in its mode");
  std::printf("silent_production audible_device_frames=%llu "
              "silent_device_frames=%llu\n",
              static_cast<unsigned long long>(audible.device_frames),
              static_cast<unsigned long long>(silent.device_frames));
  check(audible.device_frames > 0,
        "control: audible production delivers PCM to the device");
  check(silent.device_frames == 0,
        "RF-5.3: silent production delivers no PCM to the device");
  check(same_windows(audible.detector, silent.detector) &&
            audible.detector.complete,
        "RF-5.4: the detector and window state equals the audible one");
  check(audible.voices == silent.voices,
        "RF-5.4: the HD voice state equals the audible one");
  // Since BR-033b the device queue does not mark the audible capture
  // incomplete; the completeness flags are aligned anyway so the comparison
  // below covers only the content. Everything else must be equal.
  obs::AudioHdRequestsPendingState audible_requests = audible.requests;
  audible_requests.complete = silent.requests.complete;
  audible_requests.pending_audio.complete =
      silent.requests.pending_audio.complete;
  check(audible_requests == silent.requests,
        "RF-5.4: the requests and pending audio equal the audible ones");
  check(silent.requests.complete && silent.requests.pending_audio.complete,
        "RF-5.4: with nothing queued on the device the silent state is "
        "complete");
  check(audible.drain.code == ayther::AudioDrainResult::Code::drained &&
            silent.drain.code == ayther::AudioDrainResult::Code::drained,
        "RF-4.1: the session drains and pauses in both modes");
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
