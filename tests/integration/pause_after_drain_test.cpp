// Spec 002, BR-033 (RF-4.1, RF-4.4): pause with drain (contracts.md C4) on
// the audio player, with SDL's dummy driver as the capture backend. After
// pause_after_drain at frame k the last PCM played is the end of k, the HD
// voices stay where they were, and after resuming the first PCM is k+1's.
#include <ayther/audio_player.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

namespace obs = ayther::engine::audio_observation;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kFrameSamples = 735; // 44100 / 60 stereo frames
constexpr int kPausedAt = 12;

// Every left-channel sample the device pulled, in order.
struct Capture {
  std::mutex mutex;
  std::vector<float> left;

  static void receive(void *context,
                      const AudioPlayer::MainOutputBlock &block) noexcept {
    auto &capture = *static_cast<Capture *>(context);
    if (block.bytes == nullptr || block.channels != 2)
      return;
    const auto *samples = reinterpret_cast<const float *>(block.bytes);
    const std::size_t frames = block.byte_count / (2 * sizeof(float));
    const std::lock_guard<std::mutex> lock(capture.mutex);
    for (std::size_t i = 0; i < frames; ++i)
      capture.left.push_back(samples[i * 2]);
  }
  std::vector<float> snapshot() {
    const std::lock_guard<std::mutex> lock(mutex);
    return left;
  }
};

// The frame a sample belongs to: frame f is written as f * 1000 (the HD
// voice adds 1).
int frame_of(float sample) {
  return static_cast<int>(std::lround(sample * 32768.0F / 1000.0F));
}

void produce(AudioPlayer &player, int frame) {
  std::vector<std::int16_t> pcm(kFrameSamples * 2,
                                static_cast<std::int16_t>(frame * 1000));
  player.mark_frame_boundary(static_cast<std::uint64_t>(frame));
  player.buffer_emulator(0xF000U + static_cast<std::uint64_t>(frame),
                         pcm.data(), kFrameSamples);
  player.flush_emulator();
}

obs::AudioHdVoicesState long_voice() {
  obs::AudioHdVoicesState state;
  state.pcm_assets.push_back({1, std::vector<std::int16_t>(44100 * 4, 1)});
  obs::AudioHdVoiceState voice;
  voice.occurrence = 1;
  voice.cause_producer = 1;
  voice.cause_sequence = 1;
  voice.business_key = 0xB033;
  voice.pcm_identity = 1;
  state.voices.push_back(voice);
  state.started = 1;
  return state;
}

template <typename Done> bool wait_for(Done done, std::chrono::seconds limit) {
  const auto deadline = Clock::now() + limit;
  while (!done() && Clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  return done();
}
} // namespace

int main() try {
  AudioPlayer player;
  check(player.init(ayther::RuntimeOptions{}), "the player opens the device");
  player.set_drc_enabled(false);
  Capture capture;
  check(player.set_main_output_observer(&capture, Capture::receive),
        "the capture observer is installed");
  check(player.restore_hd_voices(long_voice()) ==
                obs::AudioHdRestoreCode::restored &&
            player.hd_voice_count() == 1,
        "an HD voice is playing");

  for (int f = 1; f <= kPausedAt; ++f)
    produce(player, f);
  const AudioPlayer::DrainResult drained =
      player.pause_after_drain(std::chrono::milliseconds(2000));
  check(drained.code == AudioPlayer::DrainResult::Code::drained &&
            drained.remaining_frames == 0,
        "RF-4.1: pause_after_drain drains within its limit");

  const std::vector<float> at_pause = capture.snapshot();
  int last_frame = 0;
  std::size_t produced = 0;
  for (const float s : at_pause)
    if (std::fabs(s) > 0.0F) {
      last_frame = frame_of(s);
      ++produced;
    }
  std::printf("pause_after_drain pulled=%zu nonsilent=%zu last_frame=%d\n",
              at_pause.size(), produced, last_frame);
  check(last_frame == kPausedAt && produced == kPausedAt * kFrameSamples,
        "RF-4.1: the last PCM played is the end of frame k, all of it");

  const obs::AudioHdVoicesState voices = player.hd_voices_state();
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  check(player.hd_voices_state() == voices && player.hd_voice_count() == 1,
        "RF-4.4: the HD voices stay where they were while paused");
  check(capture.snapshot().size() == at_pause.size(),
        "RF-4.1: the device plays nothing while paused");

  player.resume_transport();
  produce(player, kPausedAt + 1);
  const std::size_t pause_point = at_pause.size();
  int first_after = 0;
  check(wait_for(
            [&] {
              const std::vector<float> now = capture.snapshot();
              for (std::size_t i = pause_point; i < now.size(); ++i)
                if (std::fabs(now[i]) > 0.0F) {
                  first_after = frame_of(now[i]);
                  return true;
                }
              return false;
            },
            std::chrono::seconds(2)),
        "the device plays again after resume_transport");
  std::printf("pause_after_drain first_frame_after_resume=%d\n", first_after);
  check(first_after == kPausedAt + 1,
        "RF-4.4: after resuming, the first PCM is that of frame k+1");

  (void)player.set_main_output_observer(nullptr, nullptr);
  player.shutdown();
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
