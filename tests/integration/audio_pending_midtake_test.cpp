// Spec 002, BR-033b (RF-5.3, RF-5.7, RF-3.6; contracts.md C4): the pending
// HD audio exported in the middle of a take is complete and restores. In
// silent production with runtime substitution, the state exported at frame
// k of a take must have complete = true; restoring it (with the core state,
// detector windows and voices of k) into a fresh session and producing N
// frames must give the same HD audio state as producing them linearly.
//
// The test core keeps its state in the DLL: the linear run and the restored
// run never step in turns — the first is captured whole, then destroyed.
#include <ayther/ayther_session.h>

#include "../../tools/common/synth_rom.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

namespace obs = ayther::engine::audio_observation;
using ayther::AudioOutputMode;
using ayther::AytherSession;

constexpr int kExportFrame = 180;
constexpr int kAfter = 60;

std::unique_ptr<AytherSession> open(const std::string &rom,
                                    AudioOutputMode mode) {
  AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = rom;
  config.enable_audio = true;
  config.derive_core_pack = false;
  auto created = AytherSession::create(config);
  if (!created) {
    std::fprintf(stderr, "[FAIL] %s\n", created.error.message.c_str());
    return nullptr;
  }
  std::unique_ptr<AytherSession> s = std::move(*created);
  s->set_audio_output_mode(mode);
  s->set_audio_runtime_substitution(true);
  return s;
}

void step(AytherSession &s, int frame) {
  s.set_input(0, static_cast<std::uint16_t>(frame & 3));
  (void)s.step();
}

struct Capture {
  std::vector<std::uint8_t> core;
  std::string identity;
  std::uint64_t frame = 0;
  obs::AudioHdDetectorWindowsState detector;
  obs::AudioHdVoicesState voices;
  obs::AudioHdRequestsPendingState requests;
  ayther::engine::visual_state::VisualState visual;
};

Capture capture(const AytherSession &s) {
  Capture c;
  (void)s.serialize(c.core);
  c.identity = s.game_state_identity();
  c.frame = s.audio_initial_snapshot().emulation_frame;
  c.detector = s.audio_hd_detector_windows_state();
  c.voices = s.audio_hd_voices_state();
  c.requests = s.audio_hd_requests_pending_state();
  c.visual = s.export_visual_state();
  return c;
}
} // namespace

// One take in `mode`: export at k, restore into a fresh session, produce.
// `exact` compares the whole HD audio state after N frames; in audible mode
// the device consumes on its own clock, so the device counters differ and
// only the production state is compared.
void run(const std::string &rom, AudioOutputMode mode, bool exact) {

  // 1. Linear: frames 0..k-1, capture at k, then k..k+N-1, capture again.
  Capture at_k;
  Capture linear_end;
  std::uint32_t incomplete_frames = 0;
  {
    auto s = open(rom, mode);
    if (!s)
      return;
    for (int f = 0; f < kExportFrame; ++f) {
      step(*s, f);
      if (f >= kExportFrame - 60 &&
          !s->audio_hd_requests_pending_state().pending_audio.complete)
        ++incomplete_frames;
    }
    at_k = capture(*s);
    for (int f = kExportFrame; f < kExportFrame + kAfter; ++f)
      step(*s, f);
    linear_end = capture(*s);
  }
  std::printf("  frames with incomplete pending audio in the last 60: %u\n",
              incomplete_frames);
  check(at_k.requests.complete && at_k.requests.pending_audio.complete,
        "RF-5.7: the pending audio exported mid-take is complete");
  check(incomplete_frames == 0,
        "RF-5.7: no frame of the take exports an incomplete pending state");

  // 2. Restored: a fresh session at k, then the same N frames.
  {
    auto s = open(rom, mode);
    if (!s)
      return;
    check(static_cast<bool>(s->unserialize(at_k.core)),
          "the core state of k is restored");
    // The C4 order: core, visual state (it puts the session at frame k),
    // then the HD audio state, validated against that frame.
    const auto visual =
        s->restore_visual_state(at_k.visual, at_k.identity, at_k.frame);
    check(visual.restored(), "the visual state of k is restored");
    obs::AudioHdStateHeader header;
    header.game_state_identity = at_k.identity;
    header.emulation_frame = at_k.frame;
    header.sections = obs::kAudioHdStateRequiredSections;
    const auto det = s->restore_audio_hd_detector_windows(header, at_k.identity,
                                                          at_k.detector);
    const auto voi =
        s->restore_audio_hd_voices(header, at_k.identity, at_k.voices);
    const auto req = s->restore_audio_hd_requests_pending(header, at_k.identity,
                                                          at_k.requests);
    std::printf("  restore codes: detector=%d voices=%d requests=%d\n",
                static_cast<int>(det.code), static_cast<int>(voi.code),
                static_cast<int>(req.code));
    check(req.code != obs::AudioHdRestoreCode::incomplete_payload,
          "RF-5.7: restoring it is not rejected as incomplete_payload");
    check(det.restored() && voi.restored() && req.restored(),
          "RF-3.6: the HD audio state of k restores whole");
    for (int f = kExportFrame; f < kExportFrame + kAfter; ++f)
      step(*s, f);
    const Capture restored_end = capture(*s);
    check(restored_end.identity == linear_end.identity,
          "the restored run reaches the same game state");
    obs::AudioHdRequestsPendingState a = restored_end.requests;
    obs::AudioHdRequestsPendingState b = linear_end.requests;
    if (!exact) {
      for (obs::AudioHdRequestsPendingState *r : {&a, &b}) {
        r->pending_audio.main_output_samples = 0;
        r->pending_audio.auxiliary_consumed_input = 0;
        r->pending_audio.auxiliary_discard_before = 0;
      }
    }
    check(restored_end.voices == linear_end.voices && a == b,
          "RF-5.3: after N frames the HD audio state equals the linear one");
  }
}

int main() try {
  const std::string rom = ayther::synth::canonical_rom_path();
  check(!rom.empty(), "the synthetic ROM is written");
  std::printf("-- silent production\n");
  run(rom, AudioOutputMode::silent, true);
  // The Runtime takes its checkpoints while the take plays (audible): the
  // device then still holds queued stream bytes at the export.
  std::printf("-- audible production\n");
  run(rom, AudioOutputMode::audible, false);

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
