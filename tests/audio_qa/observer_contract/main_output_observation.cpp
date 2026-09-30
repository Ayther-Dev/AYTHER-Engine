#include "audio_auxiliary_observation.h"
#include "audio_main_output_observation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <string_view>

namespace qa = ayther::audio_qa;
namespace obs = ayther::engine::audio_observation;

namespace {

constexpr std::size_t capture_limit = 32768;
constexpr std::size_t capture_target = 4096;
constexpr std::size_t submission_count = 3;
constexpr std::size_t signal_frames = 32;
constexpr std::size_t silence_frames = 16;
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

template <class T>
const T *field(const obs::FactView &fact, std::string_view name) noexcept {
  for (const auto &item : fact.fields)
    if (item.name == name)
      return std::get_if<T>(&item.value);
  return nullptr;
}

std::uint64_t number(const obs::FactView &fact,
                     std::string_view name) noexcept {
  const auto *value = field<std::uint64_t>(fact, name);
  return value ? *value : UINT64_MAX;
}

struct Capture {
  std::array<float, capture_limit * 2> samples{};
  std::size_t frames = 0;
  std::size_t blocks = 0;
  std::atomic<bool> valid{true};
  std::atomic<bool> done{false};
  obs::FactId last_pcm;
  std::array<std::atomic<std::uint64_t>, submission_count>
      auxiliary_input_sequences{};
  std::array<std::atomic<std::uint64_t>, submission_count>
      auxiliary_output_begins{};
  std::array<std::atomic<std::uint64_t>, submission_count>
      auxiliary_output_ends{};
  std::array<std::atomic<bool>, submission_count> output_seen{};
  std::atomic<std::size_t> input_count{0};
  std::atomic<std::size_t> output_count{0};
  std::atomic<std::size_t> auxiliary_facts{0};

  obs::Observer observer() noexcept { return {this, receive_fact, receive}; }

  static void receive_fact(void *value, const obs::FactView &fact) noexcept {
    auto &capture = *static_cast<Capture *>(value);
    if (fact.kind == "auxiliary_submission") {
      const auto submission = number(fact, "submission");
      const auto input_begin = number(fact, "input_begin");
      const auto input_end = number(fact, "input_end");
      const auto *silence = field<bool>(fact, "inserted_silence");
      const auto expected_index =
          submission > 0 ? submission - 1 : submission_count;
      const auto expected_begin = expected_index == 0 ? 0
                                  : expected_index == 1
                                      ? signal_frames
                                      : signal_frames + silence_frames;
      const auto expected_end =
          expected_begin +
          (expected_index == 1 ? silence_frames : signal_frames);
      if (capture.input_count.load(std::memory_order_relaxed) >=
              submission_count ||
          expected_index >= submission_count || !fact.causes.empty() ||
          input_begin != expected_begin || input_end != expected_end ||
          !silence || *silence != (expected_index == 1))
        capture.valid.store(false, std::memory_order_relaxed);
      else
        capture.auxiliary_input_sequences[expected_index].store(
            fact.id.sequence, std::memory_order_release);
      capture.input_count.fetch_add(1, std::memory_order_release);
    } else if (fact.kind == "auxiliary_output_span") {
      const auto *input = fact.causes.size() == 2
                              ? std::get_if<obs::FactId>(&fact.causes[0])
                              : nullptr;
      const auto *output = fact.causes.size() == 2
                               ? std::get_if<obs::FactId>(&fact.causes[1])
                               : nullptr;
      const auto submission = number(fact, "submission");
      const auto expected_index =
          submission > 0 ? submission - 1 : submission_count;
      const auto input_begin = number(fact, "input_begin");
      const auto input_end = number(fact, "input_end");
      const auto output_begin = number(fact, "output_begin");
      const auto output_end = number(fact, "output_end");
      const auto *silence = field<bool>(fact, "inserted_silence");
      const auto expected_begin = expected_index == 0 ? 0
                                  : expected_index == 1
                                      ? signal_frames
                                      : signal_frames + silence_frames;
      const auto expected_end =
          expected_begin +
          (expected_index == 1 ? silence_frames : signal_frames);
      if (capture.output_count.load(std::memory_order_relaxed) >=
              submission_count ||
          expected_index >= submission_count || !input || !output ||
          input->producer !=
              static_cast<std::uint32_t>(qa::Producer::auxiliary_output) ||
          input->sequence !=
              capture.auxiliary_input_sequences[expected_index].load(
                  std::memory_order_acquire) ||
          *output != capture.last_pcm || input_begin != expected_begin ||
          input_end != expected_end || !silence ||
          *silence != (expected_index == 1) ||
          output_end - output_begin != expected_end - expected_begin)
        capture.valid.store(false, std::memory_order_relaxed);
      else {
        capture.auxiliary_output_begins[expected_index].store(
            output_begin, std::memory_order_relaxed);
        capture.auxiliary_output_ends[expected_index].store(
            output_end, std::memory_order_relaxed);
        capture.output_seen[expected_index].store(true,
                                                  std::memory_order_release);
      }
      capture.output_count.fetch_add(1, std::memory_order_release);
    } else {
      capture.valid.store(false, std::memory_order_relaxed);
    }
    capture.auxiliary_facts.fetch_add(1, std::memory_order_release);
  }

