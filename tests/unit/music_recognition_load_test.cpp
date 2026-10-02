#include <ayther/engine/music_live_recognition.hpp>
#include <ayther/engine/music_recognition_candidate.hpp>

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

  std::size_t automatic_entries = 0;
  std::size_t false_positions = 0;
  for (std::size_t index = 0; index < 1'000; ++index) {
    const LiveRecognitionEvidence negative{
        0.40 + static_cast<double>(index % 40) / 100.0,
        false,
        index % 4,
        true,
        false,
        false,
        1'000'000'000ULL,
        128,
        32 * mib,
        false,
        AnalysisExecutionContext::audio,
        false};
    const auto decision = evaluate_live_recognition(negative);
    automatic_entries += decision.automatic ? 1U : 0U;
    false_positions +=
        decision.status == LiveRecognitionStatus::recognized ? 1U : 0U;
  }
  check(automatic_entries == 0 && false_positions == 0,
        "1000 negative controls produce no automatic entry or position",
        failures);

  const LiveRecognitionEvidence indistinguishable{
      1.0,
      true,
      8,
      true,
      false,
      false,
      2'000'000'000ULL,
      128,
      32 * mib,
      false,
      AnalysisExecutionContext::audio,
      false};
  check(evaluate_live_recognition(indistinguishable).status ==
            LiveRecognitionStatus::pending,
        "indistinguishable evidence cannot fabricate a resolved position",
        failures);

  auto boundary = indistinguishable;
  boundary.exact_anchor_validated = false;
  boundary.position_resolved = true;
  boundary.score = 0.90;
  boundary.match_count = 4;
  check(evaluate_live_recognition(boundary).status ==
            LiveRecognitionStatus::recognized,
        "inclusive score and match-count thresholds recognize", failures);
  boundary.score = 0.899999;
  check(evaluate_live_recognition(boundary).status ==
            LiveRecognitionStatus::pending,
        "score immediately below threshold remains pending", failures);
  boundary.score = 0.90;
  boundary.match_count = 3;
  check(evaluate_live_recognition(boundary).status ==
            LiveRecognitionStatus::pending,
        "one match below the minimum remains pending", failures);

  boundary.match_count = 4;
  for (const auto over :
       {LiveRecognitionEvidence{0.90, false, 4, true, true, false,
                                2'000'000'001ULL, 128, 32 * mib, false,
                                AnalysisExecutionContext::audio, false},
        LiveRecognitionEvidence{0.90, false, 4, true, true, false,
                                2'000'000'000ULL, 129, 32 * mib, false,
                                AnalysisExecutionContext::audio, false},
        LiveRecognitionEvidence{0.90, false, 4, true, true, false,
                                2'000'000'000ULL, 128, 32 * mib + 1, false,
                                AnalysisExecutionContext::audio, false}}) {
    const auto decision = evaluate_live_recognition(over);
    check(decision.status == LiveRecognitionStatus::limit &&
              decision.diagnostic == "recognition_limit" &&
              decision.keep_original && !decision.automatic,
          "P18-03 excess rejects automatic recognition", failures);
  }

  MusicRecognitionCandidate candidate;
  check(candidate.begin(9, 100), "candidate starts at first causal event",
        failures);
  candidate.observe_match(9, 150);
  const auto future = candidate.evaluate(200, true, false, 201);
  check(future.status == CandidateDecisionStatus::pending &&
            future.diagnostic == "future_evidence_rejected",
        "evidence beyond decision time cannot resolve live recognition",
        failures);
  const auto causal = candidate.evaluate(200, true, false, 200);
  check(causal.status == CandidateDecisionStatus::recognized &&
            causal.automatic,
        "same evidence resolves once causally observed", failures);

  std::printf("negatives=1000 automatic=%zu false_positions=%zu "
              "candidates=128 bytes=%llu\n",
              automatic_entries, false_positions,
              static_cast<unsigned long long>(32 * mib));
  return failures == 0 ? 0 : 1;
}
