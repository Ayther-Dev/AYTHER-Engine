#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace ayther::engine {

struct QualityOracle {
  std::string case_id;
  std::string family;
  bool expected_match{};
  std::string identity;
  std::optional<std::uint64_t> begin;
  std::optional<std::uint64_t> end;
};

struct QualityPrediction {
  std::string case_id;
  std::string identity;
  std::optional<std::uint64_t> begin;
  std::optional<std::uint64_t> end;
  bool partial{};
  bool ambiguous{};
};

struct QualityFamilyMetrics {
  std::string family;
  std::size_t true_positive{};
  std::size_t false_positive{};
  std::size_t false_negative{};
  std::optional<double> precision;
  std::optional<double> recall;
};

struct CorpusQualityReport {
  std::size_t true_positive{};
  std::size_t false_positive{};
  std::size_t false_negative{};
  std::size_t duplicates{};
  std::size_t partial_predictions{};
  std::size_t ambiguous_predictions{};
  std::size_t automatic_entries{};
  std::size_t false_resolved_positions{};
  std::optional<double> precision;
  std::optional<double> recall;
  std::vector<QualityFamilyMetrics> families;
};

namespace detail {
[[nodiscard]] constexpr std::uint64_t
quality_distance(std::uint64_t left, std::uint64_t right) noexcept {
  return left > right ? left - right : right - left;
}

inline void finish_metrics(QualityFamilyMetrics &metrics) {
  const auto precision_denominator =
      metrics.true_positive + metrics.false_positive;
  const auto recall_denominator =
      metrics.true_positive + metrics.false_negative;
  if (precision_denominator != 0)
    metrics.precision = static_cast<double>(metrics.true_positive) /
                        static_cast<double>(precision_denominator);
  if (recall_denominator != 0)
    metrics.recall = static_cast<double>(metrics.true_positive) /
                     static_cast<double>(recall_denominator);
}
} // namespace detail

[[nodiscard]] inline CorpusQualityReport
evaluate_corpus_quality(const std::vector<QualityOracle> &oracles,
                        const std::vector<QualityPrediction> &predictions,
                        std::uint64_t endpoint_tolerance) {
  CorpusQualityReport report;
  for (const auto &oracle : oracles) {
    auto family = std::ranges::find_if(report.families, [&](const auto &value) {
      return value.family == oracle.family;
    });
    if (family == report.families.end()) {
      report.families.push_back(
          {oracle.family, 0, 0, 0, std::nullopt, std::nullopt});
      family = std::prev(report.families.end());
    }
    std::vector<const QualityPrediction *> candidates;
    for (const auto &prediction : predictions)
      if (prediction.case_id == oracle.case_id)
        candidates.push_back(&prediction);
    if (candidates.size() > 1)
      report.duplicates += candidates.size() - 1;
    for (const auto *prediction : candidates) {
      report.partial_predictions += prediction->partial ? 1U : 0U;
      report.ambiguous_predictions += prediction->ambiguous ? 1U : 0U;
    }

    if (!oracle.expected_match) {
      report.false_positive += candidates.size();
      family->false_positive += candidates.size();
      report.automatic_entries += candidates.size();
      report.false_resolved_positions += static_cast<std::size_t>(
          std::ranges::count_if(candidates, [](const auto *prediction) {
            return !prediction->ambiguous;
          }));
      continue;
    }

    bool matched = false;
    for (const auto *prediction : candidates) {
      const bool begin_matches =
          !oracle.begin ||
          (prediction->begin &&
           detail::quality_distance(*oracle.begin, *prediction->begin) <=
               endpoint_tolerance);
      const bool end_matches =
          !oracle.end ||
          (prediction->end &&
           detail::quality_distance(*oracle.end, *prediction->end) <=
               endpoint_tolerance);
      if (!matched && prediction->identity == oracle.identity &&
          begin_matches && end_matches) {
        matched = true;
        ++report.true_positive;
        ++family->true_positive;
      } else {
        ++report.false_positive;
        ++family->false_positive;
      }
    }
    if (!matched) {
      ++report.false_negative;
      ++family->false_negative;
    }
  }

  for (auto &family : report.families)
    detail::finish_metrics(family);
  QualityFamilyMetrics totals{"all",
                              report.true_positive,
                              report.false_positive,
                              report.false_negative,
                              std::nullopt,
                              std::nullopt};
  detail::finish_metrics(totals);
  report.precision = totals.precision;
  report.recall = totals.recall;
  return report;
}

} // namespace ayther::engine
