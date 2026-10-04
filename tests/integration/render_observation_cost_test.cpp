// Spec 002, BR-024 (RNF-2): the render observation (contracts.md C3) neither
// changes the frame nor slows down its production.
//
// The test core runs the same 600 synthetic frames in sessions without
// observer and in sessions that publish every frame. A pose preview is
// installed so that the pose resolver also reports its members.
//   - The FrameView content is identical frame by frame with and without
//     observer, and publishing leaves the FrameView unchanged byte for byte.
//   - The time of step() (the produce path) with and without observer differs
//     by no more than the noise measured between the runs without observer
//     (after an unmeasured warm-up run), with a floor at the DA-2 budget for
//     hidden debugging (0.05 ms).
//   - Publishing costs at most the DA-2 budget for visible debugging: 1.0 ms
//     of CPU per frame at the 95th percentile.
#include <ayther/ayther_session.h>
#include <ayther/engine/render_observer.hpp>

#include "../../tools/common/synth_rom.h"
#include "frame_view_digest.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#endif

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

namespace ro = ayther::engine::render_observation;
using ayther::AytherSession;
using ayther::FrameView;

constexpr std::size_t kFrames = 600;
constexpr std::size_t kRounds = 3;
constexpr double kHiddenBudgetUs = 50.0;    // DA-2: hidden debugging
constexpr double kVisibleBudgetUs = 1000.0; // DA-2: visible debugging, p95

class CountingObserver final : public ro::RenderObserver {
public:
  std::size_t frames = 0;
  std::size_t occurrences = 0;
  std::size_t members = 0;
  void on_render_frame(const ro::RenderFrameView &frame) noexcept override {
    ++frames;
    occurrences += frame.occurrences.size();
    for (const ro::ReplacementView &r : frame.replacements)
      members += r.members.size();
  }
};

struct Run {
  std::vector<std::uint64_t> digests;
  std::vector<double> step_us;
  std::vector<double> publish_us;
  bool publish_kept_frame = true;
  std::size_t observed_occurrences = 0;
};

double micros_since(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::micro>(
             std::chrono::steady_clock::now() - start)
      .count();
}

bool run(const std::string &rom, bool observed, Run &out) {
  CountingObserver observer;
  AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = rom;
  config.enable_audio = false;
  config.derive_core_pack = false;
  if (observed)
    config.render_observer = &observer;
  auto created = AytherSession::create(config);
  if (!created) {
    std::fprintf(stderr, "[FAIL] %s\n", created.error.message.c_str());
    return false;
  }
  std::unique_ptr<AytherSession> &session = *created;

  // A pose made of the first two sprites of frame 1, so the pose resolver
  // runs with its members every frame.
  const FrameView &first = session->step();
  if (first.sprite_occ_count < 2)
    return false;
  AytherSession::PosePreview pose;
  pose.hashes = {first.sprite_occs[0].hash, first.sprite_occs[1].hash};
  pose.rel_x = {0, static_cast<std::int16_t>(first.sprite_occs[1].screen_x -
                                             first.sprite_occs[0].screen_x)};
  pose.rel_y = {0, static_cast<std::int16_t>(first.sprite_occs[1].screen_y -
                                             first.sprite_occs[0].screen_y)};
  pose.asset = "graphics/cost-pose.png";
  session->set_pose_preview({pose});

  out = Run{};
  out.digests.reserve(kFrames);
  out.step_us.reserve(kFrames);
  for (std::size_t f = 0; f < kFrames; ++f) {
    session->set_input(0, static_cast<std::uint16_t>(f & 1U));
    const auto started = std::chrono::steady_clock::now();
    const FrameView &v = session->step();
    out.step_us.push_back(micros_since(started));
    out.digests.push_back(ayther::test::frame_digest(v));
    if (!observed)
      continue;
    std::array<unsigned char, sizeof(FrameView)> before{};
    std::memcpy(before.data(), &v, sizeof(FrameView));
    const auto publishing = std::chrono::steady_clock::now();
    session->publish_render_observation(nullptr);
    out.publish_us.push_back(micros_since(publishing));
    out.publish_kept_frame =
        out.publish_kept_frame &&
        std::memcmp(before.data(), &v, sizeof(FrameView)) == 0 &&
        ayther::test::frame_digest(v) == out.digests.back();
  }
  out.observed_occurrences = observer.occurrences;
  return !observed || observer.frames == kFrames;
}

