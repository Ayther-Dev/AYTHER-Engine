#pragma once
// ---------------------------------------------------------------------------
// audio_seq_anchor.h — replay Sequence anchors with CLAIMS between Sequences.
// Header-only and pure: testable without SDL, a core, or a GPU.
//
// A Sequence (substitution) opens a window at every occurrence of its TRIGGER
// signature among the events detected in the take. Rules:
//
//  1. Greedy segmentation (2026-07-23 report): the step is the event SPAN. A
//     trigger occurrence inside the previous window's step is INTERNAL (the
//     melody repeats its first note) and does not re-anchor. A REAL repetition
//     after the step does re-anchor and retrigger.
//
//  2. CLAIM (2026-08-21 report): an occurrence of S's trigger inside ANOTHER
//     Sequence T's window (with HD), where T contains that signature as a
//     MEMBER, belongs to T. S neither anchors nor triggers: "the one already
//     playing wins." In Golden Axe, "The Battle - Intro" and "- Loop" share
//     26 signatures; the hi-hat that opens the Intro reappears every 63 frames
//     inside the Loop, while the bass that opens the Loop appears in the Intro,
//     causing both to play at once.
//
//  3. HEAD (2026-08-21 report, second pass): a lone trigger is fragile. On the
//     third Loop pass, the opening bass uses ANOTHER signature (a variant),
//     while the other five channels start identically. Without the trigger,
//     the Loop failed to re-anchor, its window expired, and the Intro leaked in
//     (intro, loop, intro, loop...). The head is the set of signatures that
//     start on the SAME frame as the trigger. A Sequence also anchors when a
//     MAJORITY of its head starts (>= ceil(n/2), with n >= 2), even without the
//     trigger.
//
//  4. Frame tie (two Sequences start on the SAME frame): CONTINUATION wins
//     first—a looping Sequence whose step ends on that frame while its window
//     is still alive continues ("always keep the one already playing unless it
//     ended or the events changed"; the window may outlast the step when the
//     HD asset is longer than the loop, so the test is "step reached", not
//     "window ends exactly here"). Next, a CLAIMER wins over the Sequence it
//     claims: when A lists B's candidate signature as trigger or member and B
//     does not list A's, A anchors first and its fresh window claims B. Golden
//     Axe, 2026-10-14: the intro's hi-hat is one of the loop's six head
//     signatures; at the intro→loop boundary both fired, the intro (fewer
//     members) was taken as "more specific", anchored first, and replaced the
//     loop that had just started — the HD intro played again at every loop
//     pass. Only then comes the most SPECIFIC Sequence (fewest member
//     signatures), then ID as a deterministic tie-breaker.
//
//  5. RESTART (2026-10-14, Golden Axe title→selection): an occurrence inside
//     the current step is internal and ignored — a loop's own bars repeat its
//     signatures — UNLESS it is the WHOLE opening: the trigger AND every other
//     head signature start together before the step is due, and nothing the
//     Sequence claims is still sounding from an earlier frame. That is the
//     game jumping back to the start of the music (screen change, retry), not
//     a bar of the loop, so the Sequence re-anchors there and its step
//     restarts. A head quorum was not enough: Wilderness repeats three of its
//     five opening notes every few bars, and each repeat cut the HD loop and
//     started it again from zero. A lone trigger inside the step stays internal
//     (the intro's hi-hat recurs three times inside the intro), as does a
//     Sequence without a head.
//
//  6. CONTRADICTION (2026-10-17, Golden Axe title→selection, detector
//     revision 3): the game reaches the loop point of "The Battle" while the
//     title fades out, so the Loop anchors; 55 frames later the selection
//     screen restarts the music from the INTRO, whose lone hi-hat trigger is a
//     member of the open Loop and was simply claimed — the HD kept playing the
//     loop over the game's intro and ended 55 frames early. Rule 5 cannot see
//     it: the Intro has no other head. Timing cannot either: with revision 3
//     the lead notes that differed only by volume share one signature and the
//     Loop has it 54 frames into a pass. What gives the restart away is the
//     VOICE STATE: the driver cut every voice one frame earlier, so nothing
//     the Loop claims is still sounding, while inside a loop bar the bass
//     sustains. So an OPEN exclusive Sequence is contradicted on a frame where
//     none of its claimed signatures is sustained from the previous frame AND
//     the frame carries the whole opening of ANOTHER enabled exclusive
//     Sequence (its trigger and every other head signature; a lone trigger is
//     a whole opening when the head is empty). The window closes on that frame
//     and the frame resolves as if it had never been open, so the Intro
//     anchors in sync and its step ends exactly where the loop begins. Only
//     with voice state (live paths); the table relies on the whole head.
//     Effects never contradict music and are never contradicted.
//
// Events are traversed in ascending `start_frame` order. The detector does NOT
// return them sorted (it emits them per channel), so the table applies a stable
// sort. The same traversal drives playback, bare-frame muting, and export
// mixdown: ONE table and one policy.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ayther {

