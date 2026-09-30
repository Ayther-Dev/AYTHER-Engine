// ---------------------------------------------------------------------------
// audio_match_rule.h — event-substitution matching policy.
//
// Exact signatures distinguish note, channel, and pan changes. An author can
// opt into broader matching per assignment, persisted as `match` in
// audio_events.toml. Legacy packs remain exact without migration because the
// persisted primary key is still `signature`.
//
// This header has no SDL dependency. Its deterministic table lookup is covered
// by tests/audio_match_rule_test.cpp.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace ayther {

/// Match policy for one authored signature. Numeric values cross FFI/TOML and
/// are ABI data; do not reorder them.
enum class AudioMatchRule : uint8_t {
  kExact = 0,           ///< Exact signature only; legacy default.
  kInstrument = 1,      ///< Any voice using the same instrument.
  kInstrumentPitch = 2, ///< Same instrument and pitch.
};

/// Detector sentinel for unpitched DAC/noise events.
inline constexpr uint8_t kAudioNoPitch = 255;

/// Policy metadata persisted with an authored signature assignment.
struct AudioMatchRuleInfo {
  AudioMatchRule rule = AudioMatchRule::kExact;
  uint64_t instrument = 0;       ///< Timbre identity.
  uint8_t pitch = kAudioNoPitch; ///< Used only by kInstrumentPitch.
};

/// Instrument-to-assignment index for active-voice matching.
///
/// The caller checks exact signatures first. This index then prefers matching
/// `kInstrumentPitch` entries over `kInstrument`; ties select the numerically
/// lowest authored signature for deterministic results.
class AudioMatchIndex {
public:
  /// Observed lookup input; named fields keep instrument and pitch distinct.
  struct Query {
    uint64_t instrument = 0;
    uint8_t pitch = kAudioNoPitch;
  };
  /// One index entry actually visited, before checking its pitch or rank.
  /// Visitation is not selection; duplicates remain separate visits.
  struct CandidateView {
    uint64_t signature = 0;
    AudioMatchRule rule = AudioMatchRule::kExact;
    uint8_t pitch = kAudioNoPitch;
  };
  enum class CandidateResult : uint8_t {
    pitch_rejected,
    winner_updated,
    lower_priority,
  };
  struct CandidateDecisionView {
    CandidateView candidate;
    CandidateResult result = CandidateResult::pitch_rejected;
    uint64_t best_signature = 0;
    int best_rank = -1;
  };
  enum class ResolutionResult : uint8_t {
    invalid_instrument,
    empty_index,
    unknown_instrument,
    no_applicable_candidate,
    selected,
  };
  struct ResolutionView {
    ResolutionResult result = ResolutionResult::invalid_instrument;
    uint64_t selected_signature = 0;
    int selected_rank = -1;
  };
  void clear() { by_instr_.clear(); }
  [[nodiscard]] bool empty() const noexcept { return by_instr_.empty(); }

  /// Adds a broad-match assignment. Exact or incomplete policies are ignored.
  void add(uint64_t authored_sig, AudioMatchRule rule, uint64_t instrument,
           uint8_t pitch) {
    if (rule == AudioMatchRule::kExact || instrument == 0)
      return;
    if (rule == AudioMatchRule::kInstrumentPitch && pitch == kAudioNoPitch)
      return;
    by_instr_.emplace(instrument, Entry{authored_sig, pitch, rule});
  }

  /// Resolves one instrument/pitch pair. Unknown instruments never match.
  /// Returns true and writes the canonical authored signature when found.
  [[nodiscard]] bool resolve(uint64_t instrument, uint8_t pitch,
                             uint64_t *out) const {
    return resolve_observed({instrument, pitch}, out,
                            [](const CandidateView &) noexcept {});
  }

  /// Resolves through the same traversal and visits each encountered entry.
  /// The synchronous visitor returns void and must be noexcept. It must not
  /// reenter/mutate this index or the output, perform I/O, block, or retain the
  /// view. No callback is made for entries outside the instrument range, an
  /// unknown instrument, or an empty index. Candidate visits do not report a
  /// verdict.
  template <typename Visit>
  [[nodiscard]] bool resolve_observed(Query query, uint64_t *out,
                                      Visit &&visit) const {
    return resolve_decided(
        query, out, std::forward<Visit>(visit),
        [](const CandidateDecisionView &) noexcept {},
        [](const ResolutionView &) noexcept {});
  }

  /// Resolves through the same traversal and reports the decisions made in
  /// its original branches. Callbacks are synchronous, borrowed, and
  /// observational under the same restrictions as resolve_observed.
  template <typename Visit, typename Decide, typename Finish>
  [[nodiscard]] bool resolve_decided(Query query, uint64_t *out, Visit &&visit,
                                     Decide &&decide, Finish &&finish) const {
    static_assert(std::is_nothrow_invocable_v<Visit &, const CandidateView &>);
    static_assert(
        std::is_same_v<std::invoke_result_t<Visit &, const CandidateView &>,
                       void>);
    static_assert(
        std::is_nothrow_invocable_v<Decide &, const CandidateDecisionView &>);
    static_assert(std::is_same_v<
                  std::invoke_result_t<Decide &, const CandidateDecisionView &>,
                  void>);
    static_assert(
        std::is_nothrow_invocable_v<Finish &, const ResolutionView &>);
    static_assert(
        std::is_same_v<std::invoke_result_t<Finish &, const ResolutionView &>,
                       void>);
    const auto instrument = query.instrument;
    const auto pitch = query.pitch;
    if (instrument == 0) {
      finish({ResolutionResult::invalid_instrument});
      return false;
    }
    if (by_instr_.empty()) {
      finish({ResolutionResult::empty_index});
      return false;
    }
    int best_rank = -1;
    uint64_t best_sig = 0;
    const auto range = by_instr_.equal_range(instrument);
    if (range.first == range.second) {
      finish({ResolutionResult::unknown_instrument});
      return false;
    }
    for (auto it = range.first; it != range.second; ++it) {
      const Entry &e = it->second;
      const CandidateView candidate{e.sig, e.rule, e.pitch};
      visit(candidate);
      int rank = 0;
      if (e.rule == AudioMatchRule::kInstrumentPitch) {
        if (pitch == kAudioNoPitch || pitch != e.pitch) {
          decide({candidate, CandidateResult::pitch_rejected, best_sig,
                  best_rank});
          continue;
        }
        rank = 1;
      }
      if (rank > best_rank || (rank == best_rank && e.sig < best_sig)) {
        best_rank = rank;
        best_sig = e.sig;
        decide(
            {candidate, CandidateResult::winner_updated, best_sig, best_rank});
      } else {
        decide(
            {candidate, CandidateResult::lower_priority, best_sig, best_rank});
      }
    }
    if (best_rank < 0) {
      finish({ResolutionResult::no_applicable_candidate, best_sig, best_rank});
      return false;
    }
    finish({ResolutionResult::selected, best_sig, best_rank});
    if (out)
      *out = best_sig;
    return true;
  }

private:
  struct Entry {
    uint64_t sig;
    uint8_t pitch;
    AudioMatchRule rule;
  };
  std::unordered_multimap<uint64_t, Entry> by_instr_;
};

} // namespace ayther
