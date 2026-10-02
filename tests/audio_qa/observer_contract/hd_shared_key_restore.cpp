#include "../../../tools/common/synth_rom.h"

#include <ayther/ayther_session.h>
#include <ayther/engine/audio_hd_state.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace obs = ayther::engine::audio_observation;

namespace {

struct VoiceSeed {
  std::uint64_t occurrence = 0;
  std::uint64_t source_position = 0;
  std::uint64_t output_start = 0;
};

obs::AudioHdVoiceState voice(const VoiceSeed seed) {
  obs::AudioHdVoiceState value;
  value.occurrence = seed.occurrence;
  value.cause_producer = 8;
  value.cause_sequence = seed.occurrence + 100;
  value.business_key = 0x5151;
  value.pcm_identity = 23;
  value.source_position = seed.source_position;
  value.output_start = seed.output_start;
  value.gain = seed.occurrence == 1001 ? 0.4F : 0.8F;
  value.end_frame = 90;
  value.cut_frame = 95;
  value.fade_span = 2646;
  return value;
}

bool restores_distinct_occurrences(const std::filesystem::path &rom_path) {
  ayther::synth::Rom rom("AYTHER QA SHARED KEY");
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
  header.game_state_identity = "shared-key-state";
  header.emulation_frame = frame;
  header.sections = obs::kAudioHdStateRequiredSections;

  obs::AudioHdVoicesState expected;
  std::vector<std::int16_t> samples(96);
  for (std::size_t i = 0; i < samples.size(); ++i)
    samples[i] = static_cast<std::int16_t>(700 - i * 9);
  expected.pcm_assets.push_back({23, std::move(samples)});
  expected.voices = {voice({1001, 3, 2000}), voice({1002, 19, 2016})};
  expected.started = 2;

  if (!session.restore_audio_hd_voices(header, "shared-key-state", expected)
           .restored())
    return false;

  const auto snapshot = session.audio_initial_snapshot();
  if (snapshot.emulation_frame != frame || snapshot.voices.size() != 2 ||
      !snapshot.pending_audio.empty())
    return false;
  const auto &first = snapshot.voices[0];
  const auto &second = snapshot.voices[1];
  if (first.business_key != second.business_key || first.occurrence != 1001 ||
      second.occurrence != 1002 || first.source_position != 3 ||
      second.source_position != 19 || first.output_start != 2000 ||
      second.output_start != 2016 || first.source_limit != 48 ||
      second.source_limit != 48)
    return false;

  const auto captured = session.audio_hd_voices_state();
  if (captured != expected || captured.pcm_assets.size() != 1 ||
      captured.voices[0].pcm_identity != captured.voices[1].pcm_identity)
    return false;

  auto invalid = expected;
  invalid.voices[1].occurrence = invalid.voices[0].occurrence;
  if (session.restore_audio_hd_voices(header, "shared-key-state", invalid)
          .code != obs::AudioHdRestoreCode::invalid_voice)
    return false;
  return session.audio_hd_voices_state() == captured &&
         session.audio_initial_snapshot().emulation_frame == frame;
}

} // namespace

int main() { // NOLINT(bugprone-exception-escape) -- Test filesystem failure is
             // fatal.
  const auto rom = std::filesystem::current_path() / "shared-key-restore.md";
  const bool valid = restores_distinct_occurrences(rom);
  std::error_code ignored;
  std::filesystem::remove(rom, ignored);
  return valid ? 0 : 1;
}
