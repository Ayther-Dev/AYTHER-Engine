#include "audio_mix_observation.h"

#include <ayther/audio_hd_mixer.h>
#include <ayther/engine/audio_fact_queue.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace qa = ayther::audio_qa;
namespace obs = ayther::engine::audio_observation;
namespace {

constexpr std::size_t occurrence_limit = 256U;
using Queue = obs::BoundedFactQueue<occurrence_limit>;

template <class T>
const T *field(const obs::FactView &fact,
               const std::string_view name) noexcept {
  for (const auto &item : fact.fields)
    if (item.name == name)
      return std::get_if<T>(&item.value);
  return nullptr;
}

struct ObservationSink {
  obs::ObservationOverflowCounter overflow;
  std::unique_ptr<Queue> queue = std::make_unique<Queue>(&overflow);
  bool valid = true;

  [[nodiscard]] obs::Observer observer() noexcept {
    return {this, receive, nullptr};
  }

  static void receive(void *value, const obs::FactView &fact) noexcept {
    auto &sink = *static_cast<ObservationSink *>(value);
    const auto result = sink.queue->try_push(fact);
    sink.valid = sink.valid && (result == obs::FactPushResult::accepted ||
                                result == obs::FactPushResult::full);
  }
};

struct Capture {
  std::array<bool, occurrence_limit + 1U> seen{};
  std::size_t count{};
  bool valid = true;

  static void consume(void *value, const obs::FactView &fact) noexcept {
    auto &capture = *static_cast<Capture *>(value);
    const auto *occurrence = field<obs::OccurrenceId>(fact, "occurrence");
    const auto *mix_begin = field<std::uint64_t>(fact, "mix_begin");
    const auto *mix_end = field<std::uint64_t>(fact, "mix_end");
    const auto *track_begin = field<std::uint64_t>(fact, "track_begin");
    const auto *track_end = field<std::uint64_t>(fact, "track_end");
    const auto *complete = field<bool>(fact, "links_complete");
    const auto *cause = fact.causes.size() == 1U
                            ? std::get_if<obs::FactId>(&fact.causes.front())
                            : nullptr;
    if (fact.kind != "hd_mix_participant" || !occurrence || !mix_begin ||
        !mix_end || !track_begin || !track_end || !complete || !*complete ||
        !cause || cause->producer != 5U ||
        cause->sequence != occurrence->value || *mix_begin != 0U ||
        *mix_end != 1U || *track_begin != 0U || *track_end != 1U ||
        occurrence->value == 0U || occurrence->value > capture.seen.size() ||
        capture.seen[static_cast<std::size_t>(occurrence->value - 1U)]) {
      capture.valid = false;
      return;
    }
    capture.seen[static_cast<std::size_t>(occurrence->value - 1U)] = true;
    ++capture.count;
  }
};

struct RunResult {
  std::array<std::int16_t, 2> output{};
  std::size_t starts{};
  std::size_t remaining_voices{};
  std::size_t observed{};
  obs::ObservationOverflowSnapshot overflow{};
  bool valid = true;
};

RunResult run(const std::size_t occurrences, const bool observe) {
  RunResult result;
  ObservationSink sink;
  Capture capture;
  qa::IdentitySource ids;
  HdMixer mixer;

  struct Bridge {
    ObservationSink *sink;
    qa::IdentitySource *ids;
    static void position(void *value,
                         const HdMixer::PositionSpan &span) noexcept {
      auto &bridge = *static_cast<Bridge *>(value);
      if (!qa::MixObservation::emit_participant(bridge.sink->observer(),
                                                *bridge.ids, span))
        bridge.sink->valid = false;
    }
  } bridge{&sink, &ids};
  if (observe)
    mixer.set_position_observer(&bridge, Bridge::position);

  const auto pcm =
      std::make_shared<const std::vector<std::int16_t>>(2U, std::int16_t{1});
  for (std::size_t index = 0; index < occurrences; ++index) {
    const auto occurrence = static_cast<std::uint64_t>(index + 1U);
    const HdMixer::VoiceIdentity identity{occurrence, 5U, occurrence};
    HdMixer::StartResult start;
    if (!mixer.start(1'000U + occurrence, pcm, 0U, 0U, 1.0F, false, false,
                     UINT64_MAX, UINT64_MAX, 0U, 0U, 0U, &identity, &start) ||
        start.action != HdMixer::StartAction::start || !start.voice_created ||
        start.new_occurrence != occurrence)
      result.valid = false;
  }
  result.starts = mixer.started();
  mixer.mix_into(result.output.data(), 1U, 0U);
  result.remaining_voices = mixer.voice_count();

  if (observe) {
    while (sink.queue->try_consume(&capture, Capture::consume)) {
    }
    result.observed = capture.count;
    result.overflow = sink.overflow.snapshot();
    result.valid = result.valid && sink.valid && capture.valid;
  }
  return result;
}

} // namespace

int main() try {
  const auto at_limit = run(occurrence_limit, true);
  const auto over_limit = run(occurrence_limit + 1U, true);
  const auto control = run(occurrence_limit + 1U, false);

  return at_limit.valid && at_limit.starts == occurrence_limit &&
                 at_limit.remaining_voices == 0U &&
                 at_limit.observed == occurrence_limit &&
                 at_limit.overflow.count == 0U &&
                 at_limit.output ==
                     std::array<std::int16_t, 2>{
                         static_cast<std::int16_t>(occurrence_limit),
                         static_cast<std::int16_t>(occurrence_limit)} &&
                 over_limit.valid &&
                 over_limit.starts == occurrence_limit + 1U &&
                 over_limit.remaining_voices == 0U &&
                 over_limit.observed == occurrence_limit &&
                 over_limit.overflow.count == 1U &&
                 over_limit.overflow.has_first &&
                 over_limit.overflow.first_stream ==
                     obs::ObservationStream::fact &&
                 over_limit.output == control.output &&
                 over_limit.starts == control.starts &&
                 over_limit.remaining_voices == control.remaining_voices
             ? 0
             : 1;
} catch (...) {
  return 2;
}