/// Minimal view of a substitution for anchoring (copied from AudioSeqSub: the
/// tests do not need the whole session).
struct SeqAnchorSub {
  uint64_t key = 0;
  uint64_t trigger_signature = 0;
  uint32_t duration_frames = 1;     ///< window (with the HD)
  uint32_t span_frames = 0;         ///< segmentation step (0 = duration)
  bool enabled = true;              ///< asset assigned
  bool looping = false;             ///< looping HD (continuation)
  std::vector<uint64_t> signatures; ///< member signatures
  std::vector<uint64_t> head;       ///< signatures starting with the trigger
  /// Rule 6 runs between EXCLUSIVE Sequences only (music / ambience): an
  /// effect that borrows a music signature never closes the music window.
  bool exclusive = true;
};

/// How many head signatures are needed to anchor without the trigger?
/// 0 = never (a single-signature head: the trigger only).
inline size_t seq_head_quorum(const SeqAnchorSub &s) {
  return s.head.size() >= 2 ? (s.head.size() + 1) / 2 : 0;
}

/// Does `claimer` claim an occurrence of `sig`? Yes if it is its trigger or a
/// member signature.
inline bool seq_sub_claims(const SeqAnchorSub &claimer, uint64_t sig) {
  if (claimer.trigger_signature == sig)
    return true;
  return std::find(claimer.signatures.begin(), claimer.signatures.end(), sig) !=
         claimer.signatures.end();
}

/// Rule 6: is this frame the opening of `s` — its trigger and every other
/// head signature start now, and nothing it claims is sustained? Unlike Rule
/// 5's whole opening, a lone trigger counts when the head is empty: here the
/// question is not "did MY music restart" but "did ANOTHER music start".
inline bool seq_opening_now(const SeqAnchorSub &s,
                            std::span<const uint64_t> sigs,
                            std::span<const uint64_t> sustained) {
  if (std::find(sigs.begin(), sigs.end(), s.trigger_signature) == sigs.end())
    return false;
  for (const uint64_t head : s.head)
    if (head != s.trigger_signature &&
        std::find(sigs.begin(), sigs.end(), head) == sigs.end())
      return false;
  for (const uint64_t sig : sustained)
    if (seq_sub_claims(s, sig))
      return false;
  return true;
}

/// Is this frame the WHOLE opening of `s` (Rule 5)? The trigger and every
/// other head signature start now (`sigs`), the head has at least one
/// signature besides the trigger (a lone trigger recurs inside its own bars),
/// and no signature the Sequence claims is still sounding from an earlier
/// frame (`sustained`: active now and already active last frame).
inline bool seq_whole_opening(const SeqAnchorSub &s,
                              std::span<const uint64_t> sigs,
                              std::span<const uint64_t> sustained) {
  if (std::find(sigs.begin(), sigs.end(), s.trigger_signature) == sigs.end())
    return false;
  size_t others = 0;
  for (const uint64_t head : s.head) {
    if (head == s.trigger_signature)
      continue;
    ++others;
    if (std::find(sigs.begin(), sigs.end(), head) == sigs.end())
      return false;
  }
  if (others == 0)
    return false;
  for (const uint64_t sig : sustained)
    if (seq_sub_claims(s, sig))
      return false;
  return true;
}

/// Priority on a frame tie (without continuation): the most specific one.
inline bool seq_sub_before(const SeqAnchorSub &a, const SeqAnchorSub &b) {
  if (a.signatures.size() != b.signatures.size())
    return a.signatures.size() < b.signatures.size();
  return a.key < b.key;
}

