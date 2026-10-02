#pragma once

#include <ayther/engine/music_sequence.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ayther::engine {

inline constexpr std::size_t max_music_voices_per_identity = 3;
inline constexpr std::size_t max_music_voices_per_bus = 6;
inline constexpr std::size_t max_total_audio_voices = 256;

struct MusicVoiceAdmission {
  bool accepted{};
  std::string diagnostic;
};

class MusicVoiceBudget {
public:
  [[nodiscard]] MusicVoiceAdmission add_music_voice(
      MusicIdentityId identity, AudioBusId bus,
      bool authorized_substitution) {
    const auto identity_count = static_cast<std::size_t>(std::ranges::count_if(
        music_voices_, [&](const auto &voice) { return voice.first == identity; }));
    const auto bus_count = static_cast<std::size_t>(std::ranges::count_if(
        music_voices_, [&](const auto &voice) { return voice.second == bus; }));
    if (!identity || !bus || identity_count >= max_music_voices_per_identity ||
        (authorized_substitution && bus_count >= max_music_voices_per_bus) ||
        total_voice_count() >= max_total_audio_voices)
      return {false, "transition_voice_limit"};
    music_voices_.emplace_back(identity, bus);
    return {true, {}};
  }

  [[nodiscard]] bool add_effect_voice() noexcept {
    if (total_voice_count() >= max_total_audio_voices)
      return false;
    ++effect_voices_;
    return true;
  }

  void begin_link() noexcept { active_link_ = true; }
  void set_pending() noexcept { pending_ = true; }
  void pause(bool paused) noexcept { paused_ = paused; }
  void cancel_transition() noexcept {
    active_link_ = false;
    pending_ = false;
  }

  [[nodiscard]] std::size_t music_voice_count() const noexcept {
    return music_voices_.size();
  }
  [[nodiscard]] std::size_t effect_voice_count() const noexcept {
    return effect_voices_;
  }
  [[nodiscard]] std::size_t total_voice_count() const noexcept {
    return music_voices_.size() + effect_voices_;
  }
  [[nodiscard]] std::size_t retained_transition_objects() const noexcept {
    return static_cast<std::size_t>(active_link_) +
           static_cast<std::size_t>(pending_);
  }

private:
  std::vector<std::pair<MusicIdentityId, AudioBusId>> music_voices_;
  std::size_t effect_voices_{};
  bool active_link_{};
  bool pending_{};
  bool paused_{};
};

} // namespace ayther::engine
