#include <ayther/engine/music_voice_budget.hpp>

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
  MusicVoiceBudget identity_budget;
  for (int voice = 0; voice < 3; ++voice)
    check(identity_budget.add_music_voice(MusicIdentityId{1}, AudioBusId{1},
                                          true)
                  .accepted,
          "first three identity voices are admitted", failures);
  auto result = identity_budget.add_music_voice(MusicIdentityId{1},
                                                AudioBusId{1}, true);
  check(!result.accepted && result.diagnostic == "transition_voice_limit" &&
            identity_budget.music_voice_count() == 3,
        "fourth identity voice is rejected without eviction", failures);

  MusicVoiceBudget bus_budget;
  for (std::uint64_t identity = 1; identity <= 6; ++identity)
    check(bus_budget.add_music_voice(MusicIdentityId{identity}, AudioBusId{2},
                                     true)
                  .accepted,
          "six authorized substitution voices fit one bus", failures);
  result = bus_budget.add_music_voice(MusicIdentityId{7}, AudioBusId{2}, true);
  check(!result.accepted && bus_budget.music_voice_count() == 6,
        "seventh bus voice is rejected", failures);

  MusicVoiceBudget total_budget;
  for (std::size_t effect = 0; effect < 250; ++effect)
    check(total_budget.add_effect_voice(), "effect fits total budget", failures);
  for (std::uint64_t identity = 1; identity <= 6; ++identity)
    check(total_budget.add_music_voice(MusicIdentityId{identity}, AudioBusId{3},
                                       true)
                  .accepted,
          "music fills remaining total capacity", failures);
  result = total_budget.add_music_voice(MusicIdentityId{7}, AudioBusId{4}, true);
  check(!result.accepted && total_budget.total_voice_count() == 256 &&
            total_budget.effect_voice_count() == 250,
        "global excess preserves all effects", failures);

  MusicVoiceBudget lifecycle;
  for (int cycle = 0; cycle < 100; ++cycle) {
    lifecycle.begin_link();
    lifecycle.set_pending();
    lifecycle.pause(true);
    lifecycle.pause(false);
    lifecycle.cancel_transition();
  }
  check(lifecycle.retained_transition_objects() == 0 &&
            lifecycle.total_voice_count() == 0,
        "repeated links and pauses retain no voices or pending objects",
        failures);
  return failures == 0 ? 0 : 1;
}
