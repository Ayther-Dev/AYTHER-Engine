#include <ayther/engine/music_analysis_limits.hpp>
#include <ayther/engine/music_corpus_quality.hpp>
#include <ayther/engine/music_event_scoring.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifndef AYTHER_RF18_EVIDENCE_DIR
#error AYTHER_RF18_EVIDENCE_DIR is required
#endif

namespace {
using Row = std::map<std::string, std::string>;

void check(bool value, const char *message, int &failures) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

std::vector<std::string> fields(const std::string &line) {
  std::vector<std::string> result;
  std::string field;
  bool quoted = false;
  for (char value : line) {
    if (value == '"')
      quoted = !quoted;
    else if (value == ',' && !quoted) {
      result.push_back(field);
      field.clear();
    } else
      field.push_back(value);
  }
  result.push_back(field);
  return result;
}

std::vector<Row> read_csv(const std::string &name) {
  std::ifstream input{std::string{AYTHER_RF18_EVIDENCE_DIR} + "/" + name};
  std::string line;
  std::getline(input, line);
  const auto headers = fields(line);
  std::vector<Row> rows;
  while (std::getline(input, line)) {
    const auto values = fields(line);
    Row row;
    for (std::size_t index = 0; index < headers.size(); ++index)
      row.emplace(headers[index], values[index]);
    rows.push_back(std::move(row));
  }
  return rows;
}

std::uint64_t pattern_seed(const std::string &pattern) {
  std::uint64_t result = 1469598103934665603ULL;
  for (unsigned char value : pattern) {
    result ^= value;
    result *= 1099511628211ULL;
  }
  return result;
}

std::vector<ayther::engine::MusicEvent> make_pattern(const std::string &pattern,
                                                     std::size_t count) {
  std::vector<ayther::engine::MusicEvent> result;
  result.reserve(count);
  const auto seed = pattern_seed(pattern);
  for (std::size_t index = 0; index < count; ++index)
    result.push_back({ayther::engine::EventId{index + 1}, 100 + index * 4, 1,
                      seed + index, 1, static_cast<std::uint32_t>(index % 128),
                      ayther::engine::EventProvenance{"corpus", 1, index + 1}});
  return result;
}
} // namespace

