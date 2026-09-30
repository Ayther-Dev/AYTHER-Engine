#include "../../../tools/common/synth_rom.h"

#include <ayther/ayther_session.h>
#include <ayther/engine/audio_fact_queue.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <numeric>
#include <optional>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#endif

namespace obs = ayther::engine::audio_observation;

namespace {

constexpr std::size_t measured_frames = 2'048U;
constexpr std::size_t pair_count = 3U;
constexpr std::uint64_t p95_ratio_numerator = 105U;
constexpr std::uint64_t p95_ratio_denominator = 100U;
constexpr std::uint64_t p99_delta_limit_nanoseconds = 1'000'000U;

struct ObservationSink {
  using Queue = obs::BoundedFactQueue<128U>;

  std::array<Queue, 9> queues;
  std::array<std::uint64_t, 9> last_sequence{};
  std::uint64_t attempted{};
  std::uint64_t facts{};
  std::uint64_t pcm_blocks{};
  std::uint64_t losses{};
  bool valid{true};

  static void receive_fact(void *const context,
                           const obs::FactView &fact) noexcept {
    auto &sink = *static_cast<ObservationSink *>(context);
    if (fact.id.producer == 0U ||
        fact.id.producer > sink.last_sequence.size()) {
      sink.valid = false;
      ++sink.losses;
      return;
    }
    ++sink.attempted;
    if (sink.queues[fact.id.producer - 1U].try_push(fact) !=
        obs::FactPushResult::accepted)
      ++sink.losses;
  }

  static void consume_fact(void *const context,
                           const obs::FactView &fact) noexcept {
    auto &sink = *static_cast<ObservationSink *>(context);
    auto &previous = sink.last_sequence[fact.id.producer - 1U];
    if (fact.id.sequence != previous + 1U)
      sink.valid = false;
    previous = fact.id.sequence;
    ++sink.facts;
  }

  static void receive_pcm(void *const context, const obs::PcmView &) noexcept {
    ++static_cast<ObservationSink *>(context)->pcm_blocks;
  }

  void drain() noexcept {
    for (auto &queue : queues)
      while (queue.try_consume(this, consume_fact)) {
      }
  }
};

struct RunResult {
  std::vector<std::uint64_t> work_nanoseconds;
  std::vector<std::uint64_t> frame_indices;
  std::vector<std::uint8_t> state;
  std::array<std::uint64_t, 15> decisions{};
  std::uint64_t facts{};
  std::uint64_t pcm_blocks{};
  std::uint64_t losses{};
  bool valid{};
};

struct RunContext {
  std::unique_ptr<ObservationSink> sink;
  std::unique_ptr<ayther::AytherSession> session;
  RunResult result;
  bool observed{};
};

std::uint64_t work_clock_nanoseconds() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool configure_measurement_thread() noexcept {
#if defined(_WIN32)
  const auto processor = GetCurrentProcessorNumber();
  if (processor >= sizeof(DWORD_PTR) * 8U)
    return false;
  const auto affinity = static_cast<DWORD_PTR>(1) << processor;
  return SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) != 0 &&
         SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != 0 &&
         SetThreadAffinityMask(GetCurrentThread(), affinity) != 0;
#else
  return true;
#endif
}

std::optional<RunContext> start(const std::filesystem::path &rom_path,
                                const bool observed) {
  RunContext context;
  context.sink = std::make_unique<ObservationSink>();
  context.observed = observed;
  ayther::AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = rom_path.string();
  config.enable_audio = false;
  config.derive_core_pack = false;
  if (observed) {
    config.audio_observer = {context.sink.get(), ObservationSink::receive_fact,
                             ObservationSink::receive_pcm};
  }

  auto created = ayther::AytherSession::create(config);
  if (!created)
    return std::nullopt;
  context.session = std::move(*created);
  context.session->set_audio_runtime_substitution(true);
  context.result.work_nanoseconds.reserve(measured_frames);
  context.result.frame_indices.reserve(measured_frames);
  return context;
}

void step(RunContext &context, const std::size_t frame) {
  const auto started = work_clock_nanoseconds();
  context.session->set_input(0, static_cast<std::uint16_t>(frame & 1U));
  const auto &view = context.session->step();
  if (context.observed)
    context.sink->drain();
  const auto finished = work_clock_nanoseconds();
  context.result.frame_indices.push_back(view.frame_index);
  context.result.work_nanoseconds.push_back(finished - started);
}

std::optional<RunResult> finish(RunContext &context) {
  auto &result = context.result;
  auto &session = *context.session;
  if (!session.serialize(result.state))
    return std::nullopt;
  session.audio_live_match_stats(&result.decisions[0], &result.decisions[1],
                                 &result.decisions[2], &result.decisions[3]);
  session.audio_unified_stats(&result.decisions[4], &result.decisions[5],
                              &result.decisions[6], &result.decisions[7]);
  session.audio_fallback_stats(&result.decisions[8], &result.decisions[9]);
  session.audio_resume_stats(&result.decisions[10], &result.decisions[11],
                             &result.decisions[12]);
  result.decisions[13] = session.audio_event_count();
  result.decisions[14] = session.audio_event_assignment_count();
  result.facts = context.sink->facts;
  result.pcm_blocks = context.sink->pcm_blocks;
  result.losses = context.sink->losses;
  result.valid =
      context.sink->valid && context.sink->losses == 0U &&
      (!context.observed || (context.sink->facts > 0U &&
                             context.sink->facts == context.sink->attempted)) &&
      (context.observed ||
       (context.sink->facts == 0U && context.sink->pcm_blocks == 0U));
  return result;
}

