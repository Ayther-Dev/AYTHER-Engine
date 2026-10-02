#include "../../../tools/common/synth_rom.h"

#include <ayther/audio_player.h>
#include <ayther/ayther_session.h>
#include <ayther/engine/audio_hd_state.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

namespace obs = ayther::engine::audio_observation;

namespace {

constexpr std::size_t kPendingFrames = 227;
constexpr std::size_t kCaptureFrames = 4096;

struct Capture {
  std::array<float, kCaptureFrames * 2> samples{};
  std::atomic<std::size_t> frames{0};
  std::atomic<bool> valid{true};

  static void receive(void *context,
                      const AudioPlayer::MainOutputBlock &block) noexcept {
    auto &capture = *static_cast<Capture *>(context);
    const auto block_frames = block.sample_end - block.sample_begin;
    const auto begin = capture.frames.load(std::memory_order_relaxed);
    if (!block.bytes || block.channels != 2 || block.sample_rate != 44100 ||
        block.sample_end <= block.sample_begin ||
        block.byte_count != block_frames * 2 * sizeof(float) ||
        block_frames > kCaptureFrames - begin) {
      capture.valid.store(false, std::memory_order_release);
      return;
    }
    std::memcpy(capture.samples.data() + begin * 2, block.bytes,
                block.byte_count);
    capture.frames.store(begin + static_cast<std::size_t>(block_frames),
                         std::memory_order_release);
  }
};

obs::AudioHdPendingAudioState pending_audio() {
  obs::AudioHdPendingAudioState state;
  state.main_pcm.resize(kPendingFrames * 2);
  for (std::size_t index = 0; index < state.main_pcm.size(); ++index)
    state.main_pcm[index] =
        static_cast<std::int16_t>(-12000 + static_cast<int>(index * 19));
  state.main_batches.push_back({0xA084, 0, kPendingFrames});
  state.original_batches.push_back(
      {0xA084, 0, kPendingFrames, 88, 700, true, true});
  state.timeline_samples = 700;
  state.current_emulation_frame = 88;
  state.current_frame_boundary = 700;
  state.current_frame_known = true;
  return state;
}

obs::AudioHdVoicesState active_voice() {
  obs::AudioHdVoicesState state;
  state.pcm_assets.push_back({1, std::vector<std::int16_t>(32, 900)});
  obs::AudioHdVoiceState voice;
  voice.occurrence = 84;
  voice.cause_producer = 7;
  voice.cause_sequence = 84;
  voice.business_key = 0xA084;
  voice.pcm_identity = 1;
  voice.source_position = 4;
  voice.output_start = 700;
  state.voices.push_back(voice);
  state.started = 1;
  return state;
}

bool contains_pending_block(const Capture &capture,
                            const obs::AudioHdPendingAudioState &pending) {
  const auto captured = capture.frames.load(std::memory_order_acquire);
  if (!capture.valid.load(std::memory_order_acquire) ||
      captured < kPendingFrames)
    return false;
  for (std::size_t begin = 0; begin + kPendingFrames <= captured; ++begin) {
    bool equal = true;
    for (std::size_t index = 0; index < pending.main_pcm.size(); ++index) {
      const auto expected =
          static_cast<float>(pending.main_pcm[index]) / 32768.0F;
      if (std::fabs(capture.samples[begin * 2 + index] - expected) >
          0.000001F) {
        equal = false;
        break;
      }
    }
    if (equal)
      return true;
  }
  return false;
}

bool drains_exact_staging() {
  AudioPlayer player;
  if (!player.init(ayther::RuntimeOptions{}) ||
      !SDL_PauseAudioDevice(player.device_id())) {
    std::fputs("player_or_pause_failed\n", stderr);
    return false;
  }
  player.set_drc_enabled(false);

  Capture capture;
  if (!player.set_main_output_observer(&capture, Capture::receive)) {
    std::fputs("observer_install_failed\n", stderr);
    return false;
  }
  const auto pending = pending_audio();
  if (player.restore_hd_pending_audio(pending) !=
      obs::AudioHdRestoreCode::restored) {
    std::fputs("pending_restore_failed\n", stderr);
    return false;
  }

  const auto early = player.drain_frozen_audio();
  if (early.accepted || player.hd_pending_audio_state() != pending) {
    std::fputs("early_drain_changed_state\n", stderr);
    return false;
  }

  if (player.restore_hd_voices(active_voice()) !=
          obs::AudioHdRestoreCode::restored ||
      player.hd_voice_count() != 1) {
    std::fputs("voice_setup_failed\n", stderr);
    return false;
  }
  const auto limit = player.freeze_production(88);
  const auto before_voice_close = player.drain_frozen_audio();
  if (before_voice_close.accepted ||
      player.pending_frames() != kPendingFrames) {
    std::fputs("drain_accepted_active_voice\n", stderr);
    return false;
  }
  const auto voices = player.finalize_hd_voices_for_test();
  const auto drained = player.drain_frozen_audio();
  if (!voices.accepted || voices.finalized_voices != 1 || !drained.accepted ||
      !drained.complete || drained.drained_main_frames != kPendingFrames ||
      drained.remaining_main_frames != 0 ||
      drained.main_sample_limit != limit.main_sample_limit ||
      player.pending_frames() != 0 ||
      player.timeline_samples() != limit.main_sample_limit ||
      player.production_limit() != limit) {
    std::fprintf(stderr,
                 "drain_contract_failed accepted=%d complete=%d drained=%llu "
                 "remaining=%llu limit=%llu timeline=%llu pending=%zu\n",
                 drained.accepted, drained.complete,
                 static_cast<unsigned long long>(drained.drained_main_frames),
                 static_cast<unsigned long long>(drained.remaining_main_frames),
                 static_cast<unsigned long long>(drained.main_sample_limit),
                 static_cast<unsigned long long>(player.timeline_samples()),
                 player.pending_frames());
    return false;
  }

  const auto repeated = player.drain_frozen_audio();
  if (!repeated.accepted || !repeated.complete ||
      repeated.drained_main_frames != 0 ||
      repeated.main_sample_limit != limit.main_sample_limit ||
      !SDL_ResumeAudioDevice(player.device_id())) {
    std::fputs("repeat_or_resume_failed\n", stderr);
    return false;
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (capture.frames.load(std::memory_order_acquire) < kPendingFrames &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const bool captured = contains_pending_block(capture, pending);
  auto output_drained = player.drain_frozen_audio();
  const auto output_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!output_drained.output_complete &&
         std::chrono::steady_clock::now() < output_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    output_drained = player.drain_frozen_audio();
  }
  if (!captured)
    std::fprintf(stderr, "pending_block_not_captured frames=%zu valid=%d\n",
                 capture.frames.load(std::memory_order_acquire),
                 capture.valid.load(std::memory_order_acquire));
  return output_drained.output_complete &&
         output_drained.output_sample_limit ==
             capture.frames.load(std::memory_order_acquire) &&
         player.set_main_output_observer(nullptr, nullptr) && captured &&
         player.production_limit() == limit;
}

bool session_drain_does_not_step(const std::filesystem::path &rom_path) {
  ayther::synth::Rom rom("AYTHER QA FROZEN DRAIN");
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
  if (!session.finalize_audio_hd_voices_for_test().accepted)
    return false;
  const auto drained = session.drain_frozen_audio();
  const auto &after = session.step();
  return drained.accepted && drained.complete && drained.output_complete &&
         drained.output_sample_limit == 0 && drained.drained_main_frames == 0 &&
         after.frame_index == frame &&
         session.freeze_audio_production() == limit;
}

} // namespace

int main() { // NOLINT(bugprone-exception-escape) -- Test filesystem failure is
             // fatal.
  const auto rom = std::filesystem::current_path() / "frozen-drain.md";
  const bool valid = drains_exact_staging() && session_drain_does_not_step(rom);
  std::error_code ignored;
  std::filesystem::remove(rom, ignored);
  return valid ? 0 : 1;
}
