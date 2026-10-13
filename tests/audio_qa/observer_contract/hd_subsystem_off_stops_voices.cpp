// Apagar el audio HD a mitad calla lo que ya suena.
//
// El perfil «Original» del menú de pausa apaga los subsistemas: el original
// vuelve en el acto (hd_can_sound), pero la voz HD que ya sonaba seguía —y la
// instantánea de la pausa la reanudaba—, así que se oían las dos músicas.
#include "../../../tools/common/synth_rom.h"

#include <ayther/ayther_session.h>
#include <ayther/engine/audio_hd_state.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace obs = ayther::engine::audio_observation;

namespace {

constexpr std::uint64_t kAssignedKey = 0xA0B0;

int g_fail = 0;

void check(bool ok, const char *what) {
  std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what);
  if (!ok)
    ++g_fail;
}

obs::AudioHdVoicesState one_music_voice() {
  obs::AudioHdVoicesState state;
  std::vector<std::int16_t> samples(64);
  for (std::size_t i = 0; i < samples.size(); ++i)
    samples[i] = static_cast<std::int16_t>(i * 17 - 400);
  state.pcm_assets.push_back({17, std::move(samples)});

  // The valid voice of hd_voice_restore, keyed by an assigned signature.
  obs::AudioHdVoiceState voice;
  voice.occurrence = 901;
  voice.cause_producer = 7;
  voice.cause_sequence = 81;
  voice.business_key = kAssignedKey;
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
  voice.category = ayther::AudioCategory::music;
  state.voices.push_back(voice);
  state.started = 4;
  state.mixed_samples = 1234;
  state.skew_samples = 9;
  state.max_skew_samples = 9;
  return state;
}

bool restore_voice(ayther::AytherSession &session) {
  obs::AudioHdStateHeader header;
  header.game_state_identity = "hd-off";
  header.emulation_frame = session.audio_initial_snapshot().emulation_frame;
  header.sections = obs::kAudioHdStateRequiredSections;
  const auto result =
      session.restore_audio_hd_voices(header, "hd-off", one_music_voice());
  if (!result.restored())
    std::printf("  restore code %d\n", static_cast<int>(result.code));
  return result.restored() &&
         session.audio_hd_voices_state().voices.size() == 1;
}

void run(const std::filesystem::path &rom_path) {
  ayther::synth::Rom rom("AYTHER QA HD OFF");
  ayther::synth::program_canonical(rom, true);
  check(rom.save(rom_path), "write synthetic ROM");

  ayther::AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = rom_path.string();
  config.enable_audio = false;
  config.derive_core_pack = false;
  auto created = ayther::AytherSession::create(config);
  check(static_cast<bool>(created), "create session");
  if (!created)
    return;
  auto &session = **created;
  (void)session.step();
  session.assign_audio_event(kAssignedKey, "assets/music.wav");

  check(restore_voice(session), "an HD music voice is playing");
  session.set_subsystem_enabled(ayther::Subsystem::Music, false);
  check(
      session.audio_hd_voices_state().voices.size() == 1,
      "with HD effects still on the gate stays open: the voice keeps playing");

  session.set_subsystems_enabled_mask(0); // the «Original» profile
  check(session.audio_hd_voices_state().voices.empty(),
        "turning HD audio off stops the HD voice already playing");

  session.set_subsystems_enabled_mask(0xFFFFFFFFu);
  check(restore_voice(session), "HD audio back on: a voice can play again");
  session.set_subsystem_enabled(ayther::Subsystem::Music, false);
  session.set_subsystem_enabled(ayther::Subsystem::Sfx, false);
  check(session.audio_hd_voices_state().voices.empty(),
        "switching the last HD audio subsystem off also stops it");
}

} // namespace

int main() { // NOLINT(bugprone-exception-escape) -- Test filesystem failure is
             // fatal.
  const auto rom = std::filesystem::current_path() / "hd-subsystem-off.md";
  run(rom);
  std::error_code ignored;
  std::filesystem::remove(rom, ignored);
  return g_fail == 0 ? 0 : 1;
}
