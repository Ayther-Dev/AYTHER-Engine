#include "audio_auxiliary_observation.h"
#include "audio_main_output_observation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <string_view>

namespace qa = ayther::audio_qa;
namespace obs = ayther::engine::audio_observation;

namespace {

constexpr std::size_t signal_frames = 1024;
constexpr std::size_t silence_frames = 512;
constexpr std::size_t submission_count = 3;

template <class T>
const T *field(const obs::FactView &fact, std::string_view name) noexcept {
  for (const auto &item : fact.fields)
    if (item.name == name)
      return std::get_if<T>(&item.value);
  return nullptr;
}

struct Range {
  std::uint64_t input_begin = UINT64_MAX;
  std::uint64_t input_end = 0;
  std::uint64_t output_begin = UINT64_MAX;
  std::uint64_t output_end = 0;
  bool seen = false;
};

struct Bridge {
  qa::IdentitySource ids;
  std::array<Range, submission_count> ranges{};
  std::atomic<std::uint64_t> captured_frames{0};
  std::atomic<bool> complete{true};
  std::atomic<bool> done{false};
  std::atomic<int> failure{0};

  obs::Observer observer() noexcept { return {this, fact, pcm}; }

  static std::uint64_t
  submission(void *, const AudioPlayer::AuxiliarySubmission &value) noexcept {
    return value.submission;
  }

  static void output(void *context,
                     const AudioPlayer::MainOutputBlock &block) noexcept {
    auto &bridge = *static_cast<Bridge *>(context);
    const auto result = qa::MainOutputObservation::emit_with_id(
        bridge.observer(), bridge.ids, block);
    bool complete = result.complete && block.auxiliary_mapping_complete;
    if (result.id)
      for (const auto &span : block.auxiliary_spans) {
        const bool emitted = qa::AuxiliaryObservation::emit_output(
            bridge.observer(), bridge.ids, span, *result.id);
        if (!emitted)
          std::fprintf(
              stderr,
              "emit_failed observation=%llu submission=%llu "
              "input=%llu,%llu output=%llu,%llu rates=%u,%u\n",
              static_cast<unsigned long long>(span.observation_sequence),
              static_cast<unsigned long long>(span.submission),
              static_cast<unsigned long long>(span.input_begin),
              static_cast<unsigned long long>(span.input_end),
              static_cast<unsigned long long>(span.output_begin),
              static_cast<unsigned long long>(span.output_end),
              span.input_sample_rate, span.output_sample_rate);
        complete = emitted && complete;
      }
    if (!complete) {
      bridge.complete.store(false, std::memory_order_relaxed);
      bridge.failure.store(!result.complete                    ? 1
                           : !block.auxiliary_mapping_complete ? 2
                                                               : 3,
                           std::memory_order_relaxed);
    }
  }

  static void fact(void *context, const obs::FactView &value) noexcept {
    auto &bridge = *static_cast<Bridge *>(context);
    if (value.kind != "auxiliary_output_span") {
      bridge.complete.store(false, std::memory_order_relaxed);
      bridge.failure.store(4, std::memory_order_relaxed);
      return;
    }
    const auto *submission_value = field<std::uint64_t>(value, "submission");
    const auto *input_begin = field<std::uint64_t>(value, "input_begin");
    const auto *input_end = field<std::uint64_t>(value, "input_end");
    const auto *output_begin = field<std::uint64_t>(value, "output_begin");
    const auto *output_end = field<std::uint64_t>(value, "output_end");
    const auto *input_rate = field<std::uint64_t>(value, "input_sample_rate");
    const auto *output_rate = field<std::uint64_t>(value, "output_sample_rate");
    const auto *resample_rate =
        field<std::uint64_t>(value, "resample_rate_q32");
    const auto *resample_phase =
        field<std::uint64_t>(value, "resample_phase_begin_q32");
    const auto *support_left =
        field<std::uint64_t>(value, "filter_support_left");
    const auto *support_right =
        field<std::uint64_t>(value, "filter_support_right");
    if (!submission_value || *submission_value == 0 ||
        *submission_value > submission_count || !input_begin || !input_end ||
        !output_begin || !output_end || !input_rate || *input_rate != 44100 ||
        !output_rate || *output_rate != 48000 || !resample_rate ||
        *resample_rate != 3946001204ULL || !resample_phase ||
        *resample_phase >= (std::uint64_t{1} << 32) || !support_left ||
        *support_left != 5 || !support_right || *support_right != 6 ||
        value.causes.size() != 2) {
      bridge.complete.store(false, std::memory_order_relaxed);
      bridge.failure.store(5, std::memory_order_relaxed);
      return;
    }
    auto &range = bridge.ranges[*submission_value - 1];
    range.input_begin = (std::min)(range.input_begin, *input_begin);
    range.input_end = (std::max)(range.input_end, *input_end);
    range.output_begin = (std::min)(range.output_begin, *output_begin);
    range.output_end = (std::max)(range.output_end, *output_end);
    range.seen = true;
  }

