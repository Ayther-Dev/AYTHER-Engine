#include <ayther/audio_seq_anchor.h>

#include <array>
#include <iostream>

namespace {
struct Visits {
  std::array<ayther::SeqAnchorCandidateView, 16> rows{};
  std::size_t count = 0;
  void operator()(const ayther::SeqAnchorCandidateView &row) noexcept {
    if (count < rows.size())
      rows[count] = row;
    ++count;
  }
};
struct Decisions {
  std::array<ayther::SeqAnchorDecisionView, 16> rows{};
  ayther::SeqAnchorResolutionView terminal{};
  std::size_t count = 0;
  std::size_t terminals = 0;
  void decide(const ayther::SeqAnchorDecisionView &row) noexcept {
    if (count < rows.size())
      rows[count] = row;
    ++count;
  }
  void finish(const ayther::SeqAnchorResolutionView &row) noexcept {
    terminal = row;
    ++terminals;
  }
};

bool same_state(const std::vector<ayther::SeqAnchorState> &a,
                const std::vector<ayther::SeqAnchorState> &b) {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].next_free != b[i].next_free || a[i].win_start != b[i].win_start ||
        a[i].win_end != b[i].win_end || a[i].open != b[i].open)
      return false;
  }
  return true;
}

bool formation() {
  using Stage = ayther::SeqAnchorCandidateStage;
  std::vector<ayther::SeqAnchorSub> subs(5);
  for (std::size_t i = 0; i < subs.size(); ++i) {
    subs[i].key = i + 10;
    subs[i].duration_frames = 10;
    subs[i].trigger_signature = 90;
  }
  subs[0].enabled = false;
  subs[2].head = {7, 8, 9, 10, 11}; // One hit is below quorum.
  subs[3].trigger_signature = 7;
  std::vector<ayther::SeqAnchorState> state(5);
  state[1].next_free = 8;
  auto plain = state;
  Visits visits;
  const auto anchored =
      ayther::seq_anchor_frame_observed(5, {7}, subs, state, visits);
  if (anchored != std::vector<std::size_t>{3} ||
      anchored != ayther::seq_anchor_frame(5, {7}, subs, plain) ||
      !same_state(state, plain) || visits.count != 5)
    return false;
  constexpr std::array stages{Stage::disabled, Stage::internal, Stage::formed,
                              Stage::formed, Stage::not_head};
  for (std::size_t i = 0; i < stages.size(); ++i) {
    const auto &row = visits.rows[i];
    if (row.stage != stages[i] || row.sub_index != i ||
        row.key != subs[i].key || row.frame != 5 || row.signature != 7)
      return false;
  }
  return !visits.rows[2].trigger && visits.rows[2].head_hits == 1 &&
         visits.rows[3].trigger && visits.rows[3].head_hits == 1;
}

bool duplicates_and_claims() {
  using Stage = ayther::SeqAnchorCandidateStage;
  std::vector<ayther::SeqAnchorSub> subs(2);
  subs[0] = {10, 7, 10, 0, true, false, {7, 8, 9}, {7, 8}};
  subs[1] = {20, 8, 10, 0, true, false, {7, 8}, {7, 8}};
  std::vector<ayther::SeqAnchorState> state;
  auto plain = state;
  Visits visits;
  const std::vector<std::uint64_t> signatures{7, 7, 8};
  const auto anchored =
      ayther::seq_anchor_frame_observed(5, signatures, subs, state, visits);
  if (anchored != std::vector<std::size_t>{1} ||
      anchored != ayther::seq_anchor_frame(5, signatures, subs, plain) ||
      !same_state(state, plain) || visits.count != 6)
    return false;
  for (std::size_t i = 0; i < visits.count; ++i) {
    const auto &row = visits.rows[i];
    if (row.stage != (i < 2 ? Stage::formed : Stage::accumulated) ||
        row.head_hits != i / 2 + 1 || row.sub_index != i % 2)
      return false;
  }
  // Observer storage exhaustion does not bound the algorithm or suppress
  // visits.
  std::vector<std::uint64_t> repeated(64, 7);
  state.clear();
  plain.clear();
  Visits bounded;
  const auto observed =
      ayther::seq_anchor_frame_observed(5, repeated, subs, state, bounded);
  return bounded.count == 128 &&
         observed == ayther::seq_anchor_frame(5, repeated, subs, plain) &&
         same_state(state, plain);
}

