#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ayther::engine {

inline constexpr std::uint64_t max_analysis_duration_ns = 900'000'000'000ULL;
inline constexpr std::uint64_t max_analysis_frames = 54'000;
inline constexpr std::uint64_t max_analysis_events = 1'000'000;
inline constexpr std::uint64_t max_analysis_identities = 4'096;
inline constexpr std::uint64_t max_analysis_private_bytes = 256ULL * 1024 * 1024;
inline constexpr std::uint64_t max_analysis_intervals = 10'000;
inline constexpr std::uint64_t max_analysis_work_ns = 120'000'000'000ULL;
inline constexpr std::uint64_t max_analysis_cancel_ns = 2'000'000'000ULL;

enum class AnalysisExecutionContext : std::uint8_t { worker, audio, ui };

struct AnalysisInputSize {
  std::uint64_t duration_ns{};
  std::uint64_t frames{};
  std::uint64_t events{};
  std::uint64_t identities{};
};

struct AnalysisAdmission {
  AnalysisInputSize input{};
  AnalysisExecutionContext context{AnalysisExecutionContext::worker};
};

enum class AnalysisAdmissionStatus : std::uint8_t {
  accepted,
  input_limit,
  wrong_execution_context,
};

struct AnalysisAdmissionResult {
  AnalysisAdmissionStatus status{AnalysisAdmissionStatus::accepted};
  std::string limit;
  bool truncated{};
};

[[nodiscard]] inline AnalysisAdmissionResult
check_analysis_admission(const AnalysisAdmission &request) {
  if (request.context != AnalysisExecutionContext::worker)
    return {AnalysisAdmissionStatus::wrong_execution_context,
            "offline_worker_context", false};
  const auto &input = request.input;
  if (input.duration_ns > max_analysis_duration_ns)
    return {AnalysisAdmissionStatus::input_limit, "duration", false};
  if (input.frames > max_analysis_frames)
    return {AnalysisAdmissionStatus::input_limit, "frames", false};
  if (input.events > max_analysis_events)
    return {AnalysisAdmissionStatus::input_limit, "events", false};
  if (input.identities > max_analysis_identities)
    return {AnalysisAdmissionStatus::input_limit, "identities", false};
  return {};
}

struct ProjectId {
  std::uint64_t value{};
  friend constexpr bool operator==(ProjectId, ProjectId) noexcept = default;
};

struct AnalysisLease {
  ProjectId project{};
  std::uint64_t serial{};
};

class AnalysisProjectGate {
public:
  [[nodiscard]] std::optional<AnalysisLease> try_acquire(ProjectId project) {
    const std::scoped_lock lock{mutex_};
    if (std::ranges::any_of(active_, [&](const auto &lease) {
          return lease.project == project;
        }))
      return std::nullopt;
    AnalysisLease lease{project, ++serial_};
    active_.push_back(lease);
    return lease;
  }

  void release(AnalysisLease lease) {
    const std::scoped_lock lock{mutex_};
    std::erase_if(active_, [&](const auto &active) {
      return active.project == lease.project && active.serial == lease.serial;
    });
  }

private:
  std::mutex mutex_;
  std::uint64_t serial_{};
  std::vector<AnalysisLease> active_;
};

struct AnalysisWorkMetrics {
  std::uint64_t private_bytes{};
  std::uint64_t intervals{};
  std::uint64_t elapsed_ns{};
  std::uint64_t cancellation_latency_ns{};
  std::uint64_t covered_events{};
  std::uint64_t total_events{};
  bool mandatory_scenario{};
};

enum class AnalysisWorkStatus : std::uint8_t { complete, analysis_limit };

struct AnalysisWorkResult {
  AnalysisWorkStatus status{AnalysisWorkStatus::complete};
  bool partial{};
  bool acceptance_blocked{};
  std::string limit;
  std::uint64_t covered_events{};
  std::uint64_t total_events{};
};

[[nodiscard]] inline AnalysisWorkResult
evaluate_analysis_work(const AnalysisWorkMetrics &metrics) {
  std::string limit;
  if (metrics.private_bytes > max_analysis_private_bytes)
    limit = "private_memory";
  else if (metrics.intervals > max_analysis_intervals)
    limit = "intervals";
  else if (metrics.elapsed_ns > max_analysis_work_ns)
    limit = "elapsed_time";
  else if (metrics.cancellation_latency_ns > max_analysis_cancel_ns)
    limit = "cancellation_latency";
  if (limit.empty())
    return {AnalysisWorkStatus::complete, false, false, {},
            metrics.covered_events, metrics.total_events};
  return {AnalysisWorkStatus::analysis_limit, true,
          metrics.mandatory_scenario, std::move(limit), metrics.covered_events,
          metrics.total_events};
}

} // namespace ayther::engine