/// Per-substitution state across frames (replay: local to the table; live: the
/// session holds it and keeps it in sync with its windows).
struct SeqAnchorState {
  uint32_t next_free = 0; ///< segmentation step: before this = internal
  uint32_t win_start = 0, win_end = 0; ///< current window [start, end)
  bool open = false;
};

/// Actual branches encountered while forming candidates; none is an anchor
/// verdict.
enum class SeqAnchorCandidateStage : uint8_t {
  disabled,
  internal,
  not_head,
  formed,
  accumulated
};

/// Borrowed observation of one signature/substitution visit. trigger/head_hits
/// describe a match only for formed/accumulated; other stages did not reach it.
struct SeqAnchorCandidateView {
  uint32_t frame = 0;
  uint64_t signature = 0;
  size_t sub_index = 0;
  uint64_t key = 0;
  SeqAnchorCandidateStage stage = SeqAnchorCandidateStage::disabled;
  bool trigger = false;
  size_t head_hits = 0;
  size_t input_index = 0;
};

enum class SeqAnchorDecisionResult : uint8_t {
  quorum_rejected,
  claimed_by_open_sequence,
  selected,
  /// Rule 6: this Sequence's OPEN window was closed on this frame because
  /// none of its voices was sustained and `candidate_signature` opened another
  /// exclusive Sequence (`related_sub_index`).
  contradicted_open_sequence,
};

struct SeqAnchorDecisionView {
  uint32_t frame = 0;
  size_t sub_index = 0;
  uint64_t key = 0;
  uint64_t candidate_signature = 0;
  size_t input_index = 0;
  SeqAnchorDecisionResult result = SeqAnchorDecisionResult::quorum_rejected;
  size_t related_sub_index = static_cast<size_t>(-1);
};

enum class SeqAnchorResolutionResult : uint8_t {
  no_candidate,
  no_selection,
  selected
};

struct SeqAnchorResolutionView {
  uint32_t frame = 0;
  SeqAnchorResolutionResult result = SeqAnchorResolutionResult::no_candidate;
  size_t selected_count = 0;
};

/// Terminal routing result for one authored sequence key on one anchor frame.
/// `individual` means that no applicable sequence visited that key and is the
/// only disposition that permits the playback consumer to use its ordinary
/// one-shot fallback. Every rejected disposition is terminal for that route.
enum class SeqTriggerDisposition : uint8_t {
  individual,
  selected,
  rejected_internal,
  rejected_quorum,
  rejected_claimed,
};

struct SeqTriggerResolution {
  SeqTriggerDisposition disposition = SeqTriggerDisposition::individual;
  uint64_t key = 0;
  uint64_t candidate_signature = 0;
  uint64_t owner_key = 0;
  size_t input_index = 0;
};

[[nodiscard]] inline bool
seq_trigger_is_rejected(SeqTriggerDisposition disposition) noexcept {
  return disposition == SeqTriggerDisposition::rejected_internal ||
         disposition == SeqTriggerDisposition::rejected_quorum ||
         disposition == SeqTriggerDisposition::rejected_claimed;
}

/// Allocation is completed by the constructor; the synchronous callbacks are
/// noexcept and only replace fixed result slots. This collector can therefore
/// be composed with observation callbacks on the resolver's real traversal.
class SeqAnchorRoutes {
public:
  explicit SeqAnchorRoutes(std::span<const SeqAnchorSub> substitutions)
      : substitutions_(substitutions), routes_(substitutions.size()),
        seen_(substitutions.size(), false) {}

  void operator()(const SeqAnchorCandidateView &value) noexcept {
    if (value.sub_index >= routes_.size() ||
        value.stage != SeqAnchorCandidateStage::internal)
      return;
    set(value.sub_index, SeqTriggerDisposition::rejected_internal,
        value.signature, value.input_index, 0);
  }

