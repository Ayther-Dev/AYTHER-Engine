#pragma once

#include "audio_observation_ids.h"
#include <ayther/audio_seq_anchor.h>

namespace ayther::audio_qa {
enum class SequenceUse { live_pack, live_authored, replay_table };
struct SequenceContext {
  std::uint64_t session_frame = 0;
  SequenceUse use = SequenceUse::live_pack;
  std::optional<observation::FactId> source;
  std::string_view origin_storage = "caller_owned";
};
struct SequenceInputSource {
  std::optional<observation::FactId> source;
  std::optional<observation::FactId> transformation;
  std::optional<std::uint64_t> index;
  std::optional<std::uint64_t> signature;
  std::string_view origin = "unknown";
};

// One reusable session allocation: 4096 sequence triggers plus 64 live voices.
// Exceeding observation capacity never bounds the real signature vector.
class SequenceOrigins final {
public:
  static constexpr std::size_t capacity = 4096 + 64;
  void clear() noexcept {
    count_ = 0;
    complete_ = true;
  }
  void append(const SequenceInputSource &origin) noexcept {
    if (count_ < rows_.size())
      rows_[count_++] = origin;
    else
      complete_ = false;
  }
  [[nodiscard]] std::span<const SequenceInputSource> view() const noexcept {
    return std::span{rows_}.first(count_);
  }
  [[nodiscard]] bool complete() const noexcept { return complete_; }

private:
  std::array<SequenceInputSource, capacity> rows_{};
  std::size_t count_ = 0;
  bool complete_ = true;
};
static_assert(sizeof(SequenceOrigins) <= std::size_t{512} * 1024);

struct SequenceTransformation {
  std::uint64_t frame = 0;
  std::optional<observation::FactId> source;
  std::optional<observation::FactId> resolution;
  std::uint64_t original = 0;
  std::uint64_t target = 0;
  std::string_view kind;
  std::uint64_t key = 0;
  std::uint64_t rule = 0;
  std::uint64_t instrument = 0;
  std::uint64_t pitch = 255;
};

namespace sequence_detail {
inline bool valid(std::optional<observation::FactId> id) noexcept {
  return id && id->producer && id->sequence;
}
// Retain links to an actually emitted contiguous group without a row-sized
// history. Check every appended identity; never infer through gaps or wrapping.
struct FactRun {
  std::optional<observation::FactId> first;
  std::uint64_t count = 0;
  bool coherent = true;
  bool append(std::optional<observation::FactId> id) noexcept {
    if (!coherent || !id || id->producer == 0 || id->sequence == 0 ||
        (first && (id->producer != first->producer ||
                   count > (std::numeric_limits<std::uint64_t>::max)() -
                               first->sequence ||
                   id->sequence != first->sequence + count))) {
      coherent = false;
      return false;
    }
    if (!first)
      first = id;
    ++count;
    return true;
  }
  std::optional<observation::FactId> at(std::uint64_t index) const noexcept {
    if (!coherent || !first || index >= count)
      return {};
    return observation::FactId{first->producer, first->sequence + index};
  }
};
inline observation::FieldView
known(std::string_view name, observation::Value value,
      observation::Unit unit = observation::Unit::none) noexcept {
  return {name, observation::Availability::known, unit, value, {}};
}
inline observation::FieldView
absent(std::string_view name, std::string_view reason,
       observation::Availability availability =
           observation::Availability::unknown) noexcept {
  return {name, availability, observation::Unit::none, std::monostate{},
          reason};
}
inline std::string_view name(SequenceUse use) noexcept {
  switch (use) {
  case SequenceUse::live_pack:
    return "live_pack";
  case SequenceUse::live_authored:
    return "live_authored";
  case SequenceUse::replay_table:
    return "replay_table";
  }
  return "unknown";
}
inline std::string_view name(SeqAnchorCandidateStage stage) noexcept {
  switch (stage) {
  case SeqAnchorCandidateStage::disabled:
    return "disabled";
  case SeqAnchorCandidateStage::internal:
    return "internal";
  case SeqAnchorCandidateStage::not_head:
    return "not_head";
  case SeqAnchorCandidateStage::formed:
    return "formed";
  case SeqAnchorCandidateStage::accumulated:
    return "accumulated";
  }
  return "unknown";
}
inline std::string_view name(SeqAnchorDecisionResult result) noexcept {
  switch (result) {
  case SeqAnchorDecisionResult::quorum_rejected:
    return "quorum_rejected";
  case SeqAnchorDecisionResult::claimed_by_open_sequence:
    return "claimed_by_open_sequence";
  case SeqAnchorDecisionResult::selected:
    return "selected";
  }
  return "unknown";
}
inline std::string_view name(SeqAnchorResolutionResult result) noexcept {
  switch (result) {
  case SeqAnchorResolutionResult::no_candidate:
    return "no_candidate";
  case SeqAnchorResolutionResult::no_selection:
    return "no_selection";
  case SeqAnchorResolutionResult::selected:
    return "selected";
  }
  return "unknown";
}
} // namespace sequence_detail

// Records an actual canonical signature append, never repeats its predicate.
inline std::optional<observation::FactId>
observe_sequence_transformation(observation::Observer observer,
                                IdentitySource &ids,
                                const SequenceTransformation &value) noexcept {
  using namespace sequence_detail;
  if (!observer.on_fact)
    return {};
  const auto id = ids.next_fact(Producer::selection);
  if (!id)
    return {};
  const bool inline_rule = value.kind == "sequence_rule_trigger";
  const std::array fields{
      known("original_signature", value.original),
      known("target_signature", value.target),
      known("transformation", value.kind),
      known("key", value.key),
      inline_rule ? known("rule", value.rule)
                  : absent("rule", "assignment_query",
                           observation::Availability::not_applicable),
      inline_rule ? known("instrument", value.instrument)
                  : absent("instrument", "assignment_query",
                           observation::Availability::not_applicable),
      inline_rule ? known("pitch", value.pitch)
                  : absent("pitch", "assignment_query",
                           observation::Availability::not_applicable),
      valid(value.source) ? known("source", *value.source)
                          : absent("source", "not_observed"),
      valid(value.resolution)
          ? known("resolution", *value.resolution)
          : absent("resolution",
                   inline_rule ? "inline_sequence_rule" : "not_observed",
                   inline_rule ? observation::Availability::not_applicable
                               : observation::Availability::unknown)};
  std::array<observation::Cause, 2> causes{};
  std::size_t count = 0;
  if (valid(value.source))
    causes[count++] = *value.source;
  if (valid(value.resolution))
    causes[count++] = *value.resolution;
  observer.observe(
      observation::FactView{*id,
                            "sequence_signature_transform",
                            {observation::Availability::known, value.frame, {}},
                            std::span{causes}.first(count),
                            {},
                            fields});
  return id;
}

// Session-owner thread only, synchronous borrowed observations, no searches,
// detector calls, retained spans, allocations or output mutation. Group links
// use the non-reentrant single-writer selection lane. No anchor verdict is
// emitted.
class SequenceObservation final {
public:
  SequenceObservation(observation::Observer observer, IdentitySource &ids,
                      SequenceContext context) noexcept
      : observer_(observer), ids_(ids), context_(context) {}
  SequenceObservation(const SequenceObservation &) = delete;
  SequenceObservation &operator=(const SequenceObservation &) = delete;
  SequenceObservation(SequenceObservation &&) = delete;
  SequenceObservation &operator=(SequenceObservation &&) = delete;
  ~SequenceObservation() = default;

