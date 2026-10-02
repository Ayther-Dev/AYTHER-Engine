#include "audio_playback_observation.h"

#include <array>
#include <iostream>

namespace qa = ayther::audio_qa;
namespace obs = ayther::engine::audio_observation;
namespace {
struct Sink {
  std::array<obs::FactId, 6> ids{};
  std::array<std::string_view, 6> kinds{};
  std::array<obs::OccurrenceId, 6> occurrences{};
  std::array<std::string_view, 6> actions{};
  std::array<std::string_view, 6> reasons{};
  std::array<std::string_view, 6> results{};
  std::array<obs::Availability, 6> played_availability{};
  std::array<bool, 6> played{};
  std::size_t count = 0;
  bool valid = true;
  static void receive(void *context, const obs::FactView &fact) noexcept {
    auto &sink = *static_cast<Sink *>(context);
    if (sink.count >= sink.ids.size()) {
      sink.valid = false;
      return;
    }
    const auto index = sink.count++;
    sink.ids[index] = fact.id;
    sink.kinds[index] = fact.kind;
    if (fact.id != obs::FactId{5, index + 1} ||
        fact.frame.emulation_frame != 12)
      sink.valid = false;
    if (index % 3 == 0) {
      const auto selection = obs::FactId{5, index == 0 ? 8U : 9U};
      const auto *cause = fact.causes.size() == 1
                              ? std::get_if<obs::FactId>(&fact.causes[0])
                              : nullptr;
      if (!cause || *cause != selection)
        sink.valid = false;
    } else {
      const auto *cause = fact.causes.size() == 1
                              ? std::get_if<obs::FactId>(&fact.causes[0])
                              : nullptr;
      if (!cause || *cause != sink.ids[index - 1])
        sink.valid = false;
    }
    for (const auto &field : fact.fields)
      if (field.name == "occurrence")
        if (const auto *value = std::get_if<obs::OccurrenceId>(&field.value))
          sink.occurrences[index] = *value;
    for (const auto &field : fact.fields) {
      if (const auto *value = std::get_if<std::string_view>(&field.value)) {
        if (field.name == "action")
          sink.actions[index] = *value;
        if (field.name == "reason")
          sink.reasons[index] = *value;
        if (field.name == "result")
          sink.results[index] = *value;
      }
      if (field.name == "played") {
        sink.played_availability[index] = field.availability;
        if (const auto *value = std::get_if<bool>(&field.value))
          sink.played[index] = *value;
      }
    }
  }
  obs::Observer observer() noexcept { return {this, receive, nullptr}; }
};
} // namespace

int main() try {
  Sink sink;
  qa::IdentitySource ids;
  qa::PlaybackObservation start(sink.observer(), ids,
                                {12, 100, 10, obs::FactId{5, 8}, {3}});
  start.decision("start", "rising_edge");
  start.effect("started", true);
  qa::PlaybackObservation maintain(sink.observer(), ids,
                                   {12, 100, 10, obs::FactId{5, 9}, {3}});
  maintain.decision("maintain", "active_without_rising_edge");
  maintain.effect("retained", std::nullopt);
  if (!start.complete() || !maintain.complete() || !sink.valid ||
      sink.count != 6)
    return 1;
  constexpr std::array<std::string_view, 3> expected{
      "hd_playback_request", "hd_playback_decision", "hd_playback_effect"};
  for (std::size_t i = 0; i < sink.count; ++i)
    if (sink.kinds[i] != expected[i % expected.size()] ||
        sink.occurrences[i].value != 3)
      return 1;
  if (sink.actions[1] != "start" || sink.reasons[1] != "rising_edge" ||
      sink.results[2] != "started" ||
      sink.played_availability[2] != obs::Availability::known ||
      !sink.played[2] || sink.actions[4] != "maintain" ||
      sink.reasons[4] != "active_without_rising_edge" ||
      sink.results[5] != "retained" ||
      sink.played_availability[5] != obs::Availability::not_applicable)
    return 1;

  qa::PlaybackObservation incomplete(sink.observer(), ids, {12, 1, 1, {}, {}});
  return incomplete.complete() ? 1 : 0;
} catch (...) {
  return 2;
}