bool empty_and_continuation() {
  std::vector<ayther::SeqAnchorSub> subs;
  std::vector<ayther::SeqAnchorState> state(2);
  Visits visits;
  if (!ayther::seq_anchor_frame_observed(0, {7}, subs, state, visits).empty() ||
      !state.empty() || visits.count != 0)
    return false;
  subs = {{10, 7, 5, 0, true, false, {7}, {7}},
          {20, 7, 5, 0, true, true, {7, 8}, {7}}};
  state = {{}, {5, 0, 5, true}};
  auto plain = state;
  const auto observed =
      ayther::seq_anchor_frame_observed(5, {7}, subs, state, visits);
  if (observed != std::vector<std::size_t>{1} ||
      observed != ayther::seq_anchor_frame(5, {7}, subs, plain) ||
      !same_state(state, plain))
    return false;
  Visits empty;
  return ayther::seq_anchor_frame_observed(6, {}, subs, state, empty).empty() &&
         empty.count == 0;
}

bool actual_decisions() {
  using Result = ayther::SeqAnchorDecisionResult;
  using Terminal = ayther::SeqAnchorResolutionResult;
  const std::vector<ayther::SeqAnchorSub> subs{
      {10, 99, 10, 0, true, false, {7, 8, 9}, {7, 8, 9}},
      {20, 7, 10, 0, true, false, {7, 8}, {7}},
  };
  std::vector<ayther::SeqAnchorState> state;
  auto plain = state;
  Visits visits;
  Decisions decisions;
  const auto decide = [&](const ayther::SeqAnchorDecisionView &row) noexcept {
    decisions.decide(row);
  };
  const auto finish = [&](const ayther::SeqAnchorResolutionView &row) noexcept {
    decisions.finish(row);
  };
  const auto selected = ayther::seq_anchor_frame_decided(
      5, {7}, subs, state, visits, decide, finish);
  if (selected != std::vector<std::size_t>{1} ||
      selected != ayther::seq_anchor_frame(5, {7}, subs, plain) ||
      !same_state(state, plain) || decisions.count != 2 ||
      decisions.terminals != 1 ||
      decisions.terminal.result != Terminal::selected ||
      decisions.terminal.selected_count != 1)
    return false;
  bool quorum = false;
  bool winner = false;
  for (std::size_t i = 0; i < decisions.count; ++i) {
    quorum = quorum || (decisions.rows[i].sub_index == 0 &&
                        decisions.rows[i].result == Result::quorum_rejected);
    winner = winner || (decisions.rows[i].sub_index == 1 &&
                        decisions.rows[i].result == Result::selected);
  }
  if (!quorum || !winner)
    return false;

  decisions = {};
  visits = {};
  state.clear();
  if (!ayther::seq_anchor_frame_decided(6, {}, subs, state, visits, decide,
                                        finish)
           .empty() ||
      decisions.count != 0 || decisions.terminals != 1 ||
      decisions.terminal.result != Terminal::no_candidate)
    return false;

  decisions = {};
  state = {{10, 0, 10, true}, {}};
  const std::vector<ayther::SeqAnchorSub> claimed{
      {10, 99, 10, 0, true, false, {7}, {99}},
      {20, 7, 10, 0, true, false, {7}, {7}},
  };
  const auto none = ayther::seq_anchor_frame_decided(5, {7}, claimed, state,
                                                     visits, decide, finish);
  return none.empty() && decisions.count == 1 && decisions.terminals == 1 &&
         decisions.rows[0].result == Result::claimed_by_open_sequence &&
         decisions.rows[0].related_sub_index == 0 &&
         decisions.terminal.result == Terminal::no_selection;
}
} // namespace

int main() {
  try {
    if (!formation() || !duplicates_and_claims() || !empty_and_continuation() ||
        !actual_decisions()) {
      std::cerr << "Sequence candidate observation failed\n";
      return 1;
    }
    std::cout << "Sequence candidate visits preserve anchors and state\n";
  } catch (...) {
    return 2;
  }
}
