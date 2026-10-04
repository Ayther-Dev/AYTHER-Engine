// Spec 002 (P-9, DI-12): every audible frame reports where its audio starts on
// the device line (`engine_main_output`), after rate control.
//
// SDL's dummy driver runs the device at 48 kHz, so the 44.1 kHz emulator
// stream is resampled (SDL 3.4.8, the version the main-mix mapping supports).
// Frames alternate between silence and a constant level, so each frame start is
// a step in the captured device PCM. The step must cross half its level within
// two output samples of the reported boundary, for frames produced before and
// after the rate-control ratio changes.
#include <ayther/audio_player.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr std::size_t kFrames = 60;
constexpr std::size_t kSamplesPerFrame = 735;
constexpr std::size_t kChangeAfter = 30; // ratio changes once this is output
constexpr float kRatio = 1.005F;
constexpr std::int16_t kLevel = 12000;
constexpr std::size_t kCaptureLimit = 192000; // 4 s at 48 kHz

struct Capture {
  std::vector<float> left = std::vector<float>(kCaptureLimit);
  std::atomic<std::size_t> frames{0};
  std::atomic<bool> pcm_valid{true};
  std::array<std::uint64_t, kFrames> position{};
  std::array<std::uint64_t, kFrames> rate{};
  std::array<std::atomic<bool>, kFrames> seen{};
  std::atomic<std::size_t> boundaries{0};
  std::atomic<bool> boundary_valid{true};
  std::atomic<std::uint32_t> sample_rate{0};

  static void output(void *value,
                     const AudioPlayer::MainOutputBlock &block) noexcept {
    auto &capture = *static_cast<Capture *>(value);
    const auto have = capture.frames.load(std::memory_order_relaxed);
    if (block.channels != 2 || block.sample_begin != have) {
      capture.pcm_valid.store(false, std::memory_order_relaxed);
      return;
    }
    const auto count =
        static_cast<std::size_t>(block.sample_end - block.sample_begin);
    const auto *samples = reinterpret_cast<const float *>(block.bytes);
    const auto keep = (std::min)(count, kCaptureLimit - have);
    for (std::size_t index = 0; index < keep; ++index)
      capture.left[have + index] = samples[index * 2];
    capture.frames.store(have + keep, std::memory_order_release);
  }

  static void boundary(void *value,
                       const AudioPlayer::FrameOutputBoundary &b) noexcept {
    auto &capture = *static_cast<Capture *>(value);
    if (b.emulation_frame >= kFrames || !b.complete ||
        capture.seen[b.emulation_frame].load(std::memory_order_relaxed)) {
      capture.boundary_valid.store(false, std::memory_order_relaxed);
      return;
    }
    capture.position[b.emulation_frame] = b.output_position;
    capture.rate[b.emulation_frame] = b.resample_rate_q32;
    capture.sample_rate.store(b.output_sample_rate, std::memory_order_relaxed);
    capture.seen[b.emulation_frame].store(true, std::memory_order_release);
    capture.boundaries.fetch_add(1, std::memory_order_release);
  }
};

bool wait_for(const std::atomic<bool> &flag, int seconds) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (!flag.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline)
    SDL_Delay(1);
  return flag.load(std::memory_order_acquire);
}

// The first output sample at or after `from` whose level crosses half the
// step towards `up`.
std::size_t crossing(const Capture &capture, std::size_t from, bool up,
                     std::size_t limit) {
  const float half = 0.5F * static_cast<float>(kLevel) / 32768.0F;
  for (std::size_t index = from; index < limit; ++index)
    if (up ? capture.left[index] >= half : capture.left[index] <= half)
      return index;
  return limit;
}

std::uint64_t expected_rate(float ratio) {
  const auto source = static_cast<std::uint64_t>(
      static_cast<int>(static_cast<float>(44100) * ratio));
  return ((source << 32U) + 48000 - 1) / 48000;
}

} // namespace

