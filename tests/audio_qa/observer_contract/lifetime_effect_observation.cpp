#include "audio_position_observation.h"

#include <array>
#include <memory>
#include <string_view>
#include <vector>

namespace qa = ayther::audio_qa;
namespace obs = ayther::engine::audio_observation;
namespace {
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
  std::array<std::size_t, 3> tail{};
  std::array<std::size_t, 3> fade{};
  std::size_t tail_ends = 0;
  std::size_t fade_ends = 0;
  bool valid = true;
  static std::size_t stage_index(std::string_view stage) noexcept {
    if (stage == "begin")
      return 0;
    if (stage == "advance")
      return 1;
    return stage == "end" ? 2 : 3;
  }
  static void receive(void *value, const obs::FactView &fact) noexcept {
    auto &context = *static_cast<Context *>(value);
    if (fact.kind == "hd_voice_lifetime_effect") {
      const auto index = stage_index(text(fact, "stage"));
      if (index >= 3) {
        context.valid = false;
        return;
      }
      if (text(fact, "effect") == "tail" &&
          text(fact, "reason") == "window_tail")
        ++context.tail[index];
      else if (text(fact, "effect") == "fade" &&
               text(fact, "reason") == "authored_fade")
        ++context.fade[index];
      else
        context.valid = false;
    } else if (fact.kind == "hd_voice_end") {
      if (text(fact, "reason") == "tail_complete")
        ++context.tail_ends;
      else if (text(fact, "reason") == "fade_complete")
        ++context.fade_ends;
      else
        context.valid = false;
    }
  }
  obs::Observer observer() noexcept { return {this, receive, nullptr}; }
  static void effect(void *value,
                     const HdMixer::LifetimeEffect &effect) noexcept {
    auto &context = *static_cast<Context *>(value);
    context.valid = context.valid &&
                    qa::PositionObservation::emit_effect(context.observer(),
                                                         context.ids, effect);
  }
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
  mixer.set_effect_observer(&context, Context::effect);
  mixer.set_end_observer(&context, Context::ended);
  const auto pcm = std::make_shared<const std::vector<std::int16_t>>(16, 100);
  const HdMixer::VoiceIdentity tail_identity{3, 5, 8};
  if (!mixer.start(7, pcm, 0, 5, 1.0F, true, false, 0, 5, 0, 2, 6,
                   &tail_identity))
    return 1;
  mixer.tick_frame(1);
  std::array<std::int16_t, 6> tail_output{};
  mixer.mix_into(tail_output.data(), tail_output.size() / 2, 0);

  const HdMixer::VoiceIdentity fade_identity{4, 5, 9};
  if (!mixer.start(9, pcm, 3, 0, 1.0F, true, false, 0, UINT64_MAX, 2, 0, 0,
                   &fade_identity))
    return 1;
  mixer.tick_frame(1);
  std::array<std::int16_t, 4> fade_output{};
  mixer.mix_into(fade_output.data(), fade_output.size() / 2, 3);

  const std::array<std::size_t, 3> expected{1, 1, 1};
  return context.valid && context.tail == expected &&
                 context.fade == expected && context.tail_ends == 1 &&
                 context.fade_ends == 1
             ? 0
             : 1;
} catch (...) {
  return 2;
}
