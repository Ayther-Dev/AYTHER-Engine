#include "audio_original_observation.h"

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
struct Context {
  qa::IdentitySource ids;
  std::array<std::string_view, 3> states{"present", "suppressed", "absent"};
  std::array<std::string_view, 3> reasons{"unmuted", "matched_hash",
                                          "no_native_batch"};
  std::size_t count = 0;
  bool complete = true;
  bool valid = true;
  static void receive(void *value, const obs::FactView &fact) noexcept {
    auto &context = *static_cast<Context *>(value);
    if (fact.kind != "original_audio_span" ||
        context.count >= context.states.size()) {
      context.valid = false;
      return;
    }
    const auto index = context.count++;
    const auto begin = number(fact, "mix_begin");
    const auto end = number(fact, "mix_end");
    const auto *observed_complete = field<bool>(fact, "observation_complete");
    const auto *valid = field<bool>(fact, "valid");
    if (!fact.causes.empty() || begin != index * 4 || end != (index + 1) * 4 ||
        text(fact, "state") != context.states[index] ||
        text(fact, "reason") != context.reasons[index] ||
        number(fact, "sample_rate") != 44100 || !observed_complete ||
        !*observed_complete || !valid || !*valid)
      context.valid = false;
  }
  obs::Observer observer() noexcept { return {this, receive, nullptr}; }
  static void original(void *value,
                       const AudioPlayer::OriginalAudioSpan &span) noexcept {
    auto &context = *static_cast<Context *>(value);
    const bool emitted = qa::OriginalAudioObservation::emit(context.observer(),
                                                            context.ids, span);
    context.complete = context.complete && emitted;
  }
};
struct LimitContext {
  qa::IdentitySource ids;
  std::size_t count = 0;
  bool valid = true;
  static void receive(void *value, const obs::FactView &fact) noexcept {
    auto &context = *static_cast<LimitContext *>(value);
    const auto index = context.count++;
    const auto *complete = field<bool>(fact, "observation_complete");
    const auto *row_valid = field<bool>(fact, "valid");
    if (fact.kind != "original_audio_span" || !complete || *complete ||
        !row_valid || *row_valid) {
      context.valid = false;
      return;
    }
    if (index < 256) {
      if (text(fact, "state") != "present" ||
          text(fact, "reason") != "unmuted" ||
          number(fact, "mix_begin") != index ||
          number(fact, "mix_end") != index + 1)
        context.valid = false;
    } else if (index != 256 || text(fact, "state") != "unknown" ||
               text(fact, "reason") != "capacity_exceeded" ||
               number(fact, "mix_begin") != 0 ||
               number(fact, "mix_end") != 257) {
      context.valid = false;
    }
  }
  obs::Observer observer() noexcept { return {this, receive, nullptr}; }
  static void original(void *value,
                       const AudioPlayer::OriginalAudioSpan &span) noexcept {
    auto &context = *static_cast<LimitContext *>(value);
    if (qa::OriginalAudioObservation::emit(context.observer(), context.ids,
                                           span))
      context.valid = false;
  }
};
bool basic_states() {
  Context context;
  AudioPlayer player;
  if (!player.init(ayther::RuntimeOptions{}))
    return false;
  player.set_drc_enabled(false);
  player.set_original_audio_observer(&context, Context::original);
  constexpr std::array<std::int16_t, 8> native{100, -100, 200, -200,
                                               300, -300, 400, -400};
  player.buffer_emulator(11, native.data(), native.size() / 2);
  player.flush_emulator();
  constexpr std::uint64_t muted_hash = 12;
  player.set_user_mute_hashes(&muted_hash, 1);
  player.buffer_emulator(muted_hash, native.data(), native.size() / 2);
  player.flush_emulator();
  player.set_user_mute_hashes(nullptr, 0);
  constexpr std::array<float, 8> routed{0.1F, -0.1F, 0.2F, -0.2F,
                                        0.3F, -0.3F, 0.4F, -0.4F};
  player.buffer_router(routed.data(), routed.size() / 2, false);
  player.flush_emulator();
  return context.complete && context.valid && context.count == 3;
}
bool bounded_loss() {
  LimitContext context;
  AudioPlayer player;
  if (!player.init(ayther::RuntimeOptions{}))
    return false;
  player.set_drc_enabled(false);
  player.set_original_audio_observer(&context, LimitContext::original);
  constexpr std::array<std::int16_t, 2> frame{100, -100};
  for (std::uint64_t hash = 1; hash <= 257; ++hash)
    player.buffer_emulator(hash, frame.data(), 1);
  player.flush_emulator();
  return context.valid && context.count == 257;
}
} // namespace

int main() try {
  return basic_states() && bounded_loss() ? 0 : 1;
} catch (...) {
  return 2;
}
