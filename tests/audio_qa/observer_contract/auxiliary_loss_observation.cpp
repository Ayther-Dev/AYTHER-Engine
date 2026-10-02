#include "audio_auxiliary_observation.h"

#include <array>
#include <string_view>

namespace qa = ayther::audio_qa;
namespace obs = ayther::engine::audio_observation;

namespace {

template <class T>
const T *field(const obs::FactView &fact, std::string_view name) noexcept {
  for (const auto &item : fact.fields)
    if (item.name == name)
      return std::get_if<T>(&item.value);
  return nullptr;
}

struct Capture {
  struct Row {
    obs::FactId id{};
    std::string_view kind;
    obs::Availability availability = obs::Availability::unknown;
    std::string_view frame_reason;
    std::size_t cause_count = 0;
    std::string_view timeline;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::string_view reason;
  };
  std::array<Row, 2> facts{};
  std::size_t count = 0;
  bool valid = true;
  bool pcm_received = false;

  obs::Observer observer() noexcept {
    return {this, receive_fact, receive_pcm};
  }

  static void receive_fact(void *value, const obs::FactView &fact) noexcept {
    auto &capture = *static_cast<Capture *>(value);
    if (capture.count == capture.facts.size()) {
      capture.valid = false;
      return;
    }
    const auto *timeline = field<std::string_view>(fact, "input_timeline");
    const auto *begin = field<std::uint64_t>(fact, "input_begin");
    const auto *end = field<std::uint64_t>(fact, "input_end");
    const auto *reason = field<std::string_view>(fact, "reason");
    if (!timeline || !begin || !end || !reason) {
      capture.valid = false;
      return;
    }
    capture.facts[capture.count++] = {fact.id,
                                      fact.kind,
                                      fact.frame.availability,
                                      fact.frame.unavailable_reason,
                                      fact.causes.size(),
                                      *timeline,
                                      *begin,
                                      *end,
                                      *reason};
  }

  static void receive_pcm(void *value, const obs::PcmView &) noexcept {
    static_cast<Capture *>(value)->pcm_received = true;
  }
};

bool matches(const Capture::Row &fact, std::uint64_t sequence,
             std::uint64_t begin, std::uint64_t end,
             std::string_view reason) noexcept {
  return fact.id.producer ==
             static_cast<std::uint32_t>(qa::Producer::auxiliary_output) &&
         fact.id.sequence == sequence &&
         fact.kind == "auxiliary_delivery_loss" &&
         fact.availability == obs::Availability::known &&
         fact.frame_reason == reason && fact.cause_count == 0 &&
         fact.timeline == "synth_input" && fact.begin == begin &&
         fact.end == end && fact.reason == reason;
}

} // namespace

int main() {
  Capture capture;
  qa::IdentitySource ids;
  const AudioPlayer::AuxiliaryLoss discarded{
      32, 48, AudioPlayer::AuxiliaryLossReason::discarded};
  const AudioPlayer::AuxiliaryLoss delivery_failed{
      48, 64, AudioPlayer::AuxiliaryLossReason::delivery_failed};

  const bool emitted_discard =
      qa::AuxiliaryObservation::emit_loss(capture.observer(), ids, discarded);
  const bool emitted_failure = qa::AuxiliaryObservation::emit_loss(
      capture.observer(), ids, delivery_failed);
  return emitted_discard && emitted_failure && capture.valid &&
                 !capture.pcm_received && capture.count == 2 &&
                 matches(capture.facts[0], 1, 32, 48, "discarded") &&
                 matches(capture.facts[1], 2, 48, 64, "delivery_failed")
             ? 0
             : 1;
}
