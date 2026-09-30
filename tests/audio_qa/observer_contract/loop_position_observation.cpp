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
struct Context {
  qa::IdentitySource ids;
  std::size_t spans = 0;
  std::size_t crossings = 0;
  bool valid = true;
  static void receive(void *value, const obs::FactView &fact) noexcept {
    auto &context = *static_cast<Context *>(value);
    const auto *cause = fact.causes.size() == 1
                            ? std::get_if<obs::FactId>(&fact.causes[0])
                            : nullptr;
    if (!cause || *cause != obs::FactId{5, 8}) {
      context.valid = false;
      return;
    }
    if (fact.kind == "hd_voice_position_span") {
      const std::array<std::uint64_t, 2> output_begin{0, 1};
      const std::array<std::uint64_t, 2> output_end{1, 3};
      const std::array<std::uint64_t, 2> source_begin{5, 2};
      const std::array<std::uint64_t, 2> source_end{6, 4};
      const auto index = context.spans++;
      if (index >= output_begin.size() ||
          number(fact, "output_begin") != output_begin[index] ||
          number(fact, "output_end") != output_end[index] ||
          number(fact, "source_begin") != source_begin[index] ||
          number(fact, "source_end") != source_end[index] ||
          number(fact, "source_limit") != 12)
        context.valid = false;
    } else if (fact.kind == "hd_voice_loop_crossing") {
      ++context.crossings;
      if (number(fact, "output_position") != 1 ||
          number(fact, "source_before") != 6 ||
          number(fact, "source_after") != 2 ||
          number(fact, "loop_begin") != 2 || number(fact, "loop_end") != 6 ||
          number(fact, "source_limit") != 12)
        context.valid = false;
    }
  }
  obs::Observer observer() noexcept { return {this, receive, nullptr}; }
  static void position(void *value,
                       const HdMixer::PositionSpan &span) noexcept {
    auto &context = *static_cast<Context *>(value);
    context.valid = context.valid && qa::PositionObservation::emit(
                                         context.observer(), context.ids, span);
  }
  static void loop(void *value,
                   const HdMixer::LoopCrossing &crossing) noexcept {
    auto &context = *static_cast<Context *>(value);
    context.valid = context.valid &&
                    qa::PositionObservation::emit_loop(context.observer(),
                                                       context.ids, crossing);
  }
};
} // namespace

int main() try {
  Context context;
  HdMixer mixer;
  mixer.set_position_observer(&context, Context::position);
  mixer.set_loop_observer(&context, Context::loop);
  const auto pcm = std::make_shared<const std::vector<std::int16_t>>(24, 100);
  const HdMixer::VoiceIdentity identity{3, 5, 8};
  if (!mixer.start(7, pcm, 0, 5, 1.0F, true, false, UINT64_MAX, UINT64_MAX, 0,
                   2, 6, &identity))
    return 1;
  std::array<std::int16_t, 6> output{};
  mixer.mix_into(output.data(), output.size() / 2, 0);
  return context.valid && context.spans == 2 && context.crossings == 1 ? 0 : 1;
} catch (...) {
  return 2;
}
