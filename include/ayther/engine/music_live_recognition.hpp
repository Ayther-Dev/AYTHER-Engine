#pragma once

#include <ayther/engine/music_analysis_limits.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace ayther::engine {

inline constexpr std::uint64_t max_live_recognition_window_ns =
    2'000'000'000ULL;
inline constexpr std::size_t max_live_candidates_per_bus = 128;
inline constexpr std::uint64_t max_live_recognition_bytes =
    32ULL * 1024ULL * 1024ULL;

struct LiveRecognitionEvidence {
  double score{};
  bool exact_anchor_validated{};
  std::size_t match_count{};
  bool entry_usable{};
  bool position_resolved{};
  bool author_selected{};
  std::uint64_t window_ns{};
  std::size_t candidates_on_bus{};
  std::uint64_t dynamic_bytes{};
  bool evidence_truncated{};
  AnalysisExecutionContext context{AnalysisExecutionContext::audio};
  bool offline_analysis_requested{};
};

enum class LiveRecognitionStatus : std::uint8_t {
  pending,
  recognized,
  author_selected,
  limit,
};

struct LiveRecognitionDecision {
  LiveRecognitionStatus status{LiveRecognitionStatus::pending};
  std::string diagnostic;
  bool keep_original{true};
  bool automatic{};
  bool offline_analysis_executed{};
};

[[nodiscard]] inline LiveRecognitionDecision
evaluate_live_recognition(const LiveRecognitionEvidence &evidence) {
  if (evidence.window_ns > max_live_recognition_window_ns ||
      evidence.candidates_on_bus > max_live_candidates_per_bus ||
      evidence.dynamic_bytes > max_live_recognition_bytes ||
      evidence.evidence_truncated)
    return {LiveRecognitionStatus::limit, "recognition_limit", true, false,
            false};

  const bool approximate_usable = evidence.score >= 0.90 &&
                                  evidence.match_count >= 4;
  const bool musical_match = evidence.exact_anchor_validated ||
                             approximate_usable;
  const bool position_usable = evidence.position_resolved ||
                               evidence.author_selected;
  LiveRecognitionDecision result;
  if (musical_match && evidence.entry_usable && position_usable) {
    if (evidence.author_selected)
      result = {LiveRecognitionStatus::author_selected, {}, false, false,
                false};
    else
      result = {LiveRecognitionStatus::recognized, {}, false, true, false};
  }
  if (evidence.offline_analysis_requested &&
      evidence.context == AnalysisExecutionContext::audio)
    result.diagnostic = "offline_analysis_forbidden";
  return result;
}

} // namespace ayther::engine
