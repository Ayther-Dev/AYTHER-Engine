#pragma once

#include <cstdint>
#include <string>

namespace ayther::engine {

enum class PauseSignalSource : std::uint8_t {
  none,
  core,
  authored_rule,
  silence,
  button,
};

struct GameMusicPauseEvidence {
  PauseSignalSource source{PauseSignalSource::none};
  std::string revision;
  bool positive_control{};
  bool negative_control{};
  bool music_paused{};
};

struct GameMusicPauseDecision {
  bool verified{};
  bool music_frozen{};
  bool music_advances{true};
  std::string revision;
  std::string diagnostic;
  bool limitation_visible{};
};

[[nodiscard]] inline GameMusicPauseDecision
evaluate_game_music_pause(const GameMusicPauseEvidence &evidence) {
  const bool trusted_source = evidence.source == PauseSignalSource::core ||
                              evidence.source == PauseSignalSource::authored_rule;
  const bool verified = trusted_source && !evidence.revision.empty() &&
                        evidence.positive_control && evidence.negative_control;
  if (!verified)
    return {false, false, true, evidence.revision,
            "game_pause_unobservable", true};
  return {true, evidence.music_paused, !evidence.music_paused,
          evidence.revision, {}, false};
}

} // namespace ayther::engine
