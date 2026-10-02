#pragma once

#include <ayther/engine/music_sequence.hpp>

#include <cstdint>

namespace ayther::engine {

enum class IdentityStateCause : std::uint8_t {
  none,
  entry_trigger,
  authored_transition,
  internal_loop_return,
  declared_terminal,
  explicit_restart,
  explicit_cancel,
};

class MusicIdentityState {
public:
  explicit MusicIdentityState(
      const MusicSequenceDefinition &definition) noexcept
      : traversal_(definition) {}

  [[nodiscard]] bool enter(OccurrenceId occurrence, IdentityStateCause cause) {
    if (active_ || cause != IdentityStateCause::entry_trigger ||
        !traversal_.enter(occurrence))
      return false;
    active_ = true;
    last_cause_ = cause;
    return true;
  }

  [[nodiscard]] bool transition(SequenceNodeId destination,
                                IdentityStateCause cause) {
    if (!active_ || cause != IdentityStateCause::authored_transition ||
        !traversal_.transition_to(destination))
      return false;
    last_cause_ = cause;
    return true;
  }

  [[nodiscard]] bool internal_loop(IdentityStateCause cause) {
    if (!active_ || cause != IdentityStateCause::internal_loop_return ||
        !traversal_.return_internal_loop())
      return false;
    last_cause_ = cause;
    return true;
  }

  void finish(IdentityStateCause cause) noexcept {
    if (!active_ || (cause != IdentityStateCause::declared_terminal &&
                     cause != IdentityStateCause::explicit_cancel))
      return;
    active_ = false;
    last_cause_ = cause;
  }

  [[nodiscard]] bool restart(OccurrenceId occurrence,
                             IdentityStateCause cause) {
    if (active_ || cause != IdentityStateCause::explicit_restart ||
        !traversal_.enter(occurrence))
      return false;
    active_ = true;
    last_cause_ = cause;
    return true;
  }

  [[nodiscard]] bool active() const noexcept { return active_; }
  [[nodiscard]] IdentityStateCause last_cause() const noexcept {
    return last_cause_;
  }
  [[nodiscard]] const SequencePosition &position() const noexcept {
    return traversal_.position();
  }

private:
  SequenceTraversal traversal_;
  IdentityStateCause last_cause_{IdentityStateCause::none};
  bool active_{};
};

} // namespace ayther::engine
