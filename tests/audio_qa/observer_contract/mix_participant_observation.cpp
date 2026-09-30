#include "audio_mix_observation.h"

#include <algorithm>
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
std::uint64_t number(const obs::FactView &fact,
                     std::string_view name) noexcept {
  const auto *value = field<std::uint64_t>(fact, name);
  return value ? *value : UINT64_MAX;
}
std::string_view text(const obs::FactView &fact,
                      std::string_view name) noexcept {
  const auto *value = field<std::string_view>(fact, name);
  return value ? *value : std::string_view{};
}
struct ExpectedParticipant {
  std::uint64_t occurrence;
  std::uint64_t key;
  obs::FactId cause;
  std::uint64_t mix_begin;
  std::uint64_t mix_end;
  std::uint64_t track_begin;
  std::uint64_t track_end;
  double gain;
  bool muted;
  bool nonzero;
  bool found = false;
};
struct Context {
  qa::IdentitySource ids;
  std::array<ExpectedParticipant, 2> expected{{
      {101, 10, {5, 11}, 0, 10, 4, 14, 1.0, false, true, false},
      {102, 20, {5, 12}, 3, 10, 8, 15, 0.0, true, false, false},
  }};
  std::size_t count = 0;
  bool complete = true;
  bool valid = true;
  static void receive(void *value, const obs::FactView &fact) noexcept {
    auto &context = *static_cast<Context *>(value);
    if (fact.kind != "hd_mix_participant") {
      context.valid = false;
      return;
    }
    ++context.count;
    const auto *occurrence = field<obs::OccurrenceId>(fact, "occurrence");
    if (!occurrence) {
      context.valid = false;
      return;
    }
    const auto expected =
        std::find_if(context.expected.begin(), context.expected.end(),
                     [occurrence](const ExpectedParticipant &item) {
                       return item.occurrence == occurrence->value;
                     });
    const auto *cause = fact.causes.size() == 1
                            ? std::get_if<obs::FactId>(&fact.causes[0])
                            : nullptr;
    const auto *complete = field<bool>(fact, "links_complete");
    const auto *gain_begin = field<double>(fact, "effective_gain_begin");
    const auto *gain_end = field<double>(fact, "effective_gain_end");
    const auto *muted = field<bool>(fact, "muted_by_gain");
    const auto *nonzero = field<bool>(fact, "nonzero_contribution");
    if (expected == context.expected.end() || expected->found || !cause ||
        *cause != expected->cause || number(fact, "key") != expected->key ||
        number(fact, "mix_begin") != expected->mix_begin ||
        number(fact, "mix_end") != expected->mix_end ||
        number(fact, "track_begin") != expected->track_begin ||
        number(fact, "track_end") != expected->track_end ||
        number(fact, "track_limit") != 32 ||
        number(fact, "mix_sample_rate") != 44100 ||
        number(fact, "track_sample_rate") != 44100 ||
        text(fact, "mix_timeline") != "engine_main_mix" ||
        text(fact, "track_timeline") != "hd_asset_pcm" || !complete ||
        !*complete || !gain_begin || *gain_begin != expected->gain ||
        !gain_end || *gain_end != expected->gain || !muted ||
        *muted != expected->muted || !nonzero ||
        *nonzero != expected->nonzero) {
      context.valid = false;
      return;
    }
    expected->found = true;
  }
  obs::Observer observer() noexcept { return {this, receive, nullptr}; }
  static void position(void *value,
                       const HdMixer::PositionSpan &span) noexcept {
    auto &context = *static_cast<Context *>(value);
    const bool emitted = qa::MixObservation::emit_participant(
        context.observer(), context.ids, span);
    context.complete = context.complete && emitted;
  }
};
} // namespace

int main() try {
  Context context;
  HdMixer mixer;
  mixer.set_position_observer(&context, Context::position);
  const auto pcm = std::make_shared<const std::vector<std::int16_t>>(64, 100);
  const HdMixer::VoiceIdentity first{101, 5, 11};
  const HdMixer::VoiceIdentity second{102, 5, 12};
  if (!mixer.start(10, pcm, 0, 4, 1.0F, false, false, UINT64_MAX, UINT64_MAX, 0,
                   0, 0, &first) ||
      !mixer.start(20, pcm, 3, 8, 0.0F, false, false, UINT64_MAX, UINT64_MAX, 0,
                   0, 0, &second))
    return 1;
  std::array<std::int16_t, 20> output{};
  mixer.mix_into(output.data(), output.size() / 2, 0);
  return context.complete && context.valid && context.count == 2 &&
                 mixer.voice_count() == 2 &&
                 std::ranges::all_of(
                     output, [](std::int16_t value) { return value == 100; }) &&
                 std::ranges::all_of(context.expected,
                                     &ExpectedParticipant::found)
             ? 0
             : 1;
} catch (...) {
  return 2;
}