  void begin(const SeqAnchorFrameView &view,
             std::span<const SequenceInputSource> origins = {}) noexcept {
    using namespace sequence_detail;
    query_.reset();
    inputs_ = {};
    subs_ = {};
    states_ = {};
    if (!observer_.on_fact)
      return;
    frame_ = view.frame;
    state_matches_ = view.states.size() == view.substitutions.size();
    const bool source_valid = valid(context_.source);
    complete_ = complete_ && source_valid &&
                context_.origin_storage != "allocation_failed" &&
                context_.origin_storage != "limit_exceeded";
    const std::array fields{
        known("use", name(context_.use)),
        known("origin_storage", context_.origin_storage),
        known("origin_count", static_cast<std::uint64_t>(origins.size()),
              observation::Unit::count),
        known("anchor_frame", static_cast<std::uint64_t>(frame_),
              observation::Unit::emulation_frame),
        known("input_count", static_cast<std::uint64_t>(view.signatures.size()),
              observation::Unit::count),
        known("sub_count",
              static_cast<std::uint64_t>(view.substitutions.size()),
              observation::Unit::count),
        known("state_count", static_cast<std::uint64_t>(view.states.size()),
              observation::Unit::count),
        known("state_will_reset", !state_matches_),
        source_valid ? known("source", *context_.source)
                     : absent("source", "not_observed")};
    const std::array parents{context_.source};
    query_ = emit("sequence_query", fields, parents);
    if (!query_)
      return;
    for (std::size_t i = 0; i < view.signatures.size(); ++i) {
      SequenceInputSource origin;
      if (i < origins.size()) {
        origin = origins[i];
      } else if (origins.empty() &&
                 view.source_indices.size() == view.signatures.size()) {
        origin = {context_.source,
                  {},
                  view.source_indices[i],
                  view.signatures[i],
                  "detector_event"};
      }
      const bool direct = origin.origin == "detector_signature" ||
                          origin.origin == "detector_event";
      const bool transformed = origin.origin == "resolved_assignment" ||
                               origin.origin == "sequence_rule_trigger";
      const bool source =
          valid(origin.source) && origin.signature && origin.index &&
          (direct || (transformed && valid(origin.transformation)));
      complete_ = complete_ && source;
      const std::array input_fields{
          known("input_index", static_cast<std::uint64_t>(i),
                observation::Unit::count),
          known("signature", view.signatures[i]),
          known("origin", origin.origin),
          valid(origin.source) ? known("source", *origin.source)
                               : absent("source", "not_observed"),
          origin.index
              ? known("source_index", *origin.index, observation::Unit::count)
              : absent("source_index", "not_observed"),
          origin.signature ? known("source_signature", *origin.signature)
                           : absent("source_signature", "not_observed"),
          valid(origin.transformation)
              ? known("transformation", *origin.transformation)
              : absent("transformation",
                       direct ? "signature_unmodified" : "not_observed",
                       direct ? observation::Availability::not_applicable
                              : observation::Availability::unknown),
          known("provenance_complete", source)};
      const std::array causes{query_, origin.source, origin.transformation};
      const auto id = emit("sequence_input", input_fields, causes);
      if (!inputs_.append(id))
        complete_ = false;
    }
    if ((!origins.empty() && origins.size() != view.signatures.size()) ||
        (!view.source_indices.empty() &&
         view.source_indices.size() != view.signatures.size()))
      complete_ = false;
    const std::array query_cause{query_};
    for (std::size_t i = 0; i < view.substitutions.size(); ++i) {
      const auto &sub = view.substitutions[i];
      const std::array sub_fields{
          known("sub_index", static_cast<std::uint64_t>(i),
                observation::Unit::count),
          known("key", sub.key),
          known("trigger_signature", sub.trigger_signature),
          known("duration_frames",
                static_cast<std::uint64_t>(sub.duration_frames),
                observation::Unit::emulation_frame),
          known("span_frames", static_cast<std::uint64_t>(sub.span_frames),
                observation::Unit::emulation_frame),
          known("enabled", sub.enabled),
          known("looping", sub.looping),
          known("member_count",
                static_cast<std::uint64_t>(sub.signatures.size()),
                observation::Unit::count),
          known("head_count", static_cast<std::uint64_t>(sub.head.size()),
                observation::Unit::count)};
      if (!subs_.append(emit("sequence_substitution", sub_fields, query_cause)))
        complete_ = false;
    }
    for (std::size_t i = 0; i < view.states.size(); ++i) {
      const auto &state = view.states[i];
      const std::array state_fields{
          known("sub_index", static_cast<std::uint64_t>(i),
                observation::Unit::count),
          known("next_free", static_cast<std::uint64_t>(state.next_free),
                observation::Unit::emulation_frame),
          known("win_start", static_cast<std::uint64_t>(state.win_start),
                observation::Unit::emulation_frame),
          known("win_end", static_cast<std::uint64_t>(state.win_end),
                observation::Unit::emulation_frame),
          known("open", state.open),
          known("will_be_used", state_matches_)};
      const std::array causes{query_, subs_.at(i)};
      if (!states_.append(emit("sequence_state_input", state_fields, causes)))
        complete_ = false;
    }
    for (std::size_t i = 0; i < view.substitutions.size(); ++i) {
      members(view.substitutions[i].signatures, i, "member");
      members(view.substitutions[i].head, i, "head");
    }
  }

