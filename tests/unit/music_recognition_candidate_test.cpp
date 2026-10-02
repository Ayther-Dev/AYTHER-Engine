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
  MusicRecognitionCandidate candidate;
  check(candidate.begin(100, 1'000'000'000ULL) &&
            candidate.deadline_music_ns() == 3'000'000'000ULL,
        "deadline is fixed two musical seconds after first event", failures);
  candidate.observe_match(101, 2'900'000'000ULL);
  check(candidate.deadline_music_ns() == 3'000'000'000ULL,
        "later matches never extend the deadline", failures);
  check(candidate.evaluate(2'999'999'999ULL, false, false, 2'999'999'999ULL)
                .status == CandidateDecisionStatus::pending,
        "candidate remains pending before the fixed deadline", failures);
  const auto timeout =
      candidate.evaluate(3'000'000'000ULL, false, false, 3'000'000'000ULL);
  check(timeout.status == CandidateDecisionStatus::timed_out &&
            timeout.diagnostic == "recognition_timeout" &&
            timeout.keep_original && timeout.keep_active_traversal,
        "timeout preserves original and active traversal with diagnostic",
        failures);
  check(!candidate.begin(100, 3'100'000'000ULL),
        "same entry cannot silently reopen a timed-out candidate", failures);
  check(candidate.begin(102, 3'200'000'000ULL),
        "a distinct new entry starts a fresh candidate", failures);

  const auto ambiguous =
      candidate.evaluate(3'300'000'000ULL, false, false, 3'300'000'000ULL);
  check(ambiguous.status == CandidateDecisionStatus::pending &&
            ambiguous.keep_original && ambiguous.keep_active_traversal,
        "ambiguous candidate leaves original and traversal unchanged", failures);
  const auto authored =
      candidate.evaluate(3'400'000'000ULL, true, true, 3'400'000'000ULL);
  check(authored.status == CandidateDecisionStatus::author_selected &&
            authored.author_choice && !authored.automatic,
        "author choice is explicit and not automatic recognition", failures);

  MusicRecognitionCandidate causal;
  check(causal.begin(200, 10'000'000'000ULL), "causal candidate starts",
        failures);
  const auto future =
      causal.evaluate(10'100'000'000ULL, true, false, 10'100'000'001ULL);
  check(future.status == CandidateDecisionStatus::pending &&
            future.diagnostic == "future_evidence_rejected",
        "evidence after decision time is never consumed", failures);
  return failures == 0 ? 0 : 1;
}