  static void pcm(void *context, const obs::PcmView &value) noexcept {
    auto &bridge = *static_cast<Bridge *>(context);
    if (value.range.sample_rate != 48000 ||
        value.range.end <= value.range.begin)
      bridge.complete.store(false, std::memory_order_relaxed);
    const auto frames = value.range.end;
    bridge.captured_frames.store(frames, std::memory_order_release);
    if (frames >= 4096)
      bridge.done.store(true, std::memory_order_release);
  }
};

struct SdlLifetime {
  SdlLifetime() {
    if (!SDL_Init(SDL_INIT_AUDIO))
      throw 1;
  }
  ~SdlLifetime() { SDL_Quit(); }
};

struct DeviceFixture {
  SDL_AudioDeviceID device = 0;
  DeviceFixture() {
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 48000};
    device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (!device)
      throw 2;
  }
  ~DeviceFixture() { SDL_CloseAudioDevice(device); }
};

} // namespace

int main() try {
  SdlLifetime sdl;
  DeviceFixture fixture;
  Bridge bridge;
  AudioPlayer player;
  if (!player.init(ayther::RuntimeOptions{}))
    return 2;
  SDL_AudioSpec device_spec{};
  if (!SDL_GetAudioDeviceFormat(player.device_id(), &device_spec, nullptr) ||
      device_spec.freq != 48000 ||
      !player.set_main_output_observer(&bridge, Bridge::output))
    return 3;
  player.set_auxiliary_submission_observer(&bridge, Bridge::submission);

  std::array<float, signal_frames * 2> first{};
  std::array<float, signal_frames * 2> second{};
  for (std::size_t index = 0; index < first.size(); ++index) {
    first[index] = static_cast<float>(index + 1) / 4096.0F;
    second[index] = -static_cast<float>(index + 1) / 8192.0F;
  }
  player.feed_synth(first.data(), signal_frames);
  player.prime_synth(silence_frames);
  player.feed_synth(second.data(), signal_frames);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!bridge.done.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline)
    SDL_Delay(1);
  if (!player.set_main_output_observer(nullptr, nullptr))
    return 4;

  const auto &first_range = bridge.ranges[0];
  const auto &silence_range = bridge.ranges[1];
  const auto &second_range = bridge.ranges[2];
  const auto base = first_range.output_begin;
  const bool valid =
      bridge.complete.load(std::memory_order_acquire) &&
      bridge.done.load(std::memory_order_acquire) && first_range.seen &&
      silence_range.seen && second_range.seen && first_range.input_begin == 0 &&
      first_range.input_end == 1024 && silence_range.input_begin == 1024 &&
      silence_range.input_end == 1536 && second_range.input_begin == 1536 &&
      second_range.input_end == 2559 && first_range.output_end - base == 1120 &&
      silence_range.output_begin - base == 1109 &&
      silence_range.output_end - base == 1678 &&
      second_range.output_begin - base == 1666;
  if (!valid)
    std::fprintf(
        stderr,
        "conversion_mapping_invalid complete=%d done=%d failure=%d "
        "first=%llu,%llu,%llu,%llu silence=%llu,%llu,%llu,%llu "
        "second=%llu,%llu,%llu,%llu captured=%llu\n",
        bridge.complete.load(), bridge.done.load(), bridge.failure.load(),
        static_cast<unsigned long long>(first_range.input_begin),
        static_cast<unsigned long long>(first_range.input_end),
        static_cast<unsigned long long>(first_range.output_begin),
        static_cast<unsigned long long>(first_range.output_end),
        static_cast<unsigned long long>(silence_range.input_begin),
        static_cast<unsigned long long>(silence_range.input_end),
        static_cast<unsigned long long>(silence_range.output_begin),
        static_cast<unsigned long long>(silence_range.output_end),
        static_cast<unsigned long long>(second_range.input_begin),
        static_cast<unsigned long long>(second_range.input_end),
        static_cast<unsigned long long>(second_range.output_begin),
        static_cast<unsigned long long>(second_range.output_end),
        static_cast<unsigned long long>(bridge.captured_frames.load()));
  return valid ? 0 : 1;
} catch (...) {
  return 5;
}
