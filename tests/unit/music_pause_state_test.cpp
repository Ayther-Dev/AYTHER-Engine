#include <ayther/engine/music_pause_state.hpp>

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
  MusicPauseState pause;
  pause.advance_music(100);
  pause.set_host_pause(true);
  pause.set_host_pause(true);
  pause.set_game_music_pause(true);
  pause.advance_music(50);
  pause.return_loop();
  check(pause.music_cursor() == 100 && pause.envelope_cursor() == 100 &&
            pause.candidate_music_time() == 100 && pause.loop_iterations() == 0,
        "overlapping idempotent causes freeze all musical clocks", failures);

  pause.set_host_pause(false);
  pause.advance_music(50);
  check(pause.music_cursor() == 100 && pause.paused(),
        "releasing host pause does not release game music pause", failures);
  pause.set_game_music_pause(false);
  pause.advance_music(50);
  pause.return_loop();
  check(!pause.paused() && pause.music_cursor() == 150 &&
            pause.envelope_cursor() == 150 &&
            pause.candidate_music_time() == 150 && pause.loop_iterations() == 1,
        "music resumes only after both independent causes clear", failures);

  pause.set_game_music_pause(true);
  pause.advance_effect(25, EffectPausePolicy::continue_during_game_pause);
  check(pause.effect_cursor() == 25,
        "menu effect can retain authored continue policy", failures);
  pause.set_host_pause(true);
  pause.advance_effect(25, EffectPausePolicy::continue_during_game_pause);
  check(pause.effect_cursor() == 25, "host pause still freezes effect output",
        failures);

  MusicTransitionQueue queue;
  queue.begin_link();
  (void)queue.submit({1, SequenceNodeId{2}, 1, 1, AppearanceId{1}, true}, 0);
  pause.cancel(queue);
  check(queue.pending_count() == 0,
        "pause never prevents transition cancellation", failures);
  return failures == 0 ? 0 : 1;
}