int main() {
  using namespace ayther::engine;
  int failures = 0;
  const auto exact_rows = read_csv("rf18-corpus-exact.csv");
  const auto positive_rows = read_csv("rf18-corpus-perturbed.csv");
  const auto negative_rows = read_csv("rf18-corpus-negative.csv");
  check(exact_rows.size() == 100 && positive_rows.size() == 120 &&
            negative_rows.size() == 1000,
        "frozen corpus row counts match the oracle manifest", failures);

  std::vector<QualityOracle> exact_oracles;
  std::vector<QualityPrediction> exact_predictions;
  for (const auto &row : exact_rows) {
    const auto begin = std::stoull(row.at("start_frame"));
    const auto end = std::stoull(row.at("end_frame_inclusive")) + 1;
    exact_oracles.push_back({row.at("interval_id"), "exact", true,
                             row.at("pattern_id"), begin, end});
    exact_predictions.push_back({row.at("interval_id"), row.at("pattern_id"),
                                 begin, end, false, false});
  }
  const auto exact =
      evaluate_corpus_quality(exact_oracles, exact_predictions, 2);
  std::printf("exact: TP=%zu FP=%zu FN=%zu precision=%.3f recall=%.3f\n",
              exact.true_positive, exact.false_positive, exact.false_negative,
              exact.precision.value_or(0.0), exact.recall.value_or(0.0));
  check(exact.true_positive == 100 && exact.false_positive == 0 &&
            exact.false_negative == 0 && exact.precision == 1.0 &&
            exact.recall == 1.0,
        "exact corpus reaches 100 percent precision and recall", failures);

  std::vector<QualityOracle> positive_oracles;
  std::vector<QualityPrediction> positive_predictions;
  for (const auto &row : positive_rows) {
    const auto count =
        static_cast<std::size_t>(std::stoull(row.at("reference_event_count")));
    const auto reference = make_pattern(row.at("base_pattern_id"), count);
    auto observed = reference;
    const auto omission = std::stoi(row.at("omission_count"));
    if (omission != 0)
      observed.erase(observed.begin() + std::stoi(row.at("omission_index")));
    const auto insertion = std::stoi(row.at("insertion_count"));
    if (insertion != 0) {
      const auto index =
          static_cast<std::size_t>(std::stoi(row.at("insertion_index")));
      const auto at = std::min(index, observed.size());
      const auto begin =
          at < observed.size() ? observed[at].begin : observed.back().begin + 2;
      observed.insert(observed.begin() + static_cast<std::ptrdiff_t>(at),
                      {EventId{9'000'000}, begin, 1, pattern_seed("noise"), 9,
                       9, EventProvenance{"noise", 9, 9'000'000}});
    }
    const auto delta = std::stoi(row.at("temporal_delta_frames"));
    const auto rotation = std::stoi(row.at("channel_rotation_steps"));
    for (auto &value : observed) {
      value.begin = static_cast<std::uint64_t>(
          static_cast<std::int64_t>(value.begin) + delta);
      value.provenance.channel += static_cast<std::uint32_t>(rotation);
    }
    const auto score =
        score_music_events(reference, observed, row.at("family"), 2);
    positive_oracles.push_back({row.at("case_id"), row.at("family"), true,
                                row.at("expected_identity"), std::nullopt,
                                std::nullopt});
    if (score.score && *score.score >= 0.75 && !score.insufficient_evidence)
      positive_predictions.push_back({row.at("case_id"),
                                      row.at("base_pattern_id"), std::nullopt,
                                      std::nullopt, false, false});
  }
  const auto perturbed =
      evaluate_corpus_quality(positive_oracles, positive_predictions, 2);
  std::printf("perturbed: TP=%zu FP=%zu FN=%zu precision=%.3f recall=%.3f\n",
              perturbed.true_positive, perturbed.false_positive,
              perturbed.false_negative, perturbed.precision.value_or(0.0),
              perturbed.recall.value_or(0.0));
  for (const auto &family : perturbed.families)
    std::printf("  %s: TP=%zu FP=%zu FN=%zu precision=%.3f recall=%.3f\n",
                family.family.c_str(), family.true_positive,
                family.false_positive, family.false_negative,
                family.precision.value_or(0.0), family.recall.value_or(0.0));
  check(perturbed.precision && *perturbed.precision >= 0.99 &&
            perturbed.recall && *perturbed.recall >= 0.95,
        "perturbed corpus meets global precision and recall", failures);
  check(std::ranges::all_of(perturbed.families,
                            [](const auto &family) {
                              return family.precision &&
                                     *family.precision >= 0.99 &&
                                     family.recall && *family.recall >= 0.95;
                            }),
        "every perturbed family meets its thresholds", failures);

  std::vector<QualityOracle> negative_oracles;
  for (const auto &row : negative_rows)
    negative_oracles.push_back({row.at("case_id"), row.at("family"), false,
                                row.at("candidate_pattern_id"), std::nullopt,
                                std::nullopt});
  const auto negatives = evaluate_corpus_quality(negative_oracles, {}, 2);
  std::printf("negative: FP=%zu automatic=%zu false_positions=%zu\n",
              negatives.false_positive, negatives.automatic_entries,
              negatives.false_resolved_positions);
  check(negatives.false_positive == 0 && negatives.automatic_entries == 0 &&
            negatives.false_resolved_positions == 0,
        "all 1000 negatives produce zero automatic entries or false positions",
        failures);
  check(perturbed.duplicates == 0 && perturbed.partial_predictions == 0 &&
            perturbed.ambiguous_predictions == 0,
        "duplicates, partials and ambiguities are published", failures);

  constexpr std::uint64_t mib = 1024ULL * 1024ULL;
  const AnalysisAdmission admitted{
      {900'000'000'000ULL, 54'000, 1'000'000, 4'096},
      AnalysisExecutionContext::worker};
  check(check_analysis_admission(admitted).status ==
            AnalysisAdmissionStatus::accepted,
        "mandatory corpus case is admitted before work", failures);
  const AnalysisWorkMetrics exhausted{
      256 * mib + 1, 10'000, 120'000'000'000ULL, 2'000'000'000ULL, 900'000,
      1'000'000,     true};
  const auto exhausted_result = evaluate_analysis_work(exhausted);
  check(exhausted_result.status == AnalysisWorkStatus::analysis_limit &&
            exhausted_result.partial && exhausted_result.acceptance_blocked &&
            exhausted_result.limit == "private_memory",
        "admitted mandatory case fails acceptance on resource exhaustion",
        failures);
  std::printf("publication: duplicates=%zu partials=%zu ambiguities=%zu "
              "mandatory_exhaustion=%s\n",
              perturbed.duplicates, perturbed.partial_predictions,
              perturbed.ambiguous_predictions,
              exhausted_result.acceptance_blocked ? "blocked" : "accepted");

  return failures == 0 ? 0 : 1;
}
