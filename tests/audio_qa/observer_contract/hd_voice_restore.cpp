#include "../../../tools/common/synth_rom.h"

#include <ayther/ayther_session.h>
#include <ayther/engine/audio_hd_state.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace obs = ayther::engine::audio_observation;

namespace {

obs::AudioHdVoicesState one_voice_state() {
  obs::AudioHdVoicesState state;
  std::vector<std::int16_t> samples(64);
  for (std::size_t i = 0; i < samples.size(); ++i)
    samples[i] = static_cast<std::int16_t>(i * 17 - 400);
  state.pcm_assets.push_back({17, std::move(samples)});

  obs::AudioHdVoiceState voice;
  voice.occurrence = 901;
  voice.cause_producer = 7;
  voice.cause_sequence = 81;
  voice.business_key = 0xA0B0;
  voice.pcm_identity = 17;
  voice.source_position = 7;
  voice.output_start = 1000;
  voice.output_end = 1012;
  voice.output_end_known = true;
  voice.gain = 0.35F;
  voice.looping = true;
  voice.event = true;
  voice.end_frame = 50;
  voice.cut_frame = 55;
  voice.fade_span = 2646;
  voice.loop_begin = 4;
  voice.loop_end = 12;
  voice.late_samples = 9;
  state.voices.push_back(voice);
  state.started = 4;
  state.mixed_samples = 1234;
  state.skew_samples = 9;
  state.max_skew_samples = 9;
  return state;
}

bool restores_one_voice_without_output(const std::filesystem::path &rom_path) {
  ayther::synth::Rom rom("AYTHER QA VOICE RESTORE");
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
  (void)session.step();
  const auto frame = session.audio_initial_snapshot().emulation_frame;

  obs::AudioHdStateHeader header;
  header.game_state_identity = "voice-state-1";
  header.emulation_frame = frame;
  header.sections = obs::kAudioHdStateRequiredSections;
  const auto expected = one_voice_state();

  const auto result =
      session.restore_audio_hd_voices(header, "voice-state-1", expected);
  if (!result.restored() ||
      session.audio_initial_snapshot().emulation_frame != frame)
    return false;

  const auto snapshot = session.audio_initial_snapshot();
  if (snapshot.initialization != obs::AudioInitializationMode::restored ||
      snapshot.voices.size() != 1 || !snapshot.pending_audio.empty())
    return false;
  const auto &voice = snapshot.voices.front();
  if (voice.occurrence != 901 || voice.business_key != 0xA0B0 ||
      voice.source_position != 7 || voice.source_limit != 32 ||
      voice.output_start != 1000 || std::fabs(voice.gain - 0.35F) > 0.0001F ||
      !voice.looping || !voice.event || voice.end_frame != 50 ||
      voice.cut_frame != 55 || voice.loop_begin != 4 || voice.loop_end != 12 ||
      voice.fade_remaining != 0)
    return false;

  const auto captured = session.audio_hd_voices_state();
  if (captured != expected)
    return false;
  const auto captured_again = session.audio_hd_voices_state();
  if (captured_again != captured ||
      session.audio_initial_snapshot().emulation_frame != frame ||
      !session.audio_initial_snapshot().pending_audio.empty())
    return false;

  auto invalid = expected;
  invalid.voices.front().source_position = 32;
  if (session.restore_audio_hd_voices(header, "voice-state-1", invalid).code !=
      obs::AudioHdRestoreCode::invalid_voice)
    return false;
  return session.audio_hd_voices_state() == captured &&
         session.audio_initial_snapshot().emulation_frame == frame;
}

} // namespace

int main() { // NOLINT(bugprone-exception-escape) -- Test filesystem failure is
             // fatal.
  const auto rom = std::filesystem::current_path() / "voice-restore.md";
  const bool valid = restores_one_voice_without_output(rom);
  std::error_code ignored;
  std::filesystem::remove(rom, ignored);
  return valid ? 0 : 1;
}
