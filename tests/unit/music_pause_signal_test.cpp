#include <ayther/engine/music_pause_signal.hpp>

#include <cstdio>

namespace {
void check(bool value, const char *message, int &failures) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}
} // namespace

int main() {
  using namespace ayther::engine;
  int failures = 0;
  auto result = evaluate_game_music_pause(
      {PauseSignalSource::core, "core-r4", true, true, true});
  check(result.verified && result.music_frozen && !result.music_advances &&
            result.revision == "core-r4",
        "verified core pause freezes musical time", failures);
  result = evaluate_game_music_pause(
      {PauseSignalSource::authored_rule, "rule-r2", true, true, false});
  check(result.verified && !result.music_frozen && result.music_advances,
        "verified signal can declare music continues during game pause",
        failures);

  result = evaluate_game_music_pause(
      {PauseSignalSource::none, {}, false, false, true});
  check(!result.verified && result.music_advances &&
            result.diagnostic == "game_pause_unobservable" &&
            result.limitation_visible,
        "missing signal preserves advance and exposes limitation", failures);
  result = evaluate_game_music_pause(
      {PauseSignalSource::silence, "heuristic", true, true, true});
  check(!result.verified && result.music_advances &&
            result.diagnostic == "game_pause_unobservable",
        "silence never substitutes a verified pause signal", failures);
  result = evaluate_game_music_pause(
      {PauseSignalSource::button, "input", true, true, true});
  check(!result.verified && result.music_advances,
        "button input never proves game music paused", failures);
  result = evaluate_game_music_pause(
      {PauseSignalSource::authored_rule, "rule-r3", true, false, true});
  check(!result.verified && result.music_advances,
        "authored rule requires positive and negative controls", failures);
  return failures == 0 ? 0 : 1;
}
