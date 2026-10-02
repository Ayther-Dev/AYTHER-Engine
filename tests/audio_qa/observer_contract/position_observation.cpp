#include "audio_position_observation.h"

#include <array>
#include <memory>
#include <string_view>
#include <vector>

namespace qa = ayther::audio_qa;
namespace obs = ayther::engine::audio_observation;
namespace {
std::uint64_t number(const obs::FactView &fact,
                     std::string_view name) noexcept {
  for (const auto &item : fact.fields)
    if (item.name == name)
      if (const auto *value = std::get_if<std::uint64_t>(&item.value))
        return *value;
  return UINT64_MAX;
}
struct Sink {
  std::size_t count = 0;
  bool valid = true;
  static void receive(void *context, const obs::FactView &fact) noexcept {
    auto &sink = *static_cast<Sink *>(context);
    if (fact.kind != "hd_voice_position_span")
      return;
    const std::array<std::uint64_t, 2> output_begin{2, 10};
    const std::array<std::uint64_t, 2> output_end{10, 18};
    const std::array<std::uint64_t, 2> source_begin{0, 8};
    const std::array<std::uint64_t, 2> source_end{8, 16};
    if (sink.count >= output_begin.size()) {
      sink.valid = false;
      return;
    }
    const auto index = sink.count++;
    const auto *cause = fact.causes.size() == 1
                            ? std::get_if<obs::FactId>(&fact.causes[0])
                            : nullptr;
    if (!cause || *cause != obs::FactId{5, 8} ||
        number(fact, "output_begin") != output_begin[index] ||
        number(fact, "output_end") != output_end[index] ||
        number(fact, "source_begin") != source_begin[index] ||
        number(fact, "source_end") != source_end[index] ||
        number(fact, "source_limit") != 16 ||
        number(fact, "sample_rate") != 44100)
      sink.valid = false;
  }
  obs::Observer observer() noexcept { return {this, receive, nullptr}; }
};
struct Context {
  Sink sink;
  qa::IdentitySource ids;
  bool complete = true;
  static void position(void *value,
                       const HdMixer::PositionSpan &span) noexcept {
    auto &context = *static_cast<Context *>(value);
    context.complete = context.complete &&
                       qa::PositionObservation::emit(context.sink.observer(),
                                                     context.ids, span);
  }
};
} // namespace

int main() try {
  Context context;
  HdMixer mixer;
  mixer.set_position_observer(&context, Context::position);
  const auto pcm = std::make_shared<const std::vector<std::int16_t>>(32, 100);
  const HdMixer::VoiceIdentity identity{3, 5, 8};
  if (!mixer.start(7, pcm, 2, 0, 1.0F, false, false, UINT64_MAX, UINT64_MAX, 0,
                   0, 0, &identity))
    return 1;
  std::array<std::int16_t, 20> first{};
  std::array<std::int16_t, 16> second{};
  mixer.mix_into(first.data(), first.size() / 2, 0);
  mixer.mix_into(second.data(), second.size() / 2, 10);
  return context.complete && context.sink.valid && context.sink.count == 2 ? 0
                                                                           : 1;
} catch (...) {
  return 2;
}