double percentile(std::vector<double> values, double p) {
  if (values.empty())
    return 0.0;
  std::sort(values.begin(), values.end());
  const auto rank = static_cast<std::size_t>(
      p * static_cast<double>(values.size() - 1) + 0.5);
  return values[rank];
}

void configure_measurement_thread() {
#if defined(_WIN32)
  const DWORD processor = GetCurrentProcessorNumber();
  if (processor < sizeof(DWORD_PTR) * 8U) {
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1)
                                                  << processor);
  }
#endif
}
} // namespace

int main() try {
  configure_measurement_thread();
  const std::string rom = ayther::synth::canonical_rom_path();
  check(!rom.empty(),
        "the synthetic ROM is written to the temporary directory");
  if (rom.empty())
    return 1;

  // A first run, not measured, loads the core and warms the caches: without
  // it the first measured run is slower and inflates the noise.
  Run warm_up;
  bool ran = run(rom, false, warm_up);
  std::vector<Run> off(kRounds);
  std::vector<Run> on(kRounds);
  for (std::size_t r = 0; r < kRounds; ++r) {
    // Alternate the order so a warm-up or a drift does not favour one side.
    const bool on_first = (r % 2U) != 0U;
    ran = ran && run(rom, on_first, on_first ? on[r] : off[r]);
    ran = ran && run(rom, !on_first, on_first ? off[r] : on[r]);
  }
  check(ran, "600 frames run with and without observer, three rounds each");
  if (!ran)
    return 1;

  bool same_frames = true;
  bool kept = true;
  for (std::size_t r = 0; r < kRounds; ++r) {
    same_frames = same_frames && off[r].digests == on[r].digests &&
                  off[r].digests == off[0].digests;
    kept = kept && on[r].publish_kept_frame;
  }
  check(same_frames,
        "RNF-2: the FrameView content is identical frame by frame with and "
        "without observer");
  check(kept, "RNF-2: publishing leaves the FrameView unchanged byte for byte");
  check(on[0].observed_occurrences > 0,
        "the observer saw the frames' occurrences (not a vacuous run)");

  std::vector<double> off_medians;
  std::vector<double> on_medians;
  std::vector<double> publish_all;
  for (std::size_t r = 0; r < kRounds; ++r) {
    off_medians.push_back(percentile(off[r].step_us, 0.5));
    on_medians.push_back(percentile(on[r].step_us, 0.5));
    publish_all.insert(publish_all.end(), on[r].publish_us.begin(),
                       on[r].publish_us.end());
    std::printf(
        "render_observation_cost round=%zu frames=%zu "
        "off_step_median_us=%.2f off_step_p95_us=%.2f "
        "on_step_median_us=%.2f on_step_p95_us=%.2f "
        "publish_median_us=%.2f publish_p95_us=%.2f\n",
        r + 1, kFrames, off_medians.back(), percentile(off[r].step_us, 0.95),
        on_medians.back(), percentile(on[r].step_us, 0.95),
        percentile(on[r].publish_us, 0.5), percentile(on[r].publish_us, 0.95));
  }
  const auto [off_lo, off_hi] =
      std::minmax_element(off_medians.begin(), off_medians.end());
  const double noise_us = *off_hi - *off_lo;
  const double off_median = percentile(off_medians, 0.5);
  const double on_median = percentile(on_medians, 0.5);
  const double difference_us = on_median - off_median;
  const double publish_p95 = percentile(publish_all, 0.95);
  std::printf("render_observation_cost summary off_median_us=%.2f "
              "on_median_us=%.2f difference_us=%.2f noise_us=%.2f "
              "publish_p95_us=%.2f\n",
              off_median, on_median, difference_us, noise_us, publish_p95);
  check(std::abs(difference_us) <= std::max(noise_us, kHiddenBudgetUs),
        "RNF-2: step() with observer differs from step() without it by no "
        "more than the measured noise (floor: 0.05 ms)");
  check(publish_p95 <= kVisibleBudgetUs,
        "RNF-2: publishing costs <= 1.0 ms per frame at the 95th percentile");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
