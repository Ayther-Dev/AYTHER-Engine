#include <ayther/engine/music_live_recognition.hpp>

#include <cstdio>

namespace {
void check(bool value, const char *message, int &failures) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}
} // namespace

int main() {
  using namespace ayther::engine;
  int failures = 0;
  constexpr std::uint64_t mib = 1024ULL * 1024ULL;
  const LiveRecognitionEvidence approximate{0.90,
                                            false,
                                            6,
                                            true,
                                            true,
                                            false,
                                            2'000'000'000ULL,
                                            128,
                                            32 * mib,
                                            false,
                                            AnalysisExecutionContext::audio,
                                            false};
  const auto accepted = evaluate_live_recognition(approximate);
  check(accepted.status == LiveRecognitionStatus::recognized &&
            accepted.automatic && !accepted.offline_analysis_executed,
        "high approximate score with usable entry and resolved position passes",
        failures);

  auto weak = approximate;
  weak.score = 0.899;
  check(evaluate_live_recognition(weak).status ==
            LiveRecognitionStatus::pending,
        "approximate score below high threshold does not recognize", failures);
  weak = approximate;
  weak.entry_usable = false;
  check(evaluate_live_recognition(weak).status ==
            LiveRecognitionStatus::pending,
        "approximate match requires a usable entry", failures);
  weak = approximate;
  weak.position_resolved = false;
  check(evaluate_live_recognition(weak).status ==
            LiveRecognitionStatus::pending,
        "approximate match requires resolved position or author choice",
        failures);
  weak.author_selected = true;
  check(evaluate_live_recognition(weak).status ==
            LiveRecognitionStatus::author_selected,
        "author selection is reported separately", failures);

  auto exact = approximate;
  exact.exact_anchor_validated = true;
  exact.match_count = 1;
  exact.score = 1.0;
  check(evaluate_live_recognition(exact).status ==
            LiveRecognitionStatus::recognized,
        "validated exact anchor does not require four matched pairs", failures);

  for (auto over :
       {LiveRecognitionEvidence{0.90, false, 6, true, true, false,
                                2'000'000'001ULL, 128, 32 * mib, false,
                                AnalysisExecutionContext::audio, false},
        LiveRecognitionEvidence{0.90, false, 6, true, true, false,
                                2'000'000'000ULL, 129, 32 * mib, false,
                                AnalysisExecutionContext::audio, false},
        LiveRecognitionEvidence{0.90, false, 6, true, true, false,
                                2'000'000'000ULL, 128, 32 * mib + 1, false,
                                AnalysisExecutionContext::audio, false}}) {
    const auto limited = evaluate_live_recognition(over);
    check(limited.status == LiveRecognitionStatus::limit &&
              limited.diagnostic == "recognition_limit" &&
              limited.keep_original && !limited.automatic,
          "P18-03 excess cannot recognize from truncated evidence", failures);
  }
  auto truncated = approximate;
  truncated.evidence_truncated = true;
  check(evaluate_live_recognition(truncated).status ==
            LiveRecognitionStatus::limit,
        "explicitly truncated evidence never confirms", failures);
  auto offline = approximate;
  offline.offline_analysis_requested = true;
  const auto audio_thread = evaluate_live_recognition(offline);
  check(!audio_thread.offline_analysis_executed &&
            audio_thread.diagnostic == "offline_analysis_forbidden",
        "audio thread never executes offline analysis", failures);
  return failures == 0 ? 0 : 1;
}
