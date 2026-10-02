#include "../../../tools/common/synth_rom.h"

#include <ayther/audio_player.h>
#include <ayther/ayther_session.h>
#include <ayther/engine/audio_initial_snapshot.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace obs = ayther::engine::audio_observation;

namespace {

struct SdlLifetime {
  SdlLifetime() {
    if (!SDL_Init(SDL_INIT_AUDIO))
      throw 1;
  }
  ~SdlLifetime() { SDL_Quit(); }
};

bool write_wav(const std::filesystem::path &path) {
  constexpr std::size_t frames = 8192;
  constexpr std::uint32_t data_size =
      static_cast<std::uint32_t>(frames * 2U * sizeof(std::int16_t));
  constexpr std::uint32_t riff_size = 36 + data_size;
  const std::array<std::uint8_t, 44> header{
      'R',
      'I',
      'F',
      'F',
      static_cast<std::uint8_t>(riff_size),
      static_cast<std::uint8_t>(riff_size >> 8),
      static_cast<std::uint8_t>(riff_size >> 16),
      static_cast<std::uint8_t>(riff_size >> 24),
      'W',
      'A',
      'V',
      'E',
      'f',
      'm',
      't',
      ' ',
      16,
      0,
      0,
      0,
      1,
      0,
      2,
      0,
      0x44,
      0xAC,
      0,
      0,
      0x10,
      0xB1,
      2,
      0,
      4,
      0,
      16,
      0,
      'd',
      'a',
      't',
      'a',
      static_cast<std::uint8_t>(data_size),
      static_cast<std::uint8_t>(data_size >> 8),
      static_cast<std::uint8_t>(data_size >> 16),
      static_cast<std::uint8_t>(data_size >> 24)};
  const std::vector<std::int16_t> pcm(frames * 2, 1000);
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char *>(header.data()), header.size());
  file.write(reinterpret_cast<const char *>(pcm.data()),
             static_cast<std::streamsize>(pcm.size() * sizeof(pcm[0])));
  return file.good();
}

bool player_discards_inherited_audio(const std::filesystem::path &asset) {
  AudioPlayer player;
  if (!player.init() || !write_wav(asset) ||
      !player.play_oneshot_asset_file(asset.string(), 77))
    return false;

  const std::array<std::int16_t, 8> native{1, 2, 3, 4, 5, 6, 7, 8};
  const std::array<float, 16> synth{};
  const std::array<std::uint64_t, 1> muted{123};
  player.buffer_emulator(123, native.data(), 4);
  player.feed_synth(synth.data(), synth.size() / 2);
  player.set_user_mute_hashes(muted.data(), muted.size());
  player.set_game_gain(0.25F);
  player.set_muted(true);

  obs::AudioInitialSnapshot inherited;
  player.append_initial_snapshot(inherited);
  if (inherited.voices.empty() || inherited.pending_audio.empty())
    return false;

  if (!player.prepare_fresh_session())
    return false;
  obs::AudioInitialSnapshot fresh;
  player.append_initial_snapshot(fresh);
  if (!fresh.complete || !fresh.voices.empty() ||
      !fresh.pending_audio.empty() || player.pending_frames() != 0 ||
      player.hd_voice_count() != 0 || player.hd_voices_started() != 0 ||
      player.timeline_samples() != 0 || player.game_gain() != 1.0F ||
      player.is_muted())
    return false;

  return player.prepare_fresh_session() && player.pending_frames() == 0 &&
         player.hd_voice_count() == 0;
}

bool session_resets_detector_without_stepping(
    const std::filesystem::path &rom_path) {
  ayther::synth::Rom rom("AYTHER QA FRESH 128");
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

  std::array<AytherAudioActive, 64> active{};
  if (session.audio_live_active(active.data(), active.size()) == 0)
    return false;
  const auto frame_before = session.audio_initial_snapshot().emulation_frame;

  if (!session.prepare_fresh_hd_audio())
    return false;
  const auto fresh = session.audio_initial_snapshot();
  if (fresh.initialization != obs::AudioInitializationMode::fresh ||
      fresh.emulation_frame != frame_before || !fresh.complete ||
      !fresh.voices.empty() || !fresh.windows.empty() ||
      !fresh.pending_audio.empty() ||
      session.audio_live_active(active.data(), active.size()) != 0)
    return false;

  return session.prepare_fresh_hd_audio() &&
         session.audio_initial_snapshot().emulation_frame == frame_before;
}

} // namespace

int main() try {
  const SdlLifetime sdl;
  const auto base = std::filesystem::current_path();
  const auto asset = base / "fresh-audio.wav";
  const auto rom = base / "fresh-session.md";
  const bool valid = player_discards_inherited_audio(asset) &&
                     session_resets_detector_without_stepping(rom);
  std::error_code ignored;
  std::filesystem::remove(asset, ignored);
  std::filesystem::remove(rom, ignored);
  return valid ? 0 : 1;
} catch (...) {
  return 2;
}
