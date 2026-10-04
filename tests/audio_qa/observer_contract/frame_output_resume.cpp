// Spec 002 (P-9, DI-12): after pause_after_drain, frame k+1's audio follows
// frame k's on the device line with no silence between them, also on the
// first resume early in a take, when the pause outlasts the stall detector.
//
// Frames 0..k are produced, the device drains them and pauses; after a pause
// longer than 250 ms the transport resumes and k+1.. are produced. On the
// `engine_main_output` line (48 kHz device) the boundary of k+1 must come
// within one frame of samples (plus one device period of tolerance) after
// the boundary of k: re-priming the backlog with silence would push it ~70 ms
// later.
#include <ayther/audio_player.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr std::size_t kBefore = 18; // frames 0..17 before the pause
constexpr std::size_t kFrames = 30;
constexpr std::size_t kSamplesPerFrame = 735;
constexpr std::uint64_t kDevicePeriod = 512;

struct Capture {
  std::array<std::uint64_t, kFrames> position{};
  std::array<std::atomic<bool>, kFrames> seen{};
  std::atomic<bool> valid{true};

  static void output(void *, const AudioPlayer::MainOutputBlock &) noexcept {}

  static void boundary(void *value,
                       const AudioPlayer::FrameOutputBoundary &b) noexcept {
    auto &capture = *static_cast<Capture *>(value);
    if (b.emulation_frame >= kFrames || !b.complete) {
      capture.valid.store(false, std::memory_order_relaxed);
      return;
    }
    capture.position[b.emulation_frame] = b.output_position;
    capture.seen[b.emulation_frame].store(true, std::memory_order_release);
  }
};

bool wait_for(const std::atomic<bool> &flag, int milliseconds) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(milliseconds);
  while (!flag.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline)
    SDL_Delay(1);
  return flag.load(std::memory_order_acquire);
}

void produce(AudioPlayer &player, std::size_t frame,
             std::vector<std::int16_t> &pcm) {
  std::fill(pcm.begin(), pcm.end(), static_cast<std::int16_t>(frame * 100));
  player.mark_frame_boundary(frame);
  player.buffer_emulator(0x2000 + frame, pcm.data(), kSamplesPerFrame);
  player.flush_emulator();
}

} // namespace

int main() try {
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    return 2;
  const SDL_AudioSpec device_spec{SDL_AUDIO_F32, 2, 48000};
  const SDL_AudioDeviceID holder =
      SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &device_spec);
  if (holder == 0)
    return 2;
  Capture capture;
  AudioPlayer player;
  if (!player.init(ayther::RuntimeOptions{}) ||
      !player.set_main_output_observer(&capture, Capture::output))
    return 2;
  player.set_frame_output_observer(&capture, Capture::boundary);

  std::vector<std::int16_t> pcm(kSamplesPerFrame * 2);
  int failures = 0;
  const auto check = [&failures](bool condition, const char *message) {
    if (!condition)
      ++failures;
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
  };

  // Early in the take, paced like a host: one frame every ~16 ms.
  for (std::size_t frame = 0; frame < kBefore; ++frame) {
    produce(player, frame, pcm);
    SDL_Delay(16);
  }
  const auto drain = player.pause_after_drain(std::chrono::milliseconds(2000));
  check(drain.code == AudioPlayer::DrainResult::Code::drained,
        "the device drains frame k and pauses");
  check(capture.seen[kBefore - 1].load(), "frame k reached the device");
  SDL_Delay(400); // longer than the 250 ms stall detector
  player.resume_transport();
  for (std::size_t frame = kBefore; frame < kFrames; ++frame) {
    produce(player, frame, pcm);
    SDL_Delay(16);
  }
  (void)wait_for(capture.seen[kFrames - 1], 3000);
  if (!player.set_main_output_observer(nullptr, nullptr))
    return 3;
  SDL_CloseAudioDevice(holder);

  const auto k = kBefore - 1;
  const bool both = capture.seen[k].load() && capture.seen[k + 1].load();
  check(both && capture.valid.load(), "frames k and k+1 report boundaries");
  // One 44.1 kHz frame on the 48 kHz line.
  const std::uint64_t frame_samples =
      (kSamplesPerFrame * 48000 + 44099) / 44100;
  const std::uint64_t gap =
      both ? capture.position[k + 1] - capture.position[k] : 0;
  std::uint64_t steady = 0;
  for (std::size_t frame = kBefore + 2; frame < kFrames; ++frame)
    if (capture.seen[frame].load() && capture.seen[frame - 1].load())
      steady = std::max(steady,
                        capture.position[frame] - capture.position[frame - 1]);
  std::printf("  gap k->k+1: %llu samples (one frame: %llu; budget %llu); "
              "largest later gap: %llu\n",
              static_cast<unsigned long long>(gap),
              static_cast<unsigned long long>(frame_samples),
              static_cast<unsigned long long>(frame_samples + kDevicePeriod),
              static_cast<unsigned long long>(steady));
  check(both && gap <= frame_samples + kDevicePeriod,
        "P-9: k+1's audio follows k's within one frame (+ one device period) "
        "after the first resume");
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (...) {
  return 4;
}
