#include <ayther/engine/music_position_recovery.hpp>

#include <cstdio>
#include <optional>

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
  MusicPositionRecovery recovery{AppearanceId{8}, 100, 2};
  auto state = recovery.observe(AppearanceId{8}, std::nullopt, false);
  check(state.certainty == PositionRecoveryCertainty::confirmed &&
            !state.position_transitions_suspended,
        "isolated missing membership does not lose position", failures);

  state = recovery.observe(AppearanceId{8}, 103, true);
  check(state.certainty == PositionRecoveryCertainty::unconfirmed &&
            state.diagnostic == "position_unconfirmed" &&
            state.position_transitions_suspended && state.hd_course_continues &&
            !state.seek_requested,
        "position outside two-frame tolerance suspends only dependent "
        "transitions",
        failures);
  state = recovery.observe(AppearanceId{9}, 100, true);
  check(state.certainty == PositionRecoveryCertainty::unconfirmed &&
            !state.seek_requested,
        "different appearance cannot reconfirm or seek", failures);
  state = recovery.observe(AppearanceId{8}, 120, true);
  check(state.certainty == PositionRecoveryCertainty::unconfirmed &&
            !state.seek_requested,
        "compatible evidence at another location never auto-seeks", failures);
  state = recovery.observe(AppearanceId{8}, 102, true);
  check(state.certainty == PositionRecoveryCertainty::confirmed &&
            !state.position_transitions_suspended &&
            state.hd_course_continues && !state.seek_requested,
        "same appearance within two frames reconfirms without changing cursor",
        failures);
  return failures == 0 ? 0 : 1;
}
