#pragma once

#include <cstdint>
#include <string>

namespace ayther::engine {

inline constexpr std::uint64_t recognition_candidate_timeout_ns =
    2'000'000'000ULL;

enum class CandidateDecisionStatus : std::uint8_t {
  inactive,
  pending,
  recognized,
  author_selected,
  timed_out,
};

struct CandidateDecision {
  CandidateDecisionStatus status{CandidateDecisionStatus::inactive};
  std::string diagnostic;
  bool keep_original{true};
  bool keep_active_traversal{true};
  bool author_choice{};
  bool automatic{};
};

class MusicRecognitionCandidate {
public:
  [[nodiscard]] bool begin(std::uint64_t entry,
                           std::uint64_t music_time_ns) noexcept {
    if (active_ || (timed_out_entry_ != 0 && entry == timed_out_entry_))
      return false;
    entry_ = entry;
    first_music_ns_ = music_time_ns;
    deadline_music_ns_ = music_time_ns + recognition_candidate_timeout_ns;
    active_ = true;
    return true;
  }

  void observe_match(std::uint64_t entry,
                     std::uint64_t music_time_ns) noexcept {
    if (!active_ || entry != entry_ || music_time_ns < first_music_ns_)
      return;
    // Deliberately no deadline update: later evidence cannot extend P18-03.
  }

  [[nodiscard]] CandidateDecision evaluate(
      std::uint64_t music_time_ns, bool position_resolved, bool author_choice,
      std::uint64_t evidence_observed_until_ns) noexcept {
    if (!active_)
      return {};
    if (evidence_observed_until_ns > music_time_ns)
      return {CandidateDecisionStatus::pending, "future_evidence_rejected",
              true, true, false, false};
    if (music_time_ns >= deadline_music_ns_) {
      active_ = false;
      timed_out_entry_ = entry_;
      return {CandidateDecisionStatus::timed_out, "recognition_timeout", true,
              true, false, false};
    }
    if (!position_resolved)
      return {CandidateDecisionStatus::pending, {}, true, true, false, false};
    active_ = false;
    if (author_choice)
      return {CandidateDecisionStatus::author_selected, {}, false, true, true,
              false};
    return {CandidateDecisionStatus::recognized, {}, false, true, false, true};
  }

  [[nodiscard]] std::uint64_t deadline_music_ns() const noexcept {
    return deadline_music_ns_;
  }

private:
  std::uint64_t entry_{};
  std::uint64_t timed_out_entry_{};
  std::uint64_t first_music_ns_{};
  std::uint64_t deadline_music_ns_{};
  bool active_{};
};

} // namespace ayther::engine
