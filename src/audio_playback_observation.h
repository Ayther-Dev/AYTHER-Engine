#pragma once

#include "audio_observation_ids.h"

namespace ayther::audio_qa {

struct PlaybackRequest {
  std::uint64_t frame = 0;
  std::uint64_t detected_signature = 0;
  std::uint64_t assignment_signature = 0;
  std::optional<observation::FactId> selection;
  observation::OccurrenceId occurrence;
};

class PlaybackObservation final {
public:
  PlaybackObservation(observation::Observer observer, IdentitySource &ids,
                      const PlaybackRequest &request) noexcept
      : observer_(observer), ids_(ids), request_(request) {
    if (!observer_.on_fact)
      return;
    const bool linked = request.selection && request.selection->producer &&
                        request.selection->sequence;
    complete_ = linked && request.occurrence.value != 0;
    const std::array fields{
        known("detected_signature", request.detected_signature),
        known("assignment_signature", request.assignment_signature),
        linked ? known("selection", *request.selection)
               : absent("selection", "selection_not_observed"),
        request.occurrence.value ? known("occurrence", request.occurrence)
                                 : absent("occurrence", "identity_unavailable"),
        known("links_complete", complete_)};
    std::array<observation::Cause, 1> causes{};
    if (linked)
      causes[0] = *request.selection;
    request_id_ = emit("hd_playback_request", fields,
                       std::span{causes}.first(linked ? 1 : 0));
  }

  void
  decision(std::string_view action, std::string_view reason,
           std::optional<observation::OccurrenceId> previous = std::nullopt,
           std::optional<observation::FactId> previous_request =
               std::nullopt) noexcept {
    if (!request_id_)
      return;
    if (previous.has_value() != previous_request.has_value())
      complete_ = false;
    const std::array fields{
        known("action", action), known("reason", reason),
        known("occurrence", request_.occurrence),
        previous ? known("previous_occurrence", *previous)
                 : absent("previous_occurrence", "no_previous_voice",
                          observation::Availability::not_applicable),
        previous_request ? known("previous_request", *previous_request)
                         : absent("previous_request", "no_previous_voice",
                                  observation::Availability::not_applicable)};
    const std::array<observation::Cause, 2> causes{
        observation::Cause{*request_id_},
        previous_request ? observation::Cause{*previous_request}
                         : observation::Cause{observation::FactId{}}};
    decision_id_ = emit("hd_playback_decision", fields,
                        std::span{causes}.first(previous_request ? 2 : 1));
  }

  void effect(std::string_view result, std::optional<bool> played) noexcept {
    if (!decision_id_)
      return;
    const std::array fields{
        known("result", result),
        played ? known("played", *played)
               : absent("played", "no_physical_start_attempt",
                        observation::Availability::not_applicable),
        known("occurrence", request_.occurrence)};
    const std::array<observation::Cause, 1> causes{*decision_id_};
    (void)emit("hd_playback_effect", fields, causes);
  }

  [[nodiscard]] bool complete() const noexcept { return complete_; }
  [[nodiscard]] std::optional<observation::FactId> request_id() const noexcept {
    return request_id_;
  }

private:
  static observation::FieldView known(std::string_view name,
                                      observation::Value value) noexcept {
    return {name,
            observation::Availability::known,
            observation::Unit::none,
            value,
            {}};
  }
  static observation::FieldView
  absent(std::string_view name, std::string_view reason,
         observation::Availability availability =
             observation::Availability::unknown) noexcept {
    return {name, availability, observation::Unit::none, std::monostate{},
            reason};
  }
  std::optional<observation::FactId>
  emit(std::string_view kind, std::span<const observation::FieldView> fields,
       std::span<const observation::Cause> causes) noexcept {
    const auto id = ids_.next_fact(Producer::selection);
    if (!id) {
      complete_ = false;
      return {};
    }
    observer_.observe(observation::FactView{
        *id,
        kind,
        {observation::Availability::known, request_.frame, {}},
        causes,
        {},
        fields});
    return id;
  }

  observation::Observer observer_;
  IdentitySource &ids_;
  PlaybackRequest request_;
  std::optional<observation::FactId> request_id_;
  std::optional<observation::FactId> decision_id_;
  bool complete_ = true;
};

static_assert(sizeof(PlaybackObservation) <= 160);
} // namespace ayther::audio_qa
