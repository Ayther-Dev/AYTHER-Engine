#pragma once

#include <ayther/engine/music_pause_state.hpp>
#include <ayther/engine/music_sequence.hpp>
#include <ayther/engine/music_transition_arbiter.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace ayther::engine {

struct MusicSemanticState {
  std::string identity;
  std::string segment;
  SampleRegion region{};
  std::uint64_t visit{};
  std::uint64_t iteration{};
  std::uint64_t cursor{};
  std::uint64_t generation{};
  bool paused{};

  friend bool operator==(const MusicSemanticState &,
                         const MusicSemanticState &) = default;
};

class MusicExecutionAdapter {
public:
  explicit MusicExecutionAdapter(
      const MusicSequenceDefinition &definition) noexcept
      : definition_(&definition), traversal_(definition) {}

  [[nodiscard]] bool start(OccurrenceId occurrence) {
    const bool accepted =
        valid_sequence_shape(*definition_) && traversal_.enter(occurrence);
    last_reason_ =
        accepted ? "entry_selected" : "invalid_sequence_or_occurrence";
    return accepted;
  }

  [[nodiscard]] bool transition(SequenceNodeId destination,
                                std::uint64_t event_id) {
    const auto choice = arbiter_.choose(
        {{event_id, destination, 0, OutputAuthority::explicit_order}});
    if (choice.status != TransitionChoiceStatus::selected) {
      last_reason_ = choice.diagnostic.empty() ? "transition_not_selected"
                                               : choice.diagnostic;
      return false;
    }
    const bool accepted = traversal_.transition_to(destination);
    last_reason_ = accepted ? "transition_selected" : "transition_not_authored";
    return accepted;
  }

  void set_host_pause(bool paused) noexcept { pause_.set_host_pause(paused); }
  void set_game_music_pause(bool paused) noexcept {
    pause_.set_game_music_pause(paused);
  }
  void advance(std::uint64_t frames) noexcept { pause_.advance_music(frames); }

  void restore_generation(std::uint64_t generation) noexcept {
    if (generation <= generation_)
      return;
    generation_ = generation;
    arbiter_.invalidate(MusicBoundary::restore_or_session_close);
  }

  [[nodiscard]] SampleRegion active_region() const noexcept {
    const auto assignment_id = traversal_.position().assignment;
    const auto assignment =
        std::ranges::find_if(definition_->assignments, [&](const auto &value) {
          return value.id == assignment_id;
        });
    return assignment == definition_->assignments.end() ? SampleRegion{}
                                                        : assignment->region;
  }
  [[nodiscard]] bool paused() const noexcept { return pause_.paused(); }
  [[nodiscard]] std::uint64_t music_cursor() const noexcept {
    return pause_.music_cursor();
  }
  [[nodiscard]] std::uint64_t generation() const noexcept {
    return generation_;
  }
  [[nodiscard]] constexpr std::uint64_t
  offline_analysis_calls() const noexcept {
    return 0;
  }
  [[nodiscard]] std::string_view last_reason() const noexcept {
    return last_reason_;
  }
  [[nodiscard]] MusicSemanticState semantic_state() const {
    const auto &position = traversal_.position();
    const auto segment =
        std::ranges::find_if(definition_->segments, [&](const auto &value) {
          return value.id == position.segment;
        });
    return {definition_->name,
            segment == definition_->segments.end() ? std::string{}
                                                   : segment->name,
            active_region(),
            position.visit,
            position.iteration,
            music_cursor(),
            generation_,
            paused()};
  }

private:
  const MusicSequenceDefinition *definition_{};
  SequenceTraversal traversal_;
  MusicPauseState pause_;
  MusicTransitionArbiter arbiter_;
  std::uint64_t generation_{1};
  std::string last_reason_;
};

} // namespace ayther::engine
