#include <ayther/engine/music_analysis_limits.hpp>

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

  const AnalysisAdmission maximum{{900'000'000'000ULL, 54'000, 1'000'000,
                                   4'096},
                                  AnalysisExecutionContext::worker};
  check(check_analysis_admission(maximum).status ==
            AnalysisAdmissionStatus::accepted,
        "simultaneous exact P18-01 limits are accepted", failures);

  for (const auto over : {
           AnalysisInputSize{900'000'000'001ULL, 54'000, 1'000'000, 4'096},
           AnalysisInputSize{900'000'000'000ULL, 54'001, 1'000'000, 4'096},
           AnalysisInputSize{900'000'000'000ULL, 54'000, 1'000'001, 4'096},
           AnalysisInputSize{900'000'000'000ULL, 54'000, 1'000'000, 4'097}}) {
    const auto result = check_analysis_admission(
        AnalysisAdmission{over, AnalysisExecutionContext::worker});
    check(result.status == AnalysisAdmissionStatus::input_limit &&
              !result.truncated,
          "each P18-01 neighbor is rejected before analysis without truncation",
          failures);
  }
  check(check_analysis_admission(
            AnalysisAdmission{maximum.input, AnalysisExecutionContext::audio})
            .status == AnalysisAdmissionStatus::wrong_execution_context &&
            check_analysis_admission(
                AnalysisAdmission{maximum.input, AnalysisExecutionContext::ui})
                    .status == AnalysisAdmissionStatus::wrong_execution_context,
        "offline analysis is rejected on audio and UI contexts", failures);

  AnalysisProjectGate gate;
  const auto lease = gate.try_acquire(ProjectId{7});
  check(lease.has_value() && !gate.try_acquire(ProjectId{7}).has_value(),
        "one active analysis per project, with no implicit queue", failures);
  gate.release(*lease);
  check(gate.try_acquire(ProjectId{7}).has_value(),
        "project becomes available after release", failures);

  const AnalysisWorkMetrics exact{256 * mib, 10'000, 120'000'000'000ULL,
                                  2'000'000'000ULL, 10'000, 1'000'000, true};
  check(evaluate_analysis_work(exact).status == AnalysisWorkStatus::complete,
        "exact P18-02 work and cancellation limits pass", failures);

  for (const auto over : {
           AnalysisWorkMetrics{256 * mib + 1, 10'000, 120'000'000'000ULL,
                               2'000'000'000ULL, 9'000, 1'000'000, false},
           AnalysisWorkMetrics{256 * mib, 10'001, 120'000'000'000ULL,
                               2'000'000'000ULL, 9'000, 1'000'000, false},
           AnalysisWorkMetrics{256 * mib, 10'000, 120'000'000'001ULL,
                               2'000'000'000ULL, 9'000, 1'000'000, false},
           AnalysisWorkMetrics{256 * mib, 10'000, 120'000'000'000ULL,
                               2'000'000'001ULL, 9'000, 1'000'000, false}}) {
    const auto result = evaluate_analysis_work(over);
    check(result.status == AnalysisWorkStatus::analysis_limit &&
              result.partial && result.covered_events == 9'000 &&
              result.total_events == 1'000'000 && !result.limit.empty(),
          "P18-02 excess is explicit partial coverage", failures);
  }

  auto mandatory = exact;
  mandatory.private_bytes += 1;
  mandatory.mandatory_scenario = true;
  check(evaluate_analysis_work(mandatory).acceptance_blocked,
        "resource exhaustion blocks mandatory-scenario acceptance", failures);

  return failures == 0 ? 0 : 1;
}
