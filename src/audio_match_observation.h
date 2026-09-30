#pragma once

#include "audio_observation_ids.h"
#include <ayther/audio_match_rule.h>

namespace ayther::audio_qa {

enum class MatchUse {
  live_assignment,
  live_sequence_interest,
  live_voice_route,
  replay_trigger,
  replay_voice_route,
  asset_coverage,
  bare_frame_mute,
  export_audio
};

struct MatchQuery {
  std::uint64_t frame = 0;
  std::uint64_t signature = 0;
  std::uint64_t instrument = 0;
  std::uint8_t pitch = kAudioNoPitch;
  MatchUse use = MatchUse::live_assignment;
  std::optional<observation::FactId> source;
  std::optional<std::uint64_t> source_index{};
  std::string_view source_kind = "unspecified";
  std::optional<std::uint64_t> event_start{};
  std::optional<std::uint64_t> event_end{};
  std::optional<std::uint64_t> evaluated_frame{};
};

namespace match_detail {
inline observation::FieldView
optional_number(std::string_view name, std::optional<std::uint64_t> value,
                observation::Unit unit) noexcept {
  return value ? observation::FieldView{name,
                                        observation::Availability::known,
                                        unit,
                                        *value,
                                        {}}
               : observation::FieldView{
                     name, observation::Availability::not_applicable, unit,
                     std::monostate{}, "query_has_no_value"};
}
inline observation::FieldView
known(std::string_view name, observation::Value value,
      observation::Unit unit = observation::Unit::none) noexcept {
  return {name, observation::Availability::known, unit, value, {}};
}
inline std::string_view name(MatchUse use) noexcept {
  switch (use) {
  case MatchUse::live_assignment:
    return "live_assignment";
  case MatchUse::live_sequence_interest:
    return "live_sequence_interest";
  case MatchUse::live_voice_route:
    return "live_voice_route";
  case MatchUse::replay_trigger:
    return "replay_trigger";
  case MatchUse::replay_voice_route:
    return "replay_voice_route";
  case MatchUse::asset_coverage:
    return "asset_coverage";
  case MatchUse::bare_frame_mute:
    return "bare_frame_mute";
  case MatchUse::export_audio:
    return "export_audio";
  }
  return "unknown";
}
inline std::string_view name(AudioMatchRule rule) noexcept {
  switch (rule) {
  case AudioMatchRule::kExact:
    return "exact";
  case AudioMatchRule::kInstrument:
    return "instrument";
  case AudioMatchRule::kInstrumentPitch:
    return "instrument_pitch";
  }
  return "unknown";
}
inline std::string_view name(AudioMatchIndex::CandidateResult result) noexcept {
  switch (result) {
  case AudioMatchIndex::CandidateResult::pitch_rejected:
    return "pitch_rejected";
  case AudioMatchIndex::CandidateResult::winner_updated:
    return "winner_updated";
  case AudioMatchIndex::CandidateResult::lower_priority:
    return "lower_priority";
  }
  return "unknown";
}
inline std::string_view
name(AudioMatchIndex::ResolutionResult result) noexcept {
  switch (result) {
  case AudioMatchIndex::ResolutionResult::invalid_instrument:
    return "invalid_instrument";
  case AudioMatchIndex::ResolutionResult::empty_index:
    return "empty_index";
  case AudioMatchIndex::ResolutionResult::unknown_instrument:
    return "unknown_instrument";
  case AudioMatchIndex::ResolutionResult::no_applicable_candidate:
    return "no_applicable_candidate";
  case AudioMatchIndex::ResolutionResult::selected:
    return "selected";
  }
  return "unknown";
}
} // namespace match_detail

// A single real lookup, owned by the calling session thread. No catalog, index,
// detector or output pointer is accessible here. The caller reports its
// existing exact lookup and supplies this visitor to the existing instrument
// traversal. Callbacks borrow fixed stack storage. Emission failures never
// affect matching.
class MatchObservation final {
public:
  MatchObservation(observation::Observer observer, IdentitySource &ids,
                   const MatchQuery &query) noexcept
      : observer_(observer), ids_(ids), query_(query) {
    using match_detail::known;
    if (observer_.on_fact == nullptr)
      return;
    const bool source_valid = query.source && query.source->producer != 0 &&
                              query.source->sequence != 0;
    complete_ = source_valid;
    const auto source =
        source_valid ? known("source", *query.source)
                     : observation::FieldView{
                           "source", observation::Availability::unknown,
                           observation::Unit::none, std::monostate{},
                           query.source ? "invalid_identity" : "not_observed"};
    const std::array fields{
        known("signature", query.signature),
        known("instrument", query.instrument),
        known("pitch", static_cast<std::uint64_t>(query.pitch)),
        known("use", match_detail::name(query.use)),
        source,
        known("provenance_complete", source_valid),
        known("source_kind", query.source_kind),
        match_detail::optional_number("source_index", query.source_index,
                                      observation::Unit::count),
        match_detail::optional_number("event_start", query.event_start,
                                      observation::Unit::emulation_frame),
        match_detail::optional_number("event_end", query.event_end,
                                      observation::Unit::emulation_frame),
        match_detail::optional_number("evaluated_frame", query.evaluated_frame,
                                      observation::Unit::emulation_frame)};
    std::array<observation::Cause, 1> causes{};
    if (source_valid)
      causes[0] = *query.source;
    query_id_ = emit("assignment_query", fields,
                     std::span{causes}.first(source_valid ? 1 : 0));
  }