  void operator()(const SeqAnchorCandidateView &view) noexcept {
    using namespace sequence_detail;
    if (!observer_.on_fact)
      return;
    if (!query_) {
      complete_ = false;
      return;
    }
    const bool current = view.frame == frame_;
    const auto input = current ? inputs_.at(view.input_index) : std::nullopt;
    const auto sub = current ? subs_.at(view.sub_index) : std::nullopt;
    const auto state =
        current && state_matches_ ? states_.at(view.sub_index) : std::nullopt;
    const bool formed = view.stage == SeqAnchorCandidateStage::formed ||
                        view.stage == SeqAnchorCandidateStage::accumulated;
    const bool links =
        view.frame == frame_ && input && sub && (!state_matches_ || state);
    complete_ = complete_ && links;
    const std::array fields{
        known("anchor_frame", static_cast<std::uint64_t>(view.frame),
              observation::Unit::emulation_frame),
        known("input_index", static_cast<std::uint64_t>(view.input_index),
              observation::Unit::count),
        known("sub_index", static_cast<std::uint64_t>(view.sub_index),
              observation::Unit::count),
        known("signature", view.signature),
        known("key", view.key),
        known("stage", name(view.stage)),
        formed ? known("trigger", view.trigger)
               : absent("trigger", "branch_not_reached",
                        observation::Availability::not_applicable),
        formed ? known("head_hits", static_cast<std::uint64_t>(view.head_hits),
                       observation::Unit::count)
               : absent("head_hits", "branch_not_reached",
                        observation::Availability::not_applicable),
        known("links_complete", links)};
    const std::array causes{query_, input, sub, state};
    (void)emit("sequence_candidate_visit", fields, causes);
  }
  void operator()(const SeqAnchorDecisionView &view) noexcept {
    using namespace sequence_detail;
    if (!observer_.on_fact)
      return;
    const bool current = view.frame == frame_ && view.sub_index < subs_.count;
    const auto sub = current ? subs_.at(view.sub_index) : std::nullopt;
    const auto state =
        current && state_matches_ ? states_.at(view.sub_index) : std::nullopt;
    const bool links = current && sub && (!state_matches_ || state);
    complete_ = complete_ && links;
    const bool related = view.related_sub_index != static_cast<std::size_t>(-1);
    const std::array fields{
        known("anchor_frame", static_cast<std::uint64_t>(view.frame),
              observation::Unit::emulation_frame),
        known("input_index", static_cast<std::uint64_t>(view.input_index),
              observation::Unit::count),
        known("sub_index", static_cast<std::uint64_t>(view.sub_index),
              observation::Unit::count),
        known("key", view.key),
        known("candidate_signature", view.candidate_signature),
        known("result", name(view.result)),
        related ? known("related_sub_index",
                        static_cast<std::uint64_t>(view.related_sub_index),
                        observation::Unit::count)
                : absent("related_sub_index", "branch_has_no_related_sequence",
                         observation::Availability::not_applicable),
        known("links_complete", links)};
    const std::array causes{query_, sub, state};
    (void)emit("sequence_candidate_decision", fields, causes);
  }
  void operator()(const SeqAnchorResolutionView &view) noexcept {
    using namespace sequence_detail;
    if (!observer_.on_fact)
      return;
    const bool current = view.frame == frame_ && query_.has_value();
    complete_ = complete_ && current;
    const std::array fields{
        known("anchor_frame", static_cast<std::uint64_t>(view.frame),
              observation::Unit::emulation_frame),
        known("result", name(view.result)),
        known("selected_count", static_cast<std::uint64_t>(view.selected_count),
              observation::Unit::count),
        known("query_linked", current)};
    const std::array causes{query_};
    (void)emit("sequence_selection", fields, causes);
  }
  [[nodiscard]] bool complete() const noexcept { return complete_; }

private:
  void members(std::span<const std::uint64_t> values, std::size_t sub,
               std::string_view kind) noexcept {
    using namespace sequence_detail;
    for (std::size_t i = 0; i < values.size(); ++i) {
      const std::array fields{known("sub_index",
                                    static_cast<std::uint64_t>(sub),
                                    observation::Unit::count),
                              known("list", kind),
                              known("list_index", static_cast<std::uint64_t>(i),
                                    observation::Unit::count),
                              known("signature", values[i])};
      const std::array causes{query_, subs_.at(sub)};
      (void)emit("sequence_signature", fields, causes);
    }
  }
  std::optional<observation::FactId>
  emit(std::string_view kind, std::span<const observation::FieldView> fields,
       std::span<const std::optional<observation::FactId>> parents) noexcept {
    const auto id = ids_.next_fact(Producer::selection);
    if (!id) {
      complete_ = false;
      return {};
    }
    std::array<observation::Cause, 4> causes{};
    std::size_t count = 0;
    for (const auto parent : parents)
      if (sequence_detail::valid(parent) && count < causes.size())
        causes[count++] = *parent;
    observer_.observe(observation::FactView{
        *id,
        kind,
        {observation::Availability::known, context_.session_frame, {}},
        std::span{causes}.first(count),
        {},
        fields});
    return id;
  }
  observation::Observer observer_;
  IdentitySource &ids_;
  SequenceContext context_;
  std::optional<observation::FactId> query_;
  sequence_detail::FactRun inputs_, subs_, states_;
  std::uint32_t frame_ = 0;
  bool state_matches_ = false;
  bool complete_ = true;
};
static_assert(sizeof(SequenceObservation) <= 256);
} // namespace ayther::audio_qa
