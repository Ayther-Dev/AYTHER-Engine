#include <ayther/engine/music_transition_arbiter.hpp>

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
  MusicTransitionArbiter arbiter;
  const auto winner = arbiter.choose(
      {{1, SequenceNodeId{2}, 10, OutputAuthority::game_pattern},
       {2, SequenceNodeId{3}, 20, OutputAuthority::explicit_order}});
  check(winner.status == TransitionChoiceStatus::selected &&
            winner.destination == SequenceNodeId{3} &&
            winner.authority == OutputAuthority::explicit_order,
        "highest explicit priority wins and preserves output authority",
        failures);
  check(arbiter.choose({{2, SequenceNodeId{3}, 20,
                         OutputAuthority::explicit_order}})
                .status == TransitionChoiceStatus::already_consumed,
        "one observed event cannot be consumed twice", failures);

  const auto conflict = arbiter.choose(
      {{3, SequenceNodeId{4}, 30, OutputAuthority::hd_region},
       {4, SequenceNodeId{5}, 30, OutputAuthority::game_pattern}});
  check(conflict.status == TransitionChoiceStatus::conflict &&
            conflict.diagnostic == "transition_conflict" &&
            !conflict.destination,
        "equal priority to distinct destinations conflicts without transition",
        failures);

  check(boundary_precedes(MusicBoundary::restore_or_session_close,
                          MusicBoundary::explicit_cancel_restart_replace) &&
            boundary_precedes(MusicBoundary::explicit_cancel_restart_replace,
                              MusicBoundary::pause_change) &&
            boundary_precedes(MusicBoundary::pause_change,
                              MusicBoundary::internal_transition) &&
            boundary_precedes(MusicBoundary::internal_transition,
                              MusicBoundary::loop_or_exhaustion) &&
            boundary_precedes(MusicBoundary::loop_or_exhaustion,
                              MusicBoundary::new_entry),
        "shared boundary order is total and normative", failures);

  arbiter.set_pending({5, SequenceNodeId{6}, 1, OutputAuthority::game_pattern});
  arbiter.invalidate(MusicBoundary::explicit_cancel_restart_replace);
  check(!arbiter.pending().has_value(),
        "explicit cancellation invalidates pending transition", failures);
  arbiter.set_pending({6, SequenceNodeId{7}, 1, OutputAuthority::hd_region});
  arbiter.invalidate(MusicBoundary::restore_or_session_close);
  check(!arbiter.pending().has_value(),
        "restore boundary invalidates pending transition", failures);
  check(arbiter.source_exhausted().status ==
                TransitionChoiceStatus::delegate_to_mix &&
            arbiter.source_exhausted().diagnostic ==
                "source_exhausted_before_transition",
        "source exhaustion delegates to mixing policy", failures);
  return failures == 0 ? 0 : 1;
}
