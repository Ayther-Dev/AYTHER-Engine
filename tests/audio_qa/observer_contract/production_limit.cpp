#include "../../../tools/common/synth_rom.h"

#include <ayther/audio_player.h>
#include <ayther/ayther_session.h>
#include <ayther/engine/audio_hd_state.hpp>

#include <array>
#include <cstdint>
#include <filesystem>

namespace obs = ayther::engine::audio_observation;

namespace {

obs::AudioHdPendingAudioState pending_audio() {
  obs::AudioHdPendingAudioState state;
  state.main_pcm = {10, 11, 12, 13, 14, 15, 16, 17};
  state.main_batches.push_back({0xA100, 0, 4});
  state.original_batches.push_back({0xA100, 0, 4, 44, 1200, true, true});
  state.timeline_samples = 1200;
  state.frame_mark = 2;
  state.current_emulation_frame = 44;
  state.current_frame_boundary = 1200;
  state.current_frame_known = true;
  state.main_output_samples = 1170;
  state.auxiliary_input_samples = 80;
  state.auxiliary_consumed_input = 80;
  state.auxiliary_discard_before = 80;
  return state;
}

bool freezes_player_without_new_samples() {
  AudioPlayer player;
  if (!player.init(ayther::RuntimeOptions{}))
    return false;
  const auto pending = pending_audio();
  if (player.restore_hd_pending_audio(pending) !=
      obs::AudioHdRestoreCode::restored)
    return false;

  const auto limit = player.freeze_production(44);
  if (!limit.frozen || !limit.complete || limit.last_emulation_frame != 44 ||
      limit.main_sample_limit != 1204 || limit.pending_main_frames != 4 ||
      limit.delivered_output_samples_at_freeze != 1170 ||
      limit.auxiliary_input_limit != 80)
    return false;

  constexpr std::array<std::int16_t, 4> native{90, 91, 92, 93};
  constexpr std::array<float, 4> auxiliary{0.2F, -0.2F, 0.3F, -0.3F};
  player.buffer_emulator(0xBAD, native.data(), 2);
  player.buffer_router(auxiliary.data(), 2, false);
  player.feed_synth(auxiliary.data(), 2);
  player.prime_synth(2);
  player.play_oneshot_pcm(native.data(), 2);

  return player.hd_pending_audio_state() == pending &&
         player.restore_hd_pending_audio(pending) ==
             obs::AudioHdRestoreCode::invalid_pending_audio &&
         player.freeze_production(999) == limit &&
         player.production_limit() == limit;
}

bool freezes_session_frame(const std::filesystem::path &rom_path) {
  ayther::synth::Rom rom("AYTHER QA PRODUCTION LIMIT");
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
  const auto &last = session.step();
  const auto frame = last.frame_index;
  const auto limit = session.freeze_audio_production();
  if (!limit.frozen || !limit.complete || limit.last_emulation_frame != frame ||
      limit.main_sample_limit != 0)
    return false;

  session.set_input(0, std::uint16_t{0xFFFF});
  const auto &after_close = session.step();
  return after_close.frame_index == frame &&
         session.audio_initial_snapshot().emulation_frame == frame &&
         session.freeze_audio_production() == limit;
}

} // namespace

int main() { // NOLINT(bugprone-exception-escape) -- Test filesystem failure is
             // fatal.
  const auto rom = std::filesystem::current_path() / "production-limit.md";
  const bool valid =
      freezes_player_without_new_samples() && freezes_session_frame(rom);
  std::error_code ignored;
  std::filesystem::remove(rom, ignored);
  return valid ? 0 : 1;
}
