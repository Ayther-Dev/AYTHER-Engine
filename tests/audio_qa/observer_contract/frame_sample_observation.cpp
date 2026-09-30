#include "audio_frame_observation.h"
#include "audio_original_observation.h"

#include <array>
#include <optional>
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

struct Context {
  qa::IdentitySource ids;
  std::optional<obs::FactId> boundary;
  std::size_t fact_count = 0;
  bool complete = true;
  bool valid = true;

  obs::Observer observer() noexcept { return {this, receive, nullptr}; }

  static void receive(void *value, const obs::FactView &fact) noexcept {
    auto &context = *static_cast<Context *>(value);
    for (const auto &item : fact.fields)
      if (item.name.find("second") != std::string_view::npos)
        context.valid = false;

    if (context.fact_count == 0) {
      if (fact.kind != "audio_frame_sample_boundary" ||
          fact.frame.availability != obs::Availability::known ||
          fact.frame.emulation_frame != 77 || !fact.causes.empty() ||
          number(fact, "output_position") != 0 ||
          number(fact, "staged_offset") != 0 ||
          number(fact, "sample_rate") != 44100)
        context.valid = false;
    } else {
      const auto span = context.fact_count - 1;
      const auto *state = field<std::string_view>(fact, "state");
      const auto *cause = fact.causes.size() == 1
                              ? std::get_if<obs::FactId>(&fact.causes.front())
                              : nullptr;
      if (fact.kind != "original_audio_span" ||
          fact.frame.availability != obs::Availability::known ||
          fact.frame.emulation_frame != 77 || !state ||
          *state != (span == 0 ? "present" : "suppressed") ||
          number(fact, "mix_begin") != span * 2 ||
          number(fact, "mix_end") != (span + 1) * 2 ||
          number(fact, "frame_boundary") != 0 || !cause || !context.boundary ||
          *cause != *context.boundary)
        context.valid = false;
    }
    ++context.fact_count;
  }

  static void frame(void *value,
                    const AudioPlayer::FrameSampleBoundary &boundary) noexcept {
    auto &context = *static_cast<Context *>(value);
    const auto result = qa::FrameSampleObservation::emit(context.observer(),
                                                         context.ids, boundary);
    context.boundary = result.id;
    context.complete = context.complete && result.complete;
  }

  static void original(void *value,
                       const AudioPlayer::OriginalAudioSpan &span) noexcept {
    auto &context = *static_cast<Context *>(value);
    context.complete = context.complete && qa::OriginalAudioObservation::emit(
                                               context.observer(), context.ids,
                                               span, context.boundary);
  }
};

} // namespace

int main() try {
  Context context;
  AudioPlayer player;
  if (!player.init(ayther::RuntimeOptions{}))
    return 2;
  player.set_drc_enabled(false);
  player.set_frame_sample_observer(&context, Context::frame);
  player.set_original_audio_observer(&context, Context::original);

  constexpr std::array<std::int16_t, 4> first{100, -100, 200, -200};
  constexpr std::array<std::int16_t, 4> second{300, -300, 400, -400};
  player.mark_frame_boundary(77);
  player.buffer_emulator(11, first.data(), first.size() / 2);
  player.buffer_emulator(12, second.data(), second.size() / 2);
  constexpr std::uint64_t muted_hash = 12;
  player.set_user_mute_hashes(&muted_hash, 1);
  player.flush_emulator();

  return context.complete && context.valid && context.fact_count == 3 ? 0 : 1;
} catch (...) {
  return 3;
}