struct RunPair {
  RunResult disabled;
  RunResult enabled;
};

std::optional<RunPair> run_pair(const std::filesystem::path &rom_path,
                                const bool enabled_first) {
  auto first = start(rom_path, enabled_first);
  auto second = start(rom_path, !enabled_first);
  if (!first || !second)
    return std::nullopt;
  for (std::size_t frame = 0; frame < measured_frames; ++frame) {
    if ((frame & 1U) == 0U) {
      step(*first, frame);
      step(*second, frame);
    } else {
      step(*second, frame);
      step(*first, frame);
    }
  }
  auto first_result = finish(*first);
  auto second_result = finish(*second);
  if (!first_result || !second_result)
    return std::nullopt;
  return enabled_first
             ? RunPair{std::move(*second_result), std::move(*first_result)}
             : RunPair{std::move(*first_result), std::move(*second_result)};
}

std::uint64_t nearest_rank(std::vector<std::uint64_t> values,
                           const std::size_t percentile) {
  if (values.empty() || percentile == 0U || percentile > 100U)
    return 0U;
  std::sort(values.begin(), values.end());
  const auto rank = (percentile * values.size() + 99U) / 100U;
  return values[rank - 1U];
}

double effective_frames_per_second(
    const std::vector<std::uint64_t> &work_nanoseconds) noexcept {
  const auto total = std::accumulate(work_nanoseconds.begin(),
                                     work_nanoseconds.end(), std::uint64_t{0});
  return total == 0U ? 0.0
                     : static_cast<double>(work_nanoseconds.size()) * 1.0e9 /
                           static_cast<double>(total);
}

bool decisions_match(const RunResult &disabled,
                     const RunResult &enabled) noexcept {
  return disabled.state == enabled.state &&
         disabled.frame_indices == enabled.frame_indices &&
         disabled.decisions == enabled.decisions;
}

struct Percentiles {
  std::uint64_t p95 = 0;
  std::uint64_t p99 = 0;
};

bool within_budget(const Percentiles disabled,
                   const Percentiles enabled) noexcept {
  if (disabled.p95 == 0U)
    return false;
  const bool p95_ok =
      enabled.p95 * p95_ratio_denominator <= disabled.p95 * p95_ratio_numerator;
  const bool p99_ok = enabled.p99 <= disabled.p99 + p99_delta_limit_nanoseconds;
  return p95_ok && p99_ok;
}

} // namespace

int main() try {
  if (!configure_measurement_thread())
    return 5;
  ayther::synth::Rom rom("AYTHER QA FRAME COST 2048");
  ayther::synth::program_canonical(rom, true);
  const auto rom_path =
      std::filesystem::current_path() / "frame-instrumentation-cost.md";
  if (!rom.save(rom_path))
    return 2;

  bool behavior_passed = true;
  bool budget_passed = true;
  for (std::size_t pair = 0; pair < pair_count; ++pair) {
    const bool enabled_first = pair % 2U != 0U;
    const auto measured = run_pair(rom_path, enabled_first);
    if (!measured)
      return 3;
    const auto &disabled = measured->disabled;
    const auto &enabled = measured->enabled;
    const Percentiles disabled_percentiles{
        nearest_rank(disabled.work_nanoseconds, 95U),
        nearest_rank(disabled.work_nanoseconds, 99U)};
    const Percentiles enabled_percentiles{
        nearest_rank(enabled.work_nanoseconds, 95U),
        nearest_rank(enabled.work_nanoseconds, 99U)};
    const bool behavior_pair_passed =
        disabled.valid && enabled.valid && decisions_match(disabled, enabled);
    behavior_passed = behavior_passed && behavior_pair_passed;
    const bool pair_passed =
        behavior_pair_passed &&
        within_budget(disabled_percentiles, enabled_percentiles);
    budget_passed = budget_passed && pair_passed;
    std::printf("frame_instrumentation_cost pair=%zu order=%s frames=%zu "
                "disabled_p95_ns=%llu enabled_p95_ns=%llu disabled_p99_ns=%llu "
                "enabled_p99_ns=%llu disabled_work_fps=%.3f "
                "enabled_work_fps=%.3f pacing_ns=0 facts=%llu pcm=%llu "
                "losses=%llu decisions_equal=%s budget=%s\n",
                pair + 1U,
                enabled_first ? "enabled-disabled" : "disabled-enabled",
                measured_frames,
                static_cast<unsigned long long>(disabled_percentiles.p95),
                static_cast<unsigned long long>(enabled_percentiles.p95),
                static_cast<unsigned long long>(disabled_percentiles.p99),
                static_cast<unsigned long long>(enabled_percentiles.p99),
                effective_frames_per_second(disabled.work_nanoseconds),
                effective_frames_per_second(enabled.work_nanoseconds),
                static_cast<unsigned long long>(enabled.facts),
                static_cast<unsigned long long>(enabled.pcm_blocks),
                static_cast<unsigned long long>(enabled.losses),
                decisions_match(disabled, enabled) ? "equal" : "different",
                pair_passed ? "passed" : "failed");
  }
  std::printf("frame_instrumentation_cost aggregate=all_pairs pairs=%zu "
              "clock=%s behavior=%s budget=%s\n",
              pair_count,
#if defined(_WIN32)
              "steady_wall_pinned_high_priority",
#else
              "steady_wall",
#endif
              behavior_passed ? "passed" : "failed",
              budget_passed ? "passed" : "failed");
  return behavior_passed && budget_passed ? 0 : 1;
} catch (...) {
  return 4;
}