  MatchObservation(const MatchObservation &) = delete;
  MatchObservation &operator=(const MatchObservation &) = delete;
  MatchObservation(MatchObservation &&) = delete;
  MatchObservation &operator=(MatchObservation &&) = delete;
  ~MatchObservation() = default;

  // Report the result of the one exact lookup already performed by the caller.
  // A successful lookup produces a candidate; selection remains a later fact.
  void exact_probe(bool found) noexcept {
    using match_detail::known;
    if (!query_id_)
      return;
    const std::array fields{known("signature", query_.signature),
                            known("found", found)};
    const std::array<observation::Cause, 1> causes{*query_id_};
    probe_id_ = emit("assignment_exact_probe", fields, causes);
    if (found)
      candidate({query_.signature, AudioMatchRule::kExact, kAudioNoPitch},
                true);
  }

  void operator()(const AudioMatchIndex::CandidateView &entry) noexcept {
    candidate(entry, false);
  }
  void
  operator()(const AudioMatchIndex::CandidateDecisionView &decision) noexcept {
    using match_detail::known;
    if (!query_id_)
      return;
    const bool linked = current_candidate_ != 0;
    complete_ = complete_ && linked;
    const std::array fields{
        known("signature", decision.candidate.signature),
        known("result", match_detail::name(decision.result)),
        known("best_signature", decision.best_signature),
        known("best_rank", static_cast<std::int64_t>(decision.best_rank)),
        known("candidate_linked", linked)};
    std::array<observation::Cause, 2> causes{*query_id_, {}};
    if (linked)
      causes[1] = observation::FactId{
          static_cast<std::uint32_t>(Producer::selection), current_candidate_};
    const auto id = emit("assignment_candidate_decision", fields,
                         std::span{causes}.first(linked ? 2 : 1));
    if (decision.result == AudioMatchIndex::CandidateResult::winner_updated)
      winner_ = id ? id->sequence : 0;
    current_candidate_ = 0;
  }
  void operator()(const AudioMatchIndex::ResolutionView &resolution) noexcept {
    terminal(resolution, "instrument_index");
  }
  void exact_selected() noexcept {
    winner_ = current_candidate_;
    current_candidate_ = 0;
    terminal(
        {AudioMatchIndex::ResolutionResult::selected, query_.signature, -1},
        "exact_lookup");
  }