  static void receive(void *value, const obs::PcmView &pcm) noexcept {
    auto &capture = *static_cast<Capture *>(value);
    if (capture.done.load(std::memory_order_relaxed))
      return;
    if (pcm.range.end <= pcm.range.begin) {
      capture.valid.store(false, std::memory_order_relaxed);
      capture.done.store(true, std::memory_order_release);
      return;
    }
    const auto frames = pcm.range.end - pcm.range.begin;
    const auto expected_bytes = frames * 2 * sizeof(float);
    if (pcm.id.producer != 7 || pcm.id.sequence != capture.blocks + 1 ||
        pcm.capture_point != "sdl_logical_device_postmix" ||
        pcm.range.timeline != "engine_main_output" ||
        pcm.range.sample_rate != 44100 || pcm.range.begin != capture.frames ||
        pcm.format != obs::PcmFormat::f32_le || pcm.channels != 2 ||
        pcm.bytes.size() != expected_bytes || !pcm.causes.empty() ||
        frames > capture_limit - capture.frames) {
      capture.valid.store(false, std::memory_order_relaxed);
      capture.done.store(true, std::memory_order_release);
      return;
    }
    std::memcpy(capture.samples.data() + capture.frames * 2, pcm.bytes.data(),
                pcm.bytes.size());
    capture.last_pcm = pcm.id;
    capture.frames += static_cast<std::size_t>(frames);
    ++capture.blocks;
    if (capture.frames >= capture_target)
      capture.done.store(true, std::memory_order_release);
  }
};

struct Bridge {
  qa::IdentitySource ids;
  Capture capture;
  std::atomic<bool> complete{true};

  static uint64_t
  submission(void *value,
             const AudioPlayer::AuxiliarySubmission &submission) noexcept {
    auto &bridge = *static_cast<Bridge *>(value);
    const auto fact = qa::AuxiliaryObservation::emit_submission(
        bridge.capture.observer(), bridge.ids, submission);
    if (!fact)
      bridge.complete.store(false, std::memory_order_relaxed);
    return fact ? fact->sequence : 0;
  }

  static void output(void *value,
                     const AudioPlayer::MainOutputBlock &block) noexcept {
    auto &bridge = *static_cast<Bridge *>(value);
    const auto output = qa::MainOutputObservation::emit_with_id(
        bridge.capture.observer(), bridge.ids, block);
    bool complete = output.complete && block.auxiliary_mapping_complete;
    if (output.id)
      for (const auto &span : block.auxiliary_spans)
        complete =
            qa::AuxiliaryObservation::emit_output(
                bridge.capture.observer(), bridge.ids, span, *output.id) &&
            complete;
    if (!complete) {
      bridge.complete.store(false, std::memory_order_relaxed);
      bridge.capture.done.store(true, std::memory_order_release);
    }
  }
};

} // namespace

int main() try {
  Bridge bridge;
  AudioPlayer player;
  if (!player.init(ayther::RuntimeOptions{}) ||
      !player.set_main_output_observer(&bridge, Bridge::output))
    return 2;
  player.set_auxiliary_submission_observer(&bridge, Bridge::submission);

  std::array<float, signal_frames * 2> first_signal{};
  std::array<float, signal_frames * 2> second_signal{};
  for (std::size_t index = 0; index < first_signal.size(); ++index) {
    first_signal[index] = static_cast<float>(index + 1) / 256.0F;
    second_signal[index] = -static_cast<float>(index + 1) / 512.0F;
  }
  player.feed_synth(first_signal.data(), signal_frames);
  player.prime_synth(silence_frames);
  player.feed_synth(second_signal.data(), signal_frames);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while ((!bridge.capture.done.load(std::memory_order_acquire) ||
          bridge.capture.auxiliary_facts.load(std::memory_order_acquire) < 6) &&
         std::chrono::steady_clock::now() < deadline)
    SDL_Delay(1);
  if (!player.set_main_output_observer(nullptr, nullptr))
    return 3;

  bool samples_match = true;
  for (std::size_t index = 0; index < submission_count; ++index) {
    const auto begin = bridge.capture.auxiliary_output_begins[index].load(
        std::memory_order_acquire);
    const auto end = bridge.capture.auxiliary_output_ends[index].load(
        std::memory_order_acquire);
    if (!bridge.capture.output_seen[index].load(std::memory_order_acquire) ||
        begin >= end || end > bridge.capture.frames) {
      samples_match = false;
      continue;
    }
    const auto offset = static_cast<std::size_t>(begin) * 2;
    const auto count = static_cast<std::size_t>(end - begin) * 2;
    using SampleDifference =
        std::vector<float>::const_iterator::difference_type;
    const auto sample_begin =
        bridge.capture.samples.cbegin() + static_cast<SampleDifference>(offset);
    const auto sample_end = sample_begin + static_cast<SampleDifference>(count);
    if (index == 0)
      samples_match =
          samples_match &&
          std::equal(first_signal.begin(), first_signal.end(), sample_begin);
    else if (index == 1)
      samples_match = samples_match &&
                      std::all_of(sample_begin, sample_end,
                                  [](float sample) { return sample == 0.0F; });
    else
      samples_match =
          samples_match &&
          std::equal(second_signal.begin(), second_signal.end(), sample_begin);
  }

  return bridge.capture.done.load(std::memory_order_acquire) &&
                 bridge.capture.valid.load(std::memory_order_acquire) &&
                 bridge.complete.load(std::memory_order_relaxed) &&
                 bridge.capture.blocks > 0 && samples_match &&
                 bridge.capture.auxiliary_facts.load(
                     std::memory_order_acquire) == 6 &&
                 bridge.capture.input_count.load(std::memory_order_acquire) ==
                     submission_count &&
                 bridge.capture.output_count.load(std::memory_order_acquire) ==
                     submission_count
             ? 0
             : 1;
} catch (...) {
  return 4;
}