  void operator()(const SeqAnchorDecisionView &value) noexcept {
    if (value.sub_index >= routes_.size())
      return;
    switch (value.result) {
    case SeqAnchorDecisionResult::selected:
      set(value.sub_index, SeqTriggerDisposition::selected,
          value.candidate_signature, value.input_index, 0);
      break;
    case SeqAnchorDecisionResult::quorum_rejected:
      set(value.sub_index, SeqTriggerDisposition::rejected_quorum,
          value.candidate_signature, value.input_index, 0);
      break;
    case SeqAnchorDecisionResult::claimed_by_open_sequence: {
      const uint64_t owner = value.related_sub_index < substitutions_.size()
                                 ? substitutions_[value.related_sub_index].key
                                 : 0;
      set(value.sub_index, SeqTriggerDisposition::rejected_claimed,
          value.candidate_signature, value.input_index, owner);
      break;
    }
    case SeqAnchorDecisionResult::contradicted_open_sequence:
      break; // the window that closed, not a verdict on a trigger route
    }
  }

  [[nodiscard]] SeqTriggerResolution for_key(uint64_t key) const noexcept {
    for (size_t i = 0; i < substitutions_.size(); ++i) {
      if (substitutions_[i].key == key)
        return seen_[i] ? routes_[i]
                        : SeqTriggerResolution{
                              SeqTriggerDisposition::individual, key};
    }
    return {SeqTriggerDisposition::individual, key};
  }

private:
  void set(size_t index, SeqTriggerDisposition disposition,
           uint64_t candidate_signature, size_t input_index,
           uint64_t owner_key) noexcept {
    routes_[index] = {disposition, substitutions_[index].key,
                      candidate_signature, owner_key, input_index};
    seen_[index] = true;
  }

  std::span<const SeqAnchorSub> substitutions_;
  std::vector<SeqTriggerResolution> routes_;
  std::vector<bool> seen_;
};

/// Borrowed inputs of one actual anchor frame. source_indices, when supplied,
/// map the sorted signatures to the original detector-event array. Callbacks
/// must copy needed data before returning and must not mutate any input/state.
struct SeqAnchorFrameView {
  uint32_t frame = 0;
  std::span<const uint64_t> signatures;
  std::span<const SeqAnchorSub> substitutions;
  std::span<const SeqAnchorState> states;
  std::span<const size_t> source_indices;
};

