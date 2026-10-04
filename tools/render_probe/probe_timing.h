// render_probe --timing (spec 002, P-8): the processing time of each frame of
// a take — the session producing it (replay_seek of the next frame) and the
// renderer composing it with the pack (export_frame: record, GPU, readback).
// It uses only the session and renderer calls that predate spec 002, so the
// same measurement builds against the reference baseline.
#pragma once

#include <ayther/ayther_layers.h>
#include <ayther/ayther_renderer.h>
#include <ayther/ayther_session.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace ayther::probe {

struct FrameTiming {
  std::uint32_t frame = 0;
  double produce_ms = 0.0; ///< replay_seek of the frame
  double render_ms = 0.0;  ///< export_frame of the frame
};

struct TimingSummary {
  std::size_t frames = 0;
  double p50_ms = 0.0;
  double p95_ms = 0.0;
  double max_ms = 0.0;
};

/// Percentiles of produce + render over `rows`.
inline TimingSummary summarize(const std::vector<FrameTiming> &rows) {
  TimingSummary s;
  std::vector<double> total;
  total.reserve(rows.size());
  for (const FrameTiming &r : rows)
    total.push_back(r.produce_ms + r.render_ms);
  s.frames = total.size();
  if (total.empty())
    return s;
  std::sort(total.begin(), total.end());
  const auto at = [&](double q) {
    const std::size_t i = static_cast<std::size_t>(
        q * static_cast<double>(total.size() - 1) + 0.5);
    return total[std::min(i, total.size() - 1)];
  };
  s.p50_ms = at(0.50);
  s.p95_ms = at(0.95);
  s.max_ms = total.back();
  return s;
}

/// Replays frames [first, last] of `recording` through `session` and
/// `renderer`, timing each one. The renderer is (re)initialised when the
/// frame size changes; `width` x `height` is the size it is already
/// initialised at (0 = not initialised). Returns false when a frame does not
/// reproduce or render.
template <class Context>
bool time_frames(AytherSession &session, const AytherRecording &recording,
                 AytherRenderer &renderer, Context &ctx,
                 const AytherLayerStack &stack, const char *shaders,
                 std::uint32_t first, std::uint32_t last,
                 std::vector<FrameTiming> &rows, std::uint32_t width = 0,
                 std::uint32_t height = 0) {
  using clock = std::chrono::steady_clock;
  const auto ms = [](clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
  };
  bool ok = true;
  for (std::uint32_t f = first; f <= last && ok; ++f) {
    const auto t0 = clock::now();
    const FrameView *fv = session.replay_seek(recording, f);
    const auto t1 = clock::now();
    if (fv == nullptr || fv->fb_width == 0 || fv->fb_height == 0) {
      std::fprintf(stderr, "render_probe: frame %u did not reproduce\n", f);
      ok = false;
      break;
    }
    if (fv->fb_width != width || fv->fb_height != height) {
      // A mode change resizes the renderer, as a host does: its texture
      // caches (and what the preparation prewarmed) survive it.
      const bool first = width == 0;
      if (!first)
        renderer.readback_shutdown(ctx);
      width = fv->fb_width;
      height = fv->fb_height;
      const bool ready = first ? renderer.init(ctx, width, height, shaders)
                               : renderer.resize(ctx, width, height);
      if (!ready || !renderer.readback_init(ctx)) {
        std::fprintf(stderr, "render_probe: renderer init failed\n");
        ok = false;
        break;
      }
    }
    const auto t2 = clock::now();
    const std::uint8_t *pixels =
        renderer.export_frame(ctx, *fv, session.pack(), /*hd_on=*/true, &stack);
    const auto t3 = clock::now();
    if (pixels == nullptr) {
      std::fprintf(stderr, "render_probe: frame %u did not render\n", f);
      ok = false;
      break;
    }
    rows.push_back({f, ms(t1 - t0), ms(t3 - t2)});
  }
  if (width != 0) {
    renderer.readback_shutdown(ctx);
    renderer.shutdown(ctx);
  }
  return ok;
}

/// Writes `frame,produce_ms,render_ms` rows.
inline bool write_timing_csv(const std::string &path,
                             const std::vector<FrameTiming> &rows) {
  std::ofstream file(path, std::ios::binary);
  file << "frame,produce_ms,render_ms\n";
  char line[64];
  for (const FrameTiming &r : rows) {
    std::snprintf(line, sizeof(line), "%u,%.4f,%.4f\n", r.frame, r.produce_ms,
                  r.render_ms);
    file << line;
  }
  return static_cast<bool>(file);
}

} // namespace ayther::probe
