#pragma once

#include <ayther/engine/music_transition_queue.hpp>

#include <cstdint>

namespace ayther::engine {

enum class EffectPausePolicy : std::uint8_t {
  follow_game_music_pause,
  continue_during_game_pause,
};

class MusicPauseState {
public:
  void set_host_pause(bool paused) noexcept { host_pause_ = paused; }
  void set_game_music_pause(bool paused) noexcept {
    game_music_pause_ = paused;
  }

  void advance_music(std::uint64_t frames) noexcept {
    if (paused())
      return;
    music_cursor_ += frames;
    envelope_cursor_ += frames;
    candidate_music_time_ += frames;
  }

  void return_loop() noexcept {
    if (!paused())
      ++loop_iterations_;
  }

  void advance_effect(std::uint64_t frames, EffectPausePolicy policy) noexcept {
    if (host_pause_ || (game_music_pause_ &&
                        policy == EffectPausePolicy::follow_game_music_pause))
      return;
    effect_cursor_ += frames;
  }

  void cancel(MusicTransitionQueue &queue) const noexcept { queue.cancel(); }

  [[nodiscard]] bool paused() const noexcept {
    return host_pause_ || game_music_pause_;
  }
  [[nodiscard]] std::uint64_t music_cursor() const noexcept {
    return music_cursor_;
  }
  [[nodiscard]] std::uint64_t envelope_cursor() const noexcept {
    return envelope_cursor_;
  }
  [[nodiscard]] std::uint64_t candidate_music_time() const noexcept {
    return candidate_music_time_;
  }
  [[nodiscard]] std::uint64_t loop_iterations() const noexcept {
    return loop_iterations_;
  }
  [[nodiscard]] std::uint64_t effect_cursor() const noexcept {
    return effect_cursor_;
  }

private:
  bool host_pause_{};
  bool game_music_pause_{};
  std::uint64_t music_cursor_{};
  std::uint64_t envelope_cursor_{};
  std::uint64_t candidate_music_time_{};
  std::uint64_t loop_iterations_{};
  std::uint64_t effect_cursor_{};
};

} // namespace ayther::engine
