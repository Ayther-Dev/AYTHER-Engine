#include "audio_playback_observation.h"

#include <ayther/audio_hd_mixer.h>

#include <array>
#include <memory>
#include <string_view>
#include <vector>

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
struct Sink {
  std::size_t decisions = 0;
  std::size_t effects = 0;
  bool valid = true;
  static void receive(void *context, const obs::FactView &fact) noexcept {
    auto &sink = *static_cast<Sink *>(context);
    if (fact.kind == "hd_playback_effect") {
      ++sink.effects;
      const auto *result = field<std::string_view>(fact, "result");
      const auto *played = field<bool>(fact, "played");
      if (!result || *result != "started" || !played || !*played)
        sink.valid = false;
      return;
    }
    if (fact.kind != "hd_playback_decision")
      return;
    ++sink.decisions;
    const auto *action = field<std::string_view>(fact, "action");
    const auto *reason = field<std::string_view>(fact, "reason");
    const auto *current = field<obs::OccurrenceId>(fact, "occurrence");
    const auto *previous =
        field<obs::OccurrenceId>(fact, "previous_occurrence");
    const auto *previous_request = field<obs::FactId>(fact, "previous_request");
    const auto *previous_cause = fact.causes.size() == 2
                                     ? std::get_if<obs::FactId>(&fact.causes[1])
                                     : nullptr;
    if (!action || !reason || !current || !previous || !previous_request ||
        !previous_cause || *previous_cause != *previous_request ||
        current->value == previous->value)
      sink.valid = false;
    if (sink.decisions == 1 &&
        (*action != "restart" || *reason != "same_key_same_asset" ||
         previous->value != 11 || current->value != 12))
      sink.valid = false;
    if (sink.decisions == 2 &&
        (*action != "replace" || *reason != "same_key_new_asset" ||
         previous->value != 12 || current->value != 13))
      sink.valid = false;
  }
  obs::Observer observer() noexcept { return {this, receive, nullptr}; }
};
} // namespace

int main() try {
  const auto first = std::make_shared<const std::vector<std::int16_t>>(32, 100);
  const auto second =
      std::make_shared<const std::vector<std::int16_t>>(32, 200);
  HdMixer mixer;
  HdMixer::StartResult initial, restart, replace;
  const HdMixer::VoiceIdentity initial_identity{11, 5, 1};
  const HdMixer::VoiceIdentity restart_identity{12, 5, 2};
  const HdMixer::VoiceIdentity replace_identity{13, 5, 3};
  if (!mixer.start(7, first, 0, 0, 1.0F, false, false, UINT64_MAX, UINT64_MAX,
                   0, 0, 0, &initial_identity, &initial) ||
      !mixer.start(7, first, 1, 0, 1.0F, false, false, UINT64_MAX, UINT64_MAX,
                   0, 0, 0, &restart_identity, &restart) ||
      !mixer.start(7, second, 2, 0, 1.0F, false, false, UINT64_MAX, UINT64_MAX,
                   0, 0, 0, &replace_identity, &replace))
    return 1;
  if (initial.action != HdMixer::StartAction::start ||
      restart.action != HdMixer::StartAction::restart ||
      restart.previous_occurrence != 11 ||
      restart.previous_cause_producer != 5 ||
      restart.previous_cause_sequence != 1 ||
      replace.action != HdMixer::StartAction::replace ||
      replace.previous_occurrence != 12 ||
      replace.previous_cause_producer != 5 ||
      replace.previous_cause_sequence != 2 || !initial.voice_created ||
      !restart.voice_created || !replace.voice_created)
    return 1;

  Sink sink;
  qa::IdentitySource ids;
  qa::PlaybackObservation restarted(sink.observer(), ids,
                                    {2, 7, 7, obs::FactId{5, 90}, {12}});
  restarted.decision("restart", "same_key_same_asset", obs::OccurrenceId{11},
                     obs::FactId{5, 1});
  restarted.effect("started", true);
  qa::PlaybackObservation replaced(sink.observer(), ids,
                                   {3, 7, 7, obs::FactId{5, 91}, {13}});
  replaced.decision("replace", "same_key_new_asset", obs::OccurrenceId{12},
                    obs::FactId{5, 2});
  replaced.effect("started", true);
  return sink.valid && sink.decisions == 2 && sink.effects == 2 &&
                 restarted.complete() && replaced.complete()
             ? 0
             : 1;
} catch (...) {
  return 2;
}
