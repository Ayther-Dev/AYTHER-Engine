#include "audio_sequence_observation.h"

#include <array>
#include <iostream>
#include <memory>

namespace qa = ayther::audio_qa;
namespace obs = ayther::engine::audio_observation;
namespace {
struct Row {
  obs::FactId id;
  std::string_view kind;
  std::array<obs::FactId, 4> causes{};
  std::size_t count = 0;
  std::uint64_t index = 0;
  std::uint64_t input = 0;
  std::uint64_t hits = 0;
  std::string_view stage;
  std::string_view result;
  obs::Availability trigger = obs::Availability::unknown;
  bool provenance = false;
  bool links = false;
};
struct Sink {
  std::array<Row, 64> rows{};
  std::size_t count = 0;
  std::uint64_t base = 0;
  bool valid = true;
  static void receive(void *context, const obs::FactView &fact) noexcept {
    auto &s = *static_cast<Sink *>(context);
    const auto i = s.count++;
    if (i >= s.rows.size())
      return;
    auto &r = s.rows[i];
    r.id = fact.id;
    r.kind = fact.kind;
    if (fact.id.producer != 5 || fact.id.sequence != s.base + i + 1 ||
        fact.causes.size() > 4)
      s.valid = false;
    for (const auto &cause : fact.causes) {
      if (const auto *id = std::get_if<obs::FactId>(&cause); id && r.count < 4)
        r.causes[r.count++] = *id;
      else
        s.valid = false;
    }
    for (const auto &field : fact.fields) {
      if (const auto *v = std::get_if<bool>(&field.value)) {
        if (field.name == "provenance_complete")
          r.provenance = *v;
        if (field.name == "links_complete")
          r.links = *v;
      }
      if (field.name == "trigger")
        r.trigger = field.availability;
      if (const auto *v = std::get_if<std::uint64_t>(&field.value)) {
        if (field.name == "sub_index")
          r.index = *v;
        if (field.name == "input_index")
          r.input = *v;
        if (field.name == "head_hits")
          r.hits = *v;
      }
      if (field.name == "stage")
        if (const auto *v = std::get_if<std::string_view>(&field.value))
          r.stage = *v;
      if (field.name == "result")
        if (const auto *v = std::get_if<std::string_view>(&field.value))
          r.result = *v;
    }
  }
  obs::Observer observer() { return {this, receive, nullptr}; }
};

bool actual_frame() {
  Sink sink;
  qa::IdentitySource ids;
  // The transformation identity belongs to a previously emitted selection fact.
  for (unsigned i = 0; i < 90; ++i)
    (void)ids.next_fact(qa::Producer::selection);
  sink.base = 90;
  qa::SequenceObservation trace(
      sink.observer(), ids,
      {30, qa::SequenceUse::live_pack, obs::FactId{4, 9}});
  const std::vector<std::uint64_t> inputs{7, 7};
  const std::vector<ayther::SeqAnchorSub> subs{
      {10, 7, 8, 5, true, true, {7, 7}, {7, 8}},
      {20, 7, 1, 0, false, false, {}, {}}};
  std::vector<ayther::SeqAnchorState> states(2);
  const std::array<qa::SequenceInputSource, 2> origins{
      {{obs::FactId{4, 10}, {}, 0, 7, "detector_signature"},
       {obs::FactId{4, 11}, obs::FactId{5, 90}, 1, 77, "resolved_assignment"}}};
  trace.begin({30, inputs, subs, states, {}}, origins);
  const auto result =
      ayther::seq_anchor_frame_observed(30, inputs, subs, states, trace);
  if (!trace.complete() || !sink.valid || sink.count != 15 ||
      result != std::vector<std::size_t>{0})
    return false;
  if (sink.rows[0].kind != "sequence_query" ||
      sink.rows[1].kind != "sequence_input" || sink.rows[2].count != 3 ||
      sink.rows[2].causes[2] != obs::FactId{5, 90})
    return false;
  for (std::size_t i = 11; i < 15; ++i) {
    const auto &r = sink.rows[i];
    const auto input = (i - 11) / 2;
    const auto sub = (i - 11) % 2;
    if (r.kind != "sequence_candidate_visit" || r.input != input ||
        r.index != sub || r.count != 4 || r.causes[0] != sink.rows[0].id ||
        r.causes[1] != sink.rows[1 + input].id ||
        r.causes[2] != sink.rows[3 + sub].id ||
        r.causes[3] != sink.rows[5 + sub].id)
      return false;
    if (sub == 1 && (r.stage != "disabled" ||
                     r.trigger != obs::Availability::not_applicable))
      return false;
    if (sub == 0 &&
        (r.hits != input + 1 || r.stage != (input ? "accumulated" : "formed")))
      return false;
  }
  return true;
}

bool replay_and_bounds() {
  Sink sink;
  qa::IdentitySource ids;
  qa::SequenceObservation trace(
      sink.observer(), ids,
      {1, qa::SequenceUse::replay_table, obs::FactId{4, 8}});
  const std::vector<ayther::SeqAnchorSub> subs{
      {10, 7, 1, 0, true, false, {}, {}}};
  const auto anchors = ayther::seq_anchor_table_observed(
      1025, [](std::size_t) { return std::uint64_t{7}; },
      [](std::size_t) { return 0u; }, subs,
      [&](const ayther::SeqAnchorFrameView &view) noexcept {
        trace.begin(view);
      },
      trace);
  if (!trace.complete() || !sink.valid || sink.count != 2053 ||
      anchors.at(10) != std::vector<std::uint32_t>{0})
    return false;
  qa::SequenceObservation absent(sink.observer(), ids,
                                 {1, qa::SequenceUse::live_authored, {}});
  std::vector<ayther::SeqAnchorState> state;
  const std::vector<std::uint64_t> inputs{7};
  absent.begin({1, inputs, subs, state, {}});
  (void)ayther::seq_anchor_frame_observed(1, inputs, subs, state, absent);
  if (absent.complete())
    return false;
  qa::IdentitySource off_ids;
  qa::SequenceObservation off({}, off_ids, {1, qa::SequenceUse::live_pack, {}});
  off.begin({1, inputs, subs, state, {}});
  (void)ayther::seq_anchor_frame_observed(1, inputs, subs, state, off);
  return off.complete() &&
         off_ids.next_fact(qa::Producer::selection) == obs::FactId{5, 1};
}
bool fact_runs() {
  qa::sequence_detail::FactRun run;
  if (!run.append(obs::FactId{5, UINT64_MAX - 1}) ||
      !run.append(obs::FactId{5, UINT64_MAX}) ||
      run.at(1) != obs::FactId{5, UINT64_MAX} || run.at(2))
    return false;
  if (run.append(obs::FactId{5, 1}) || run.at(0))
    return false;
  run = {};
  if (run.append({}) || run.at(0))
    return false;
  run = {};
  if (!run.append(obs::FactId{5, 2}) || run.append(obs::FactId{5, 4}) ||
      run.at(0))
    return false;
  run = {};
  return run.append(obs::FactId{5, 2}) && !run.append(obs::FactId{4, 3}) &&
         !run.at(0);
}
bool malformed_provenance() {
  Sink sink;
  qa::IdentitySource ids;
  qa::SequenceObservation trace(
      sink.observer(), ids,
      {1, qa::SequenceUse::live_authored, obs::FactId{4, 1}});
  const std::vector<std::uint64_t> inputs{7};
  const std::vector<ayther::SeqAnchorSub> subs{
      {10, 7, 1, 0, true, false, {}, {}}};
  std::vector<ayther::SeqAnchorState> states(1);
  const std::array<qa::SequenceInputSource, 1> origins{
      {{obs::FactId{4, 2}, {}, 0, 77, "resolved_assignment"}}};
  trace.begin({1, inputs, subs, states, {}}, origins);
  if (trace.complete() || sink.rows[1].provenance)
    return false;
  // A callback from another frame must not acquire links to this frame's rows.
  trace({2, 7, 0, 10, ayther::SeqAnchorCandidateStage::formed, true, 1, 0});
  const auto &bad = sink.rows[sink.count - 1];
  if (bad.count != 1 || bad.links)
    return false;
  const auto before = sink.count;
  const std::array<std::size_t, 2> wrong_indices{0, 1};
  trace.begin({3, inputs, subs, states, wrong_indices});
  return !trace.complete() && sink.rows[before + 1].count == 1 &&
         !sink.rows[before + 1].provenance && sink.valid;
}
bool origins_and_transformations() {
  auto origins = std::make_unique<qa::SequenceOrigins>();
  const qa::SequenceInputSource origin{
      obs::FactId{4, 1}, {}, 0, 7, "detector_signature"};
  for (std::size_t i = 0; i < qa::SequenceOrigins::capacity; ++i)
    origins->append(origin);
  if (!origins->complete() ||
      origins->view().size() != qa::SequenceOrigins::capacity)
    return false;
  origins->append(origin);
  if (origins->complete() ||
      origins->view().size() != qa::SequenceOrigins::capacity ||
      origins->view().front().signature != 7)
    return false;
  origins->clear();
  if (!origins->complete() || !origins->view().empty())
    return false;
  Sink sink;
  qa::IdentitySource ids;
  const auto transformed = qa::observe_sequence_transformation(
      sink.observer(), ids,
      {1, obs::FactId{4, 1}, obs::FactId{4, 2}, 7, 8, "resolved_assignment",
       8});
  if (!transformed || sink.rows[0].kind != "sequence_signature_transform" ||
      sink.rows[0].count != 2 || sink.rows[0].causes[0] != obs::FactId{4, 1} ||
      sink.rows[0].causes[1] != obs::FactId{4, 2})
    return false;
  qa::SequenceObservation failed(
      sink.observer(), ids,
      {1, qa::SequenceUse::live_pack, obs::FactId{4, 1}, "allocation_failed"});
  failed.begin({});
  if (failed.complete())
    return false;
  qa::IdentitySource off_ids;
  return !qa::observe_sequence_transformation({}, off_ids, {}) &&
         off_ids.next_fact(qa::Producer::selection) == obs::FactId{5, 1};
}
bool decisions_and_terminal() {
  Sink sink;
  qa::IdentitySource ids;
  qa::SequenceObservation trace(
      sink.observer(), ids, {5, qa::SequenceUse::live_pack, obs::FactId{4, 1}});
  const std::vector<std::uint64_t> inputs{7};
  const std::vector<ayther::SeqAnchorSub> subs{
      {10, 99, 10, 0, true, false, {7, 8, 9}, {7, 8, 9}},
      {20, 7, 10, 0, true, false, {7, 8}, {7}},
  };
  std::vector<ayther::SeqAnchorState> states;
  const std::array<qa::SequenceInputSource, 1> origins{
      {{obs::FactId{4, 2}, {}, 0, 7, "detector_signature"}}};
  trace.begin({5, inputs, subs, states, {}}, origins);
  const auto selected = ayther::seq_anchor_frame_decided(
      5, inputs, subs, states, trace, trace, trace);
  if (selected != std::vector<std::size_t>{1} || !trace.complete() ||
      !sink.valid)
    return false;
  std::size_t decisions = 0;
  std::size_t terminals = 0;
  bool quorum = false;
  bool winner = false;
  for (std::size_t i = 0; i < sink.count; ++i) {
    const auto &row = sink.rows[i];
    if (row.kind == "sequence_candidate_decision") {
      ++decisions;
      quorum = quorum || row.result == "quorum_rejected";
      winner = winner || row.result == "selected";
      if (row.count != 2)
        return false;
    }
    if (row.kind == "sequence_selection") {
      ++terminals;
      if (row.result != "selected" || row.count != 1 ||
          row.causes[0] != sink.rows[0].id)
        return false;
    }
  }
  return decisions == 2 && terminals == 1 && quorum && winner;
}
} // namespace
int main() {
  try {
    if (!actual_frame() || !replay_and_bounds() || !fact_runs() ||
        !malformed_provenance() || !origins_and_transformations() ||
        !decisions_and_terminal()) {
      std::cerr << "Sequence fact observation failed\n";
      return 1;
    }
  } catch (...) {
    return 2;
  }
}