/// ONE frame: given the signatures that START at `f` (key-ons), decides which
/// substitutions anchor (in priority order) and updates `st`. It serves replay
/// (the table) and live playback (the detector rising edge) — one single
/// criterion. `sustained` = signatures sounding at `f` that already sounded
/// at `f - 1`; only Rule 5 reads them (empty where no voice state exists).
/// The synchronous visitor must return void, be noexcept, and not reenter,
/// mutate inputs/state, perform I/O, block, or retain the borrowed view.
/// Observation adds no searches or storage proportional to the visit count.
template <class Visit, class Decide, class Finish>
inline std::vector<size_t>
seq_anchor_frame_decided(uint32_t f, const std::vector<uint64_t> &sigs,
                         std::optional<std::span<const uint64_t>> voice_state,
                         const std::vector<SeqAnchorSub> &subs,
                         std::vector<SeqAnchorState> &st, Visit &&visit,
                         Decide &&decide, Finish &&finish) {
  // `voice_state` = the signatures sounding at `f` that already sounded at
  // `f - 1`; nullopt = no voice state (the table), which switches Rule 6 off.
  const std::span<const uint64_t> sustained =
      voice_state ? *voice_state : std::span<const uint64_t>{};
  static_assert(
      std::is_nothrow_invocable_v<Visit &, const SeqAnchorCandidateView &>);
  static_assert(
      std::is_same_v<
          std::invoke_result_t<Visit &, const SeqAnchorCandidateView &>, void>);
  static_assert(
      std::is_nothrow_invocable_v<Decide &, const SeqAnchorDecisionView &>);
  static_assert(
      std::is_same_v<
          std::invoke_result_t<Decide &, const SeqAnchorDecisionView &>, void>);
  static_assert(
      std::is_nothrow_invocable_v<Finish &, const SeqAnchorResolutionView &>);
  static_assert(std::is_same_v<
                std::invoke_result_t<Finish &, const SeqAnchorResolutionView &>,
                void>);
  std::vector<size_t> cand, order, anchored;
  std::vector<uint64_t> cand_sig;
  std::vector<size_t> cand_input;
  std::vector<size_t> head_hits(subs.size(), 0);
  std::vector<uint8_t> trig_hit(subs.size(), 0);
  if (st.size() != subs.size())
    st.assign(subs.size(), SeqAnchorState{});
  // Rule 5: inside the step, only the WHOLE opening counts — the game
  // restarted the music. Counted before the gate so the gate can let it
  // through.
  std::vector<uint8_t> restart(subs.size(), 0);
  for (size_t i = 0; i < subs.size(); ++i) {
    const SeqAnchorSub &sq = subs[i];
    if (!sq.enabled || f >= st[i].next_free)
      continue;
    restart[i] = seq_whole_opening(sq, sigs, sustained);
  }
  // Rule 6 (voice state only): an OPEN exclusive Sequence is contradicted on
  // a frame where none of its claimed signatures is sustained and the frame
  // carries the whole opening of ANOTHER enabled exclusive Sequence — the
  // game cut this music and started that one. The window closes here and the
  // frame resolves as if it had never been open.
  if (voice_state) {
    for (size_t j = 0; j < subs.size(); ++j) {
      const SeqAnchorSub &sq = subs[j];
      if (!sq.enabled || !sq.exclusive || !st[j].open)
        continue;
      if (f < st[j].win_start || f >= st[j].win_end)
        continue;
      bool voices_alive = false;
      for (const uint64_t sig : sustained)
        if (seq_sub_claims(sq, sig)) {
          voices_alive = true;
          break;
        }
      if (voices_alive)
        continue;
      size_t other = subs.size();
      for (size_t i = 0; i < subs.size() && other == subs.size(); ++i)
        if (i != j && subs[i].enabled && subs[i].exclusive &&
            seq_opening_now(subs[i], sigs, sustained))
          other = i;
      if (other == subs.size())
        continue;
      size_t trigger_input = 0;
      for (const uint64_t sig : sigs) {
        if (sig == subs[other].trigger_signature)
          break;
        ++trigger_input;
      }
      st[j].open = false;
      st[j].win_end = f;
      st[j].next_free = f;
      restart[j] = 0;
      decide({f, j, sq.key, subs[other].trigger_signature, trigger_input,
              SeqAnchorDecisionResult::contradicted_open_sequence, other});
    }
  }
  size_t input_index = 0;
  for (const uint64_t sig : sigs) {
    for (size_t i = 0; i < subs.size(); ++i) {
      const SeqAnchorSub &sq = subs[i];
      SeqAnchorCandidateView observation{f, sig, i, sq.key};
      observation.input_index = input_index;
      if (!sq.enabled) {
        visit(std::as_const(observation));
        continue;
      }
      if (f < st[i].next_free && !restart[i]) {
        observation.stage = SeqAnchorCandidateStage::internal;
        visit(std::as_const(observation));
        continue; // internal occurrence
      }
      const bool is_trig = sq.trigger_signature == sig;
      const bool is_head = is_trig || std::find(sq.head.begin(), sq.head.end(),
                                                sig) != sq.head.end();
      if (!is_head) {
        observation.stage = SeqAnchorCandidateStage::not_head;
        visit(std::as_const(observation));
        continue;
      }
      if (is_trig)
        trig_hit[i] = 1;
      ++head_hits[i];
      const auto c = std::find(cand.begin(), cand.end(), i);
      observation.trigger = is_trig;
      observation.head_hits = head_hits[i];
      if (c != cand.end()) {
        if (is_trig) {
          const auto candidate_index = static_cast<size_t>(c - cand.begin());
          cand_sig[candidate_index] = sig;
          cand_input[candidate_index] = input_index;
        }
        observation.stage = SeqAnchorCandidateStage::accumulated;
        visit(std::as_const(observation));
        continue;
      }
      cand.push_back(i);
      cand_sig.push_back(sig);
      cand_input.push_back(input_index);
      observation.stage = SeqAnchorCandidateStage::formed;
      visit(std::as_const(observation));
    }
    ++input_index;
  }
  if (cand.empty()) {
    finish({f, SeqAnchorResolutionResult::no_candidate, 0});
    return anchored;
  }
  // Trigger present, or head quorum.
  for (size_t k = 0; k < cand.size();) {
    const size_t i = cand[k];
    const size_t q = seq_head_quorum(subs[i]);
    if (trig_hit[i] || (q && head_hits[i] >= q)) {
      ++k;
      continue;
    }
    decide({f, i, subs[i].key, cand_sig[k], cand_input[k],
            SeqAnchorDecisionResult::quorum_rejected});
    cand.erase(cand.begin() + static_cast<std::ptrdiff_t>(k));
    cand_sig.erase(cand_sig.begin() + static_cast<std::ptrdiff_t>(k));
    cand_input.erase(cand_input.begin() + static_cast<std::ptrdiff_t>(k));
  }
  if (cand.empty()) {
    finish({f, SeqAnchorResolutionResult::no_selection, 0});
    return anchored;
  }
  order.resize(cand.size());
  for (size_t k = 0; k < order.size(); ++k)
    order[k] = k;
  // Rule 4: continuation = looping, open, its step is due and its window has
  // not ended (the window may outlast the step: duration >= span).
  auto continues = [&](size_t i) {
    return subs[i].looping && st[i].open && f >= st[i].next_free &&
           f <= st[i].win_end;
  };
  // Rule 4: a claimer (lists another candidate's signature, is listed by
  // none) anchors before the candidates it claims.
  auto claimer = [&](size_t k) {
    bool claims = false, claimed = false;
    for (size_t m = 0; m < cand.size(); ++m) {
      if (m == k)
        continue;
      claims = claims || seq_sub_claims(subs[cand[k]], cand_sig[m]);
      claimed = claimed || seq_sub_claims(subs[cand[m]], cand_sig[k]);
    }
    return claims && !claimed;
  };
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    const bool ca = continues(cand[a]), cb = continues(cand[b]);
    if (ca != cb)
      return ca;
    const bool ka = claimer(a), kb = claimer(b);
    if (ka != kb)
      return ka;
    return seq_sub_before(subs[cand[a]], subs[cand[b]]);
  });
  for (const size_t k : order) {
    const size_t i = cand[k];
    const SeqAnchorSub &sq = subs[i];
    size_t claimed_by = static_cast<size_t>(-1);
    for (size_t j = 0; j < subs.size() && claimed_by == static_cast<size_t>(-1);
         ++j) {
      if (j == i || !st[j].open)
        continue;
      if (f < st[j].win_start || f >= st[j].win_end)
        continue;
      if (seq_sub_claims(subs[j], cand_sig[k]))
        claimed_by = j;
    }
    if (claimed_by != static_cast<size_t>(-1)) {
      decide({f, i, sq.key, cand_sig[k], cand_input[k],
              SeqAnchorDecisionResult::claimed_by_open_sequence, claimed_by});
      continue;
    }
    const uint32_t seg = sq.span_frames ? sq.span_frames : sq.duration_frames;
    st[i].next_free = f + (seg ? seg : 1u);
    st[i].open = true;
    st[i].win_start = f;
    st[i].win_end = f + sq.duration_frames;
    anchored.push_back(i);
    decide({f, i, sq.key, cand_sig[k], cand_input[k],
            SeqAnchorDecisionResult::selected});
  }
  finish({f,
          anchored.empty() ? SeqAnchorResolutionResult::no_selection
                           : SeqAnchorResolutionResult::selected,
          anchored.size()});
  return anchored;
}

