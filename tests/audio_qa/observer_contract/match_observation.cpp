#include "audio_match_observation.h"

#include <array>
#include <iostream>

namespace qa = ayther::audio_qa;
namespace obs = ayther::engine::audio_observation;

namespace {
struct Row {
  obs::FactId id;
  std::string_view kind;
  std::array<obs::FactId, 3> causes{};
  std::size_t causes_count = 0;
  std::uint64_t signature = 0;
  std::uint64_t ordinal = 0;
  std::uint64_t pitch = 0;
  std::uint64_t frame = 0;
  bool found = false;
  bool provenance = false;
  obs::Availability source = obs::Availability::not_applicable;
  obs::Availability pitch_availability = obs::Availability::unknown;
  std::string_view use;
  std::string_view rule;
  std::string_view result;
  std::string_view branch;
  std::uint64_t selected_signature = 0;
  bool matched = false;
};
struct Sink {
  std::array<Row, 32> rows{};
  std::size_t count = 0;
  bool valid = true;
  static void receive(void *context, const obs::FactView &fact) noexcept {
    auto &self = *static_cast<Sink *>(context);
    const auto slot = self.count++;
    if (slot >= self.rows.size())
      return;
    auto &row = self.rows[slot];
    row.id = fact.id;
    row.kind = fact.kind;
    row.frame = fact.frame.emulation_frame;
    if (fact.id.producer != 5 || fact.id.sequence != slot + 1 ||
        !fact.state_orders.empty() || fact.causes.size() > 3 ||
        (fact.kind != "assignment_query" &&
         fact.kind != "assignment_exact_probe" &&
         fact.kind != "assignment_candidate" &&
         fact.kind != "assignment_candidate_decision" &&
         fact.kind != "assignment_selection"))
      self.valid = false;
    for (const auto &cause : fact.causes) {
      if (const auto *id = std::get_if<obs::FactId>(&cause);
          id && row.causes_count < row.causes.size())
        row.causes[row.causes_count++] = *id;
      else
        self.valid = false;
    }
    for (const auto &field : fact.fields) {
      if (field.name == "source")
        row.source = field.availability;
      if (field.name == "pitch")
        row.pitch_availability = field.availability;
      if (const auto *value = std::get_if<std::uint64_t>(&field.value)) {
        if (field.name == "signature")
          row.signature = *value;
        if (field.name == "visit_index")
          row.ordinal = *value;
        if (field.name == "pitch")
          row.pitch = *value;
        if (field.name == "selected_signature")
          row.selected_signature = *value;
      }
      if (const auto *value = std::get_if<bool>(&field.value)) {
        if (field.name == "found")
          row.found = *value;
        if (field.name == "provenance_complete")
          row.provenance = *value;
        if (field.name == "matched")
          row.matched = *value;
      }
      if (const auto *value = std::get_if<std::string_view>(&field.value)) {
        if (field.name == "use")
          row.use = *value;
        if (field.name == "rule")
          row.rule = *value;
        if (field.name == "result")
          row.result = *value;
        if (field.name == "branch")
          row.branch = *value;
      }
    }
  }
  obs::Observer observer() { return {this, receive, nullptr}; }
};

bool exact_queries() {
  Sink sink;
  qa::IdentitySource ids;
  qa::MatchQuery query{
      12, 99, 77, 60, qa::MatchUse::live_assignment, obs::FactId{4, 8}};
  qa::MatchObservation first(sink.observer(), ids, query);
  first.exact_probe(true);
  if (!first.complete() || sink.count != 3 || !sink.valid)
    return false;
  const auto &begin = sink.rows[0];
  const auto &probe = sink.rows[1];
  const auto &candidate = sink.rows[2];
  if (begin.kind != "assignment_query" || begin.use != "live_assignment" ||
      begin.source != obs::Availability::known || !begin.provenance ||
      begin.frame != 12 || begin.causes[0] != obs::FactId{4, 8} ||
      begin.causes_count != 1 || probe.kind != "assignment_exact_probe" ||
      !probe.found || probe.causes[0] != begin.id ||
      candidate.kind != "assignment_candidate" || candidate.causes_count != 2 ||
      candidate.causes[0] != begin.id || candidate.causes[1] != probe.id ||
      candidate.signature != 99 || candidate.rule != "exact" ||
      candidate.pitch_availability != obs::Availability::not_applicable)
    return false;
  qa::MatchObservation second(sink.observer(), ids, query);
  second.exact_probe(false);
  return sink.count == 5 && sink.rows[3].id != begin.id &&
         sink.rows[4].kind == "assignment_exact_probe" && !sink.rows[4].found;
}

bool instrument_visits() {
  using Rule = ayther::AudioMatchRule;
  ayther::AudioMatchIndex index;
  index.add(10, Rule::kInstrumentPitch, 9, 60);
  index.add(20, Rule::kInstrumentPitch, 9, 61);
  index.add(30, Rule::kInstrument, 9, 255);
  Sink sink;
  qa::IdentitySource ids;
  qa::MatchObservation query(
      sink.observer(), ids,
      {7, 88, 9, 60, qa::MatchUse::live_sequence_interest, obs::FactId{4, 9}});
  query.exact_probe(false);
  std::uint64_t selected = 0;
  if (!index.resolve_observed({9, 60}, &selected, query) || selected != 10 ||
      sink.count != 5 || !query.complete() || !sink.valid)
    return false;
  bool wrong_note = false;
  for (std::size_t i = 2; i < sink.count; ++i) {
    const auto &row = sink.rows[i];
    if (row.kind != "assignment_candidate" || row.ordinal != i - 2 ||
        row.causes_count != 2 || row.causes[0] != sink.rows[0].id ||
        row.causes[1] != sink.rows[1].id)
      return false;
    wrong_note = wrong_note || (row.signature == 20 && row.pitch == 61);
  }
  // Discarding records does not feed back into matching or cap callback count.
  for (std::uint64_t i = 100; i < 164; ++i)
    index.add(i, Rule::kInstrument, 9, 255);
  qa::MatchObservation repeated(
      sink.observer(), ids,
      {8, 88, 9, 60, qa::MatchUse::live_assignment, obs::FactId{4, 10}});
  repeated.exact_probe(false);
  std::uint64_t plain = 0;
  return wrong_note && index.resolve_observed({9, 60}, &selected, repeated) &&
         index.resolve(9, 60, &plain) && selected == plain && selected == 10 &&
         repeated.complete() && sink.count == 74;
}

bool absent_and_disabled() {
  Sink sink;
  qa::IdentitySource ids;
  qa::MatchQuery input{0, 0, 0, 255, qa::MatchUse::replay_trigger, {}};
  qa::MatchObservation absent(sink.observer(), ids, input);
  absent.exact_probe(false);
  if (absent.complete() || sink.rows[0].source != obs::Availability::unknown ||
      sink.rows[0].causes_count != 0 || sink.rows[0].provenance)
    return false;
  input.source = obs::FactId{0, 10};
  qa::MatchObservation invalid(sink.observer(), ids, input);
  if (invalid.complete() || sink.rows[2].causes_count != 0 ||
      sink.rows[2].source != obs::Availability::unknown)
    return false;
  qa::IdentitySource disabled_ids;
  qa::MatchObservation disabled({}, disabled_ids, input);
  disabled.exact_probe(true);
  disabled(ayther::AudioMatchIndex::CandidateView{
      10, ayther::AudioMatchRule::kInstrument, 255});
  return disabled.complete() && !disabled.id() &&
         disabled_ids.next_fact(qa::Producer::selection) == obs::FactId{5, 1};
}
bool contexts_and_missing_probe() {
  constexpr std::array uses{
      qa::MatchUse::live_assignment,    qa::MatchUse::live_sequence_interest,
      qa::MatchUse::live_voice_route,   qa::MatchUse::replay_trigger,
      qa::MatchUse::replay_voice_route, qa::MatchUse::asset_coverage,
      qa::MatchUse::bare_frame_mute,    qa::MatchUse::export_audio};
  constexpr std::array<std::string_view, 8> names{
      "live_assignment",    "live_sequence_interest",
      "live_voice_route",   "replay_trigger",
      "replay_voice_route", "asset_coverage",
      "bare_frame_mute",    "export_audio"};
  Sink sink;
  qa::IdentitySource ids;
  for (std::size_t i = 0; i < uses.size(); ++i) {
    qa::MatchObservation query(sink.observer(), ids,
                               {0, 7, 9, 255, uses[i], obs::FactId{4, 1}});
    query(ayther::AudioMatchIndex::CandidateView{
        8, ayther::AudioMatchRule::kInstrument, 255});
    if (query.complete() || sink.rows[2 * i].use != names[i] ||
        sink.rows[2 * i + 1].causes_count != 1 ||
        sink.rows[2 * i + 1].causes[0] != *query.id())
      return false;
  }
  return sink.valid && sink.count == 16;
}
bool decisions_and_terminal() {
  using Index = ayther::AudioMatchIndex;
  using Rule = ayther::AudioMatchRule;
  Index index;
  index.add(10, Rule::kInstrumentPitch, 7, 63);
  index.add(20, Rule::kInstrumentPitch, 7, 64);
  index.add(15, Rule::kInstrumentPitch, 7, 64);
  Sink sink;
  qa::IdentitySource ids;
  qa::MatchObservation trace(
      sink.observer(), ids,
      {5, 1000, 7, 64, qa::MatchUse::live_assignment, obs::FactId{4, 1}});
  trace.exact_probe(false);
  std::uint64_t selected = UINT64_MAX;
  if (!index.resolve_decided({7, 64}, &selected, trace, trace, trace) ||
      selected != 15 || !trace.complete() || !sink.valid || sink.count != 9)
    return false;
  std::size_t candidates = 0;
  std::size_t decisions = 0;
  std::size_t rejected = 0;
  std::size_t updated = 0;
  for (std::size_t i = 0; i < sink.count; ++i) {
    candidates += sink.rows[i].kind == "assignment_candidate" ? 1 : 0;
    decisions += sink.rows[i].kind == "assignment_candidate_decision" ? 1 : 0;
    rejected += sink.rows[i].result == "pitch_rejected" ? 1 : 0;
    updated += sink.rows[i].result == "winner_updated" ? 1 : 0;
  }
  if (candidates != 3 || decisions != 3 ||
      sink.rows[8].kind != "assignment_selection" || rejected != 1 ||
      updated != 2 || sink.rows[8].result != "selected" ||
      !sink.rows[8].matched || sink.rows[8].selected_signature != 15 ||
      sink.rows[8].branch != "instrument_index" ||
      sink.rows[8].causes_count != 3 ||
      sink.rows[8].causes[0] != sink.rows[0].id ||
      sink.rows[8].causes[1] != sink.rows[1].id)
    return false;

  Sink miss_sink;
  qa::IdentitySource miss_ids;
  qa::MatchObservation miss(
      miss_sink.observer(), miss_ids,
      {5, 1000, 7, 62, qa::MatchUse::live_assignment, obs::FactId{4, 3}});
  miss.exact_probe(false);
  selected = UINT64_MAX;
  if (index.resolve_decided({7, 62}, &selected, miss, miss, miss) ||
      selected != UINT64_MAX || !miss.complete() || !miss_sink.valid ||
      miss_sink.count != 9 ||
      miss_sink.rows[8].kind != "assignment_selection" ||
      miss_sink.rows[8].result != "no_applicable_candidate" ||
      miss_sink.rows[8].matched || miss_sink.rows[8].selected_signature != 0 ||
      miss_sink.rows[8].branch != "instrument_index")
    return false;

  Sink exact_sink;
  qa::IdentitySource exact_ids;
  qa::MatchObservation exact(
      exact_sink.observer(), exact_ids,
      {6, 99, 7, 64, qa::MatchUse::replay_trigger, obs::FactId{4, 2}});
  exact.exact_probe(true);
  exact.exact_selected();
  return exact.complete() && exact_sink.valid && exact_sink.count == 4 &&
         exact_sink.rows[3].kind == "assignment_selection" &&
         exact_sink.rows[3].result == "selected" &&
         exact_sink.rows[3].matched &&
         exact_sink.rows[3].selected_signature == 99 &&
         exact_sink.rows[3].branch == "exact_lookup" &&
         exact_sink.rows[3].causes_count == 3 &&
         exact_sink.rows[3].causes[2] == exact_sink.rows[2].id;
}
} // namespace

int main() {
  try {
    if (!exact_queries() || !instrument_visits() || !absent_and_disabled() ||
        !contexts_and_missing_probe() || !decisions_and_terminal()) {
      std::cerr << "Match observation contract failed\n";
      return 1;
    }
    std::cout << "Match query and candidate facts verified\n";
  } catch (...) {
    return 2;
  }
}
