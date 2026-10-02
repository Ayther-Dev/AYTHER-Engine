#pragma once

#include <ayther/engine/music_identity_state.hpp>

#include <cstdint>
#include <string>

namespace ayther::engine {

enum class MusicSignalRole : std::uint8_t {
  entry_trigger,
  evidence,
  reference,
  transition,
  terminal,
};

struct ObservedMusicSignal {
  std::uint64_t signature{};
  MusicSignalRole role{MusicSignalRole::evidence};
  SequenceNodeId destination{};
  bool declared_condition{};
  std::string label;
};

enum class MusicRecognitionAction : std::uint8_t {
  ignored,
  entered,
  continuity_evidence,
  evidence_observed,
  reference_observed,
  transitioned,
  transition_rejected,
  terminal_rejected,
  finished,
};

class CausalMusicRecognizer {
public:
  explicit CausalMusicRecognizer(
      const MusicSequenceDefinition &definition) noexcept
      : state_(definition) {}

  [[nodiscard]] MusicRecognitionAction
  observe(const ObservedMusicSignal &signal) {
    if (!state_.active()) {
      if (signal.role != MusicSignalRole::entry_trigger)
        return MusicRecognitionAction::ignored;
      return state_.enter(OccurrenceId{++occurrence_serial_},
                          IdentityStateCause::entry_trigger)
                 ? MusicRecognitionAction::entered
                 : MusicRecognitionAction::ignored;
    }

    switch (signal.role) {
    case MusicSignalRole::entry_trigger:
      return MusicRecognitionAction::continuity_evidence;
    case MusicSignalRole::evidence:
      return MusicRecognitionAction::evidence_observed;
    case MusicSignalRole::reference:
      return MusicRecognitionAction::reference_observed;
    case MusicSignalRole::transition:
      if (!signal.declared_condition)
        return MusicRecognitionAction::transition_rejected;
      return state_.transition(signal.destination,
                               IdentityStateCause::authored_transition)
                 ? MusicRecognitionAction::transitioned
                 : MusicRecognitionAction::transition_rejected;
    case MusicSignalRole::terminal:
      if (!signal.declared_condition)
        return MusicRecognitionAction::terminal_rejected;
      state_.finish(IdentityStateCause::declared_terminal);
      return MusicRecognitionAction::finished;
    }
    return MusicRecognitionAction::ignored;
  }

  [[nodiscard]] const MusicIdentityState &state() const noexcept {
    return state_;
  }

  [[nodiscard]] bool internal_loop() {
    return state_.internal_loop(IdentityStateCause::internal_loop_return);
  }

private:
  MusicIdentityState state_;
  std::uint64_t occurrence_serial_{};
};

} // namespace ayther::engine