/// Without voice state (the table, tests): Rule 5 relies on the whole head.
template <class Visit, class Decide, class Finish>
inline std::vector<size_t>
seq_anchor_frame_decided(uint32_t f, const std::vector<uint64_t> &sigs,
                         const std::vector<SeqAnchorSub> &subs,
                         std::vector<SeqAnchorState> &st, Visit &&visit,
                         Decide &&decide, Finish &&finish) {
  return seq_anchor_frame_decided(
      f, sigs, std::nullopt, subs, st, std::forward<Visit>(visit),
      std::forward<Decide>(decide), std::forward<Finish>(finish));
}

template <class Visit>
inline std::vector<size_t>
seq_anchor_frame_observed(uint32_t f, const std::vector<uint64_t> &sigs,
                          const std::vector<SeqAnchorSub> &subs,
                          std::vector<SeqAnchorState> &st, Visit &&visit) {
  return seq_anchor_frame_decided(
      f, sigs, subs, st, std::forward<Visit>(visit),
      [](const SeqAnchorDecisionView &) noexcept {},
      [](const SeqAnchorResolutionView &) noexcept {});
}

/// Original API, sharing the same traversal without an observation receiver.
inline std::vector<size_t>
seq_anchor_frame(uint32_t f, const std::vector<uint64_t> &sigs,
                 const std::vector<SeqAnchorSub> &subs,
                 std::vector<SeqAnchorState> &st) {
  return seq_anchor_frame_observed(
      f, sigs, subs, st, [](const SeqAnchorCandidateView &) noexcept {});
}

