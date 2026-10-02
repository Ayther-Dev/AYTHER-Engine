#include "../../../tools/common/synth_rom.h"

#include <ayther/audio_player.h>
#include <ayther/ayther_session.h>
#include <ayther/engine/audio_hd_state.hpp>

#include <array>
#include <cstdint>
#include <filesystem>

namespace obs = ayther::engine::audio_observation;

namespace {

struct PendingSeed {
  std::int16_t sample = 0;
  std::uint64_t source_hash = 0;
};

obs::AudioHdPendingAudioState pending(const PendingSeed seed) {
  obs::AudioHdPendingAudioState state;
  for (std::int16_t offset = 0; offset < 8; ++offset)
    state.main_pcm.push_back(static_cast<std::int16_t>(seed.sample + offset));
  state.main_batches.push_back({seed.source_hash, 0, 4});
  state.original_batches.push_back(
      {seed.source_hash, 0, 4, 44, 1198, true, true});
  state.frame_mute_hashes = {0x1010};
  state.user_mute_hashes = {0x2020};
  state.timeline_samples = 1200;
  state.frame_mark = 2;
  state.current_emulation_frame = 44;
  state.current_frame_boundary = 1198;
  state.main_output_samples = 1170;
  state.auxiliary_input_samples = 80;
  state.auxiliary_consumed_input = 80;
  state.auxiliary_discard_before = 80;
  state.auxiliary_submission = 3;
  state.auxiliary_resample_phase_q32 = 0;
  state.current_frame_known = true;
  return state;
}

struct RequestSeed {
  std::int16_t sample = 0;
  std::uint64_t key = 0;
  std::uint64_t source_hash = 0;
};

obs::AudioHdRequestsPendingState supplied_state(const RequestSeed seed) {
  obs::AudioHdRequestsPendingState state;
  state.requests.push_back({seed.key, "audio/music/stage.wav", 40, 900, 930,
                            1U << 3, 0.65F, true, false, 801});
  state.fired_requests.push_back(
      {obs::AudioHdFiredRequestKind::sequence, seed.key + 1, 41});
  state.fired_requests.push_back(
      {obs::AudioHdFiredRequestKind::event, seed.key + 2, 42});
  state.pending_audio = pending({seed.sample, seed.source_hash});
  return state;
}

bool first_audio_input_replaces_inherited_state() {
  AudioPlayer player;
  if (!player.init(ayther::RuntimeOptions{}))
    return false;
  const auto inherited = pending({900, 0xDEAD});
  const auto supplied = pending({100, 0xABCD});
  if (player.restore_hd_pending_audio(inherited) !=
          obs::AudioHdRestoreCode::restored ||
      player.restore_hd_pending_audio(supplied) !=
          obs::AudioHdRestoreCode::restored)
    return false;

  constexpr std::array<std::int16_t, 4> first_input{300, 301, 302, 303};
  player.buffer_emulator(0xBEEF, first_input.data(), 2);
  auto expected = supplied;
  expected.main_pcm.insert(expected.main_pcm.end(), first_input.begin(),
                           first_input.end());
  expected.main_batches.push_back({0xBEEF, 4, 2});
  expected.original_batches.push_back({0xBEEF, 4, 2, 44, 1198, true, true});
  const auto captured = player.hd_pending_audio_state();
  return captured == expected;
}

bool session_snapshot_matches_supplied_state(
    const std::filesystem::path &rom_path) {
  ayther::synth::Rom rom("AYTHER QA REQUEST RESTORE");
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
  header.game_state_identity = "requests-pending-state";
  header.emulation_frame = frame;
  header.sections = obs::kAudioHdStateRequiredSections;

  const auto inherited = supplied_state({800, 0xD001, 0xDEAD});
  const auto supplied = supplied_state({100, 0xA001, 0xABCD});
  if (!session
           .restore_audio_hd_requests_pending(header, "requests-pending-state",
                                              inherited)
           .restored() ||
      !session
           .restore_audio_hd_requests_pending(header, "requests-pending-state",
                                              supplied)
           .restored())
    return false;
  if (session.audio_hd_requests_pending_state() != supplied)
    return false;

  const auto snapshot = session.audio_initial_snapshot();
  if (snapshot.initialization != obs::AudioInitializationMode::restored ||
      snapshot.emulation_frame != frame || snapshot.requests.size() != 1 ||
      snapshot.fired_requests.size() != 2 || snapshot.pending_audio.size() != 2)
    return false;
  const auto &request = snapshot.requests.front();
  if (request.business_key != 0xA001 ||
      request.asset != "audio/music/stage.wav" || request.start_frame != 40 ||
      request.end_frame != 900 || request.cut_frame != 930 ||
      request.channel_mask != (1U << 3) || request.gain != 0.65F ||
      !request.looping || request.sequence_substitution ||
      request.occurrence != 801)
    return false;
  if (snapshot.fired_requests[0].kind !=
          obs::AudioInitialFiredRequestKind::sequence ||
      snapshot.fired_requests[0].business_key != 0xA002 ||
      snapshot.fired_requests[0].start_frame_plus_one != 41 ||
      snapshot.fired_requests[1].kind !=
          obs::AudioInitialFiredRequestKind::event ||
      snapshot.fired_requests[1].business_key != 0xA003 ||
      snapshot.fired_requests[1].start_frame_plus_one != 42)
    return false;
  if (snapshot.pending_audio[0].kind !=
          obs::AudioInitialPendingKind::main_staging ||
      snapshot.pending_audio[0].position != 1200 ||
      snapshot.pending_audio[0].frames != 4 ||
      snapshot.pending_audio[1].kind !=
          obs::AudioInitialPendingKind::main_batch ||
      snapshot.pending_audio[1].identity != 0xABCD ||
      snapshot.pending_audio[1].position != 1200 ||
      snapshot.pending_audio[1].frames != 4)
    return false;

  auto invalid = supplied;
  invalid.pending_audio.main_batches.front().frame_offset = 1;
  if (session
          .restore_audio_hd_requests_pending(header, "requests-pending-state",
                                             invalid)
          .code != obs::AudioHdRestoreCode::invalid_pending_audio)
    return false;
  return session.audio_hd_requests_pending_state() == supplied;
}

} // namespace

int main() { // NOLINT(bugprone-exception-escape) -- Test filesystem failure is
             // fatal.
  const auto rom =
      std::filesystem::current_path() / "requests-pending-restore.md";
  const bool valid = first_audio_input_replaces_inherited_state() &&
                     session_snapshot_matches_supplied_state(rom);
  std::error_code ignored;
  std::filesystem::remove(rom, ignored);
  return valid ? 0 : 1;
}
