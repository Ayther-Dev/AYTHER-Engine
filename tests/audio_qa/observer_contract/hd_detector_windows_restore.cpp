#include "../../../tools/common/synth_rom.h"

#include <ayther/ayther_session.h>
#include <ayther/engine/audio_hd_state.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace obs = ayther::engine::audio_observation;

namespace {

using Detector = std::unique_ptr<AytherAudioEventDetector,
                                 decltype(&ayther_audio_event_free)>;

std::vector<std::uint8_t> detector_state(std::uint32_t frame) {
  Detector detector{ayther_audio_event_new(), &ayther_audio_event_free};
  if (!detector)
    return {};
  const std::array<AytherAudioWrite, 3> writes{{
      {0, 0xA4, 0x24, 0},
      {1, 0xA0, 0x3B, 0},
      {2, 0x28, 0xF0, 0},
  }};
  ayther_audio_event_process_frame(detector.get(), frame, writes.data(),
                                   static_cast<std::uint32_t>(writes.size()));
  const auto size = ayther_audio_event_state_size(detector.get());
  std::vector<std::uint8_t> result(size);
  if (size == 0 || ayther_audio_event_state_write(detector.get(), result.data(),
                                                  result.size()) != size)
    result.clear();
  return result;
}

bool same_state(const obs::AudioHdDetectorWindowsState &left,
                const obs::AudioHdDetectorWindowsState &right) {
  if (left.detector != right.detector ||
      left.previous_active_signatures != right.previous_active_signatures ||
      left.runtime_channel_mask != right.runtime_channel_mask ||
      left.complete != right.complete ||
      left.windows.size() != right.windows.size() ||
      left.next_anchors.size() != right.next_anchors.size() ||
      left.learned_signatures.size() != right.learned_signatures.size())
    return false;
  for (std::size_t i = 0; i < left.windows.size(); ++i) {
    const auto &a = left.windows[i];
    const auto &b = right.windows[i];
    if (a.kind != b.kind || a.key != b.key || a.signature != b.signature ||
        a.start_frame != b.start_frame || a.end_frame != b.end_frame ||
        a.last_seen_frame != b.last_seen_frame ||
        a.channel_mask != b.channel_mask)
      return false;
  }
  for (std::size_t i = 0; i < left.next_anchors.size(); ++i) {
    if (left.next_anchors[i].key != right.next_anchors[i].key ||
        left.next_anchors[i].frame != right.next_anchors[i].frame)
      return false;
  }
  for (std::size_t i = 0; i < left.learned_signatures.size(); ++i) {
    const auto &a = left.learned_signatures[i];
    const auto &b = right.learned_signatures[i];
    if (a.signature != b.signature || a.instrument != b.instrument ||
        a.pitch != b.pitch || a.assigned_instrument != b.assigned_instrument)
      return false;
  }
  return true;
}

bool restores_without_stepping(const std::filesystem::path &rom_path) {
  ayther::synth::Rom rom("AYTHER QA RESTORE 128");
  ayther::synth::program_canonical(rom, true);
  if (!rom.save(rom_path))
    return false;

  ayther::AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = rom_path.string();
  config.enable_audio = false;
  config.derive_core_pack = false;
  auto created = ayther::AytherSession::create(config);
  if (!created)
    return false;
  auto &session = **created;
  session.set_audio_runtime_substitution(true);
  (void)session.step();
  const auto frame = session.audio_initial_snapshot().emulation_frame;

  obs::AudioHdStateHeader header;
  header.game_state_identity = "synthetic-save-state-1";
  header.emulation_frame = frame;
  header.sections = obs::kAudioHdStateRequiredSections;

  obs::AudioHdDetectorWindowsState expected;
  expected.detector = detector_state(static_cast<std::uint32_t>(frame));
  expected.previous_active_signatures = {0x1100, 0x2200};
  expected.windows = {
      {obs::AudioHdWindowKind::sequence, 0x31, 0x31, 0, frame + 8, frame + 8,
       0x03},
      {obs::AudioHdWindowKind::live_sequence, 0x42, 0, 0, frame + 12, frame,
       0x40},
  };
  expected.next_anchors = {{0x42, frame + 13}};
  expected.learned_signatures = {{0x1100, 0xAABB, 69, true},
                                 {0x2200, 0xCCDD, 0xFF, false}};
  expected.runtime_channel_mask = 0x43;
  if (expected.detector.empty())
    return false;

  auto inherited = expected;
  inherited.previous_active_signatures = {0xDEAD};
  inherited.windows.clear();
  inherited.next_anchors.clear();
  inherited.learned_signatures = {{0xDEAD, 0xEEEE, 60, true}};
  inherited.runtime_channel_mask = 1;
  if (!session
           .restore_audio_hd_detector_windows(header, "synthetic-save-state-1",
                                              inherited)
           .restored())
    return false;

  const auto restored = session.restore_audio_hd_detector_windows(
      header, "synthetic-save-state-1", expected);
  if (!restored.restored() ||
      session.audio_initial_snapshot().emulation_frame != frame ||
      session.audio_initial_snapshot().initialization !=
          obs::AudioInitializationMode::restored)
    return false;
  const auto actual = session.audio_hd_detector_windows_state();
  if (!same_state(actual, expected))
    return false;

  auto invalid = expected;
  invalid.detector.front() ^= 0xFF;
  if (session
          .restore_audio_hd_detector_windows(header, "synthetic-save-state-1",
                                             invalid)
          .code != obs::AudioHdRestoreCode::invalid_detector)
    return false;
  return same_state(session.audio_hd_detector_windows_state(), actual) &&
         session.audio_initial_snapshot().emulation_frame == frame;
}

} // namespace

int main() { // NOLINT(bugprone-exception-escape) -- Test filesystem failure is
             // fatal.
  const auto rom = std::filesystem::current_path() / "restore-session.md";
  const bool valid = restores_without_stepping(rom);
  std::error_code ignored;
  std::filesystem::remove(rom, ignored);
  return valid ? 0 : 1;
}
