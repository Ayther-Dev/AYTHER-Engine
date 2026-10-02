#include <ayther/audio_match_rule.h>

#include <array>
#include <iostream>

namespace {
struct Visits {
  std::array<ayther::AudioMatchIndex::CandidateView, 8> rows{};
  std::size_t count = 0;
  bool overflow = false;
  void operator()(const ayther::AudioMatchIndex::CandidateView &row) noexcept {
    if (count < rows.size())
      rows[count] = row;
    else
      overflow = true;
    ++count;
  }
};

struct Decisions {
  std::array<ayther::AudioMatchIndex::CandidateDecisionView, 8> candidates{};
  ayther::AudioMatchIndex::ResolutionView terminal{};
  std::size_t count = 0;
  std::size_t terminals = 0;
  void candidate(
      const ayther::AudioMatchIndex::CandidateDecisionView &row) noexcept {
    if (count < candidates.size())
      candidates[count] = row;
    ++count;
  }
  void finish(const ayther::AudioMatchIndex::ResolutionView &row) noexcept {
    terminal = row;
    ++terminals;
  }
};

bool actual_visits() {
  using Rule = ayther::AudioMatchRule;
  ayther::AudioMatchIndex index;
  index.add(90, Rule::kInstrument, 7, 255);
  index.add(80, Rule::kInstrumentPitch, 7, 61);
  index.add(50, Rule::kInstrumentPitch, 7, 60);
  index.add(20, Rule::kInstrumentPitch, 7, 60);
  index.add(10, Rule::kInstrument, 8, 255);
  index.add(1, Rule::kExact, 7, 60);
  Visits visits;
  std::uint64_t selected = 999;
  if (!index.resolve_observed({7, 60}, &selected, visits) || selected != 20 ||
      visits.count != 4 || visits.overflow)
    return false;
  std::array<bool, 4> seen{};
  for (std::size_t i = 0; i < visits.count; ++i) {
    const auto &row = visits.rows[i];
    const std::size_t slot = row.signature == 90   ? 0
                             : row.signature == 80 ? 1
                             : row.signature == 50 ? 2
                             : row.signature == 20 ? 3
                                                   : 4;
    if (slot >= seen.size() || seen[slot])
      return false;
    seen[slot] = true;
    if (row.rule != (slot == 0 ? Rule::kInstrument : Rule::kInstrumentPitch) ||
        row.pitch != (slot == 0   ? 255
                      : slot == 1 ? 61
                                  : 60))
      return false;
  }
  // A visited wrong-note entry is not a selected assignment. The callback has
  // no verdict or result field: only the production resolver writes `selected`.
  std::uint64_t plain = 999;
  if (!index.resolve(7, 60, &plain) || plain != selected)
    return false;
  visits = {};
  if (!index.resolve_observed({7, 62}, &selected, visits) || selected != 90 ||
      visits.count != 4)
    return false;
  visits = {};
  if (!index.resolve_observed({7, ayther::kAudioNoPitch}, nullptr, visits) ||
      visits.count != 4)
    return false;
  visits = {};
  selected = 999;
  if (index.resolve_observed({0, 60}, &selected, visits) || visits.count != 0 ||
      selected != 999)
    return false;
  if (index.resolve_observed({9, 60}, &selected, visits) || visits.count != 0 ||
      selected != 999)
    return false;
  index.clear();
  return !index.resolve_observed({7, 60}, &selected, visits) &&
         visits.count == 0;
}

bool duplicates_and_receiver_loss() {
  using Rule = ayther::AudioMatchRule;
  ayther::AudioMatchIndex index;
  index.add(4, Rule::kInstrumentPitch, 5, 60);
  index.add(4, Rule::kInstrumentPitch, 5, 60);
  std::uint64_t chosen = 777;
  Visits visits;
  if (index.resolve_observed({5, 61}, &chosen, visits) || visits.count != 2 ||
      chosen != 777)
    return false;
  if (visits.rows[0].signature != 4 || visits.rows[1].signature != 4)
    return false;
  for (unsigned i = 0; i < 64; ++i)
    index.add(100 + i, Rule::kInstrument, 5, 255);
  visits = {};
  if (!index.resolve_observed({5, 60}, &chosen, visits) || chosen != 4 ||
      visits.count != 66 || !visits.overflow)
    return false;
  std::uint64_t plain = 777;
  return index.resolve(5, 60, &plain) && plain == chosen;
}

bool actual_decisions() {
  using Index = ayther::AudioMatchIndex;
  using Rule = ayther::AudioMatchRule;
  Index index;
  index.add(10, Rule::kInstrumentPitch, 7, 63);
  index.add(20, Rule::kInstrumentPitch, 7, 64);
  index.add(15, Rule::kInstrumentPitch, 7, 64);
  Visits visits;
  Decisions decisions;
  std::uint64_t selected = UINT64_MAX;
  const auto decide = [&](const Index::CandidateDecisionView &row) noexcept {
    decisions.candidate(row);
  };
  const auto finish = [&](const Index::ResolutionView &row) noexcept {
    decisions.finish(row);
  };
  if (!index.resolve_decided({7, 64}, &selected, visits, decide, finish) ||
      selected != 15 || visits.count != 3 || decisions.count != 3 ||
      decisions.terminals != 1 ||
      decisions.terminal.result != Index::ResolutionResult::selected ||
      decisions.terminal.selected_signature != 15 ||
      decisions.terminal.selected_rank != 1)
    return false;
  std::size_t rejected = 0;
  std::size_t updated = 0;
  for (std::size_t i = 0; i < decisions.count; ++i) {
    const auto &row = decisions.candidates[i];
    rejected += row.result == Index::CandidateResult::pitch_rejected ? 1 : 0;
    updated += row.result == Index::CandidateResult::winner_updated ? 1 : 0;
  }
  if (rejected != 1 || updated != 2)
    return false;

  visits = {};
  decisions = {};
  selected = UINT64_MAX;
  if (index.resolve_decided({7, 62}, &selected, visits, decide, finish) ||
      selected != UINT64_MAX || visits.count != 3 || decisions.count != 3 ||
      decisions.terminals != 1 ||
      decisions.terminal.result !=
          Index::ResolutionResult::no_applicable_candidate)
    return false;

  decisions = {};
  if (index.resolve_decided({0, 62}, &selected, visits, decide, finish) ||
      decisions.count != 0 || decisions.terminals != 1 ||
      decisions.terminal.result != Index::ResolutionResult::invalid_instrument)
    return false;
  index.clear();
  decisions = {};
  return !index.resolve_decided({7, 62}, &selected, visits, decide, finish) &&
         decisions.terminals == 1 &&
         decisions.terminal.result == Index::ResolutionResult::empty_index;
}
} // namespace

int main() try {
  if (!actual_visits() || !duplicates_and_receiver_loss() ||
      !actual_decisions()) {
    std::cerr << "candidate visitation failed\n";
    return 1;
  }
  return 0;
} catch (...) {
  return 2;
}