int main() try {
  // SDL's dummy device opens at 44.1 kHz unless a logical device asks for
  // more: hold one at 48 kHz so the player's device shares that format.
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
  player.set_drc_enabled(false);
  player.set_frame_output_observer(&capture, Capture::boundary);

  std::vector<std::int16_t> pcm(kSamplesPerFrame * 2);
  int failures = 0;
  for (std::size_t frame = 0; frame < kFrames; ++frame) {
    if (frame == kChangeAfter + 1) {
      // Every frame up to kChangeAfter has left the device: the ones after
      // it are resampled with the new ratio.
      if (!wait_for(capture.seen[kChangeAfter], 5)) {
        std::printf("[FAIL] frame %zu was never reported\n", kChangeAfter);
        ++failures;
      }
      if (!player.set_rate_ratio_for_test(kRatio)) {
        std::printf("[FAIL] the rate ratio cannot be set\n");
        ++failures;
      }
    }
    std::fill(pcm.begin(), pcm.end(),
              static_cast<std::int16_t>(frame % 2 == 1 ? kLevel : 0));
    player.mark_frame_boundary(frame);
    player.buffer_emulator(0x1000 + frame, pcm.data(), kSamplesPerFrame);
    player.flush_emulator();
  }
  (void)wait_for(capture.seen[kFrames - 1], 5);
  // Let the device output the last frame whole.
  const auto tail_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
  while (std::chrono::steady_clock::now() < tail_deadline)
    SDL_Delay(1);
  if (!player.set_main_output_observer(nullptr, nullptr))
    return 3;
  SDL_CloseAudioDevice(holder);

  const auto boundaries = capture.boundaries.load(std::memory_order_acquire);
  const auto captured = capture.frames.load(std::memory_order_acquire);
  std::printf("  boundaries=%zu captured=%zu output_rate=%u\n", boundaries,
              captured, capture.sample_rate.load());
  if (boundaries != kFrames || !capture.boundary_valid.load() ||
      !capture.pcm_valid.load() || capture.sample_rate.load() != 48000) {
    std::printf("[FAIL] every frame is reported once, on the 48 kHz line\n");
    ++failures;
  } else {
    std::printf("[PASS] every frame is reported once, on the 48 kHz line\n");
  }

  if (boundaries == kFrames) {
    int worst = 0;
    bool ordered = true;
    bool rates = true;
    bool steps = true;
    for (std::size_t frame = 1; frame < kFrames; ++frame) {
      ordered =
          ordered && capture.position[frame] > capture.position[frame - 1];
      const auto want = expected_rate(frame <= kChangeAfter ? 1.0F : kRatio);
      rates = rates && capture.rate[frame] == want;
      const auto at = static_cast<std::size_t>(capture.position[frame]);
      const auto from = at > 8 ? at - 8 : 0;
      const auto found = crossing(capture, from, frame % 2 == 1, captured);
      const int offset = static_cast<int>(found) - static_cast<int>(at);
      if (std::abs(offset) > std::abs(worst))
        worst = offset;
      steps = steps && found < captured && std::abs(offset) <= 2;
      if (frame == kChangeAfter || frame == kChangeAfter + 1 ||
          frame == kFrames - 1)
        std::printf(
            "  frame %zu: boundary %llu, step at %zu (%+d), rate %llu\n", frame,
            static_cast<unsigned long long>(at), found, offset,
            static_cast<unsigned long long>(capture.rate[frame]));
    }
    std::printf("  worst step offset: %+d samples\n", worst);
    std::printf("[%s] boundaries increase frame by frame\n",
                ordered ? "PASS" : "FAIL");
    std::printf("[%s] each boundary carries the ratio in force (1.0, then "
                "%.3f)\n",
                rates ? "PASS" : "FAIL", static_cast<double>(kRatio));
    std::printf("[%s] the frame's audio starts within 2 samples of its "
                "boundary, across the ratio change\n",
                steps ? "PASS" : "FAIL");
    failures += ordered ? 0 : 1;
    failures += rates ? 0 : 1;
    failures += steps ? 0 : 1;
  }
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (...) {
  return 4;
}