/// Anchor table: substitution key → the starts of its windows (ascending).
/// `sig_of(i)` / `start_of(i)` read event i out of the `n` detected.
/// Both synchronous observation callbacks return void and must be noexcept.
/// They cannot reenter, mutate inputs/state, perform I/O, block or retain
/// views. No sorting, source reads or allocation is added for observation.
template <class SigOf, class StartOf, class BeforeFrame, class Visit,
          class Decide, class Finish>
inline std::unordered_map<uint64_t, std::vector<uint32_t>>
seq_anchor_table_decided(size_t n, SigOf &&sig_of, StartOf &&start_of,
                         const std::vector<SeqAnchorSub> &subs,
                         BeforeFrame &&before_frame, Visit &&visit,
                         Decide &&decide, Finish &&finish) {
  static_assert(
      std::is_nothrow_invocable_v<BeforeFrame &, const SeqAnchorFrameView &>);
  static_assert(std::is_same_v<
                std::invoke_result_t<BeforeFrame &, const SeqAnchorFrameView &>,
                void>);
  static_assert(
      std::is_nothrow_invocable_v<Visit &, const SeqAnchorCandidateView &>);
  static_assert(
      std::is_same_v<
          std::invoke_result_t<Visit &, const SeqAnchorCandidateView &>, void>);
  std::unordered_map<uint64_t, std::vector<uint32_t>> out;
  std::vector<SeqAnchorState> st(subs.size());
  std::vector<uint64_t> sigs;
  for (const auto &s : subs)
    out[s.key];
  // Ascending frame order, stable (the detector emits per channel).
  std::vector<size_t> idx(n);
  for (size_t i = 0; i < n; ++i)
    idx[i] = i;
  std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
    return start_of(a) < start_of(b);
  });
  for (size_t ei = 0; ei < n;) {
    const uint32_t f = start_of(idx[ei]);
    sigs.clear();
    size_t ej = ei;
    for (; ej < n && start_of(idx[ej]) == f; ++ej)
      sigs.push_back(sig_of(idx[ej]));
    const SeqAnchorFrameView frame_view{f, sigs, subs, st,
                                        std::span{idx}.subspan(ei, ej - ei)};
    before_frame(frame_view);
    ei = ej;
    for (const size_t i :
         seq_anchor_frame_decided(f, sigs, subs, st, visit, decide, finish))
      out[subs[i].key].push_back(f);
  }
  return out;
}

template <class SigOf, class StartOf, class BeforeFrame, class Visit>
inline std::unordered_map<uint64_t, std::vector<uint32_t>>
seq_anchor_table_observed(size_t n, SigOf &&sig_of, StartOf &&start_of,
                          const std::vector<SeqAnchorSub> &subs,
                          BeforeFrame &&before_frame, Visit &&visit) {
  return seq_anchor_table_decided(
      n, std::forward<SigOf>(sig_of), std::forward<StartOf>(start_of), subs,
      std::forward<BeforeFrame>(before_frame), std::forward<Visit>(visit),
      [](const SeqAnchorDecisionView &) noexcept {},
      [](const SeqAnchorResolutionView &) noexcept {});
}

/// Original table API; observation uses the same stable sort and source reads.
template <class SigOf, class StartOf>
inline std::unordered_map<uint64_t, std::vector<uint32_t>>
seq_anchor_table(size_t n, SigOf sig_of, StartOf start_of,
                 const std::vector<SeqAnchorSub> &subs) {
  return seq_anchor_table_observed(
      n, sig_of, start_of, subs, [](const SeqAnchorFrameView &) noexcept {},
      [](const SeqAnchorCandidateView &) noexcept {});
}

} // namespace ayther
