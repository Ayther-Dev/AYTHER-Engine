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
std::string_view text(const obs::FactView &fact,
                      std::string_view name) noexcept {
  for (const auto &item : fact.fields)
    if (item.name == name)
      if (const auto *value = std::get_if<std::string_view>(&item.value))
        return *value;
  return {};
}
struct Context {
  qa::IdentitySource ids;
  std::size_t natural = 0;
  std::size_t window = 0;
  bool valid = true;
  static void receive(void *value, const obs::FactView &fact) noexcept {
    auto &context = *static_cast<Context *>(value);
    if (fact.kind != "hd_voice_end")
      return;
    const auto reason = text(fact, "reason");
    if (reason == "natural_end") {
      ++context.natural;
      if (number(fact, "source_position") != 4 ||
          number(fact, "source_limit") != 4 ||
          number(fact, "output_position") != 6)
        context.valid = false;
    } else if (reason == "window_end") {
      ++context.window;
      if (number(fact, "source_position") != 7 ||
          number(fact, "source_limit") != 8 ||
          number(fact, "frame_position") != 2)
        context.valid = false;
    } else {
      context.valid = false;
    }
  }
  obs::Observer observer() noexcept { return {this, receive, nullptr}; }
  static void ended(void *value, const HdMixer::VoiceEnd &end) noexcept {
    auto &context = *static_cast<Context *>(value);
    context.valid = context.valid && qa::PositionObservation::emit_end(
                                         context.observer(), context.ids, end);
  }
};
} // namespace

int main() try {
  Context context;
  HdMixer mixer;
  mixer.set_end_observer(&context, Context::ended);
  const auto natural_pcm =
      std::make_shared<const std::vector<std::int16_t>>(8, 100);
  const HdMixer::VoiceIdentity natural_identity{3, 5, 8};
  if (!mixer.start(7, natural_pcm, 2, 0, 1.0F, false, false, UINT64_MAX,
                   UINT64_MAX, 0, 0, 0, &natural_identity))
    return 1;
  std::array<std::int16_t, 12> output{};
  mixer.mix_into(output.data(), output.size() / 2, 0);
  std::array<std::int16_t, 2> after{};
  mixer.mix_into(after.data(), after.size() / 2, 6);

  const auto window_pcm =
      std::make_shared<const std::vector<std::int16_t>>(16, 100);
  const HdMixer::VoiceIdentity window_identity{4, 5, 9};
  if (!mixer.start(9, window_pcm, 4, 7, 1.0F, true, false, 1, UINT64_MAX, 0, 0,
                   0, &window_identity))
    return 1;
  mixer.tick_frame(2);
  return context.valid && context.natural == 1 && context.window == 1 ? 0 : 1;
} catch (...) {
  return 2;
}