  [[nodiscard]] bool complete() const noexcept { return complete_; }
  [[nodiscard]] std::optional<observation::FactId> id() const noexcept {
    return query_id_;
  }
  [[nodiscard]] std::optional<observation::FactId>
  selection_id() const noexcept {
    return winner_
               ? std::optional<observation::FactId>{observation::FactId{
                     static_cast<std::uint32_t>(Producer::selection), winner_}}
               : std::nullopt;
  }

private:
  void candidate(const AudioMatchIndex::CandidateView &entry,
                 bool exact) noexcept {
    using match_detail::known;
    if (!query_id_)
      return;
    const auto visit = visits_.next();
    if (!visit) {
      complete_ = false;
      return;
    }
    const auto pitch =
        exact
            ? observation::FieldView{"pitch",
                                     observation::Availability::not_applicable,
                                     observation::Unit::none, std::monostate{},
                                     "exact_lookup_has_no_pitch_rule"}
            : known("pitch", static_cast<std::uint64_t>(entry.pitch));
    const std::array fields{
        known("signature", entry.signature),
        known("rule", match_detail::name(entry.rule)),
        known("rule_value", static_cast<std::uint64_t>(entry.rule)),
        pitch,
        known("origin",
              std::string_view{exact ? "exact_lookup" : "instrument_index"}),
        known("visit_index", *visit - 1, observation::Unit::count),
        known("probe_observed", probe_id_.has_value())};
    std::array<observation::Cause, 2> causes{*query_id_, observation::FactId{}};
    if (probe_id_)
      causes[1] = *probe_id_;
    else
      complete_ = false;
    const auto id = emit("assignment_candidate", fields,
                         std::span{causes}.first(probe_id_ ? 2 : 1));
    current_candidate_ = id ? id->sequence : 0;
  }

  void terminal(const AudioMatchIndex::ResolutionView &resolution,
                std::string_view branch) noexcept {
    using match_detail::known;
    if (!query_id_)
      return;
    const bool selected =
        resolution.result == AudioMatchIndex::ResolutionResult::selected;
    const bool linked = !selected || winner_ != 0;
    complete_ = complete_ && linked;
    const std::array fields{
        known("result", match_detail::name(resolution.result)),
        known("matched", selected),
        known("selected_signature", resolution.selected_signature),
        known("selected_rank",
              static_cast<std::int64_t>(resolution.selected_rank)),
        known("branch", branch),
        known("selection_linked", linked)};
    std::array<observation::Cause, 3> causes{*query_id_, {}, {}};
    std::size_t count = 1;
    if (probe_id_)
      causes[count++] = *probe_id_;
    if (selected && winner_)
      causes[count++] = observation::FactId{
          static_cast<std::uint32_t>(Producer::selection), winner_};
    const auto id =
        emit("assignment_selection", fields, std::span{causes}.first(count));
    winner_ = id ? id->sequence : 0;
  }

  [[nodiscard]] std::optional<observation::FactId>
  emit(std::string_view kind, std::span<const observation::FieldView> fields,
       std::span<const observation::Cause> causes) noexcept {
    const auto fact = ids_.next_fact(Producer::selection);
    if (!fact) {
      complete_ = false;
      return {};
    }
    observer_.observe(observation::FactView{
        *fact,
        kind,
        {observation::Availability::known, query_.frame, {}},
        causes,
        {},
        fields});
    return fact;
  }

  observation::Observer observer_;
  IdentitySource &ids_;
  MatchQuery query_;
  std::optional<observation::FactId> query_id_;
  std::optional<observation::FactId> probe_id_;
  std::uint64_t current_candidate_ = 0;
  std::uint64_t winner_ = 0;
  Sequence visits_;
  bool complete_ = true;
};

static_assert(sizeof(MatchObservation) <= 256);
} // namespace ayther::audio_qa
