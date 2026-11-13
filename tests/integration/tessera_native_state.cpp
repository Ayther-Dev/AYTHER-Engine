// ayther.integration.tessera_native_state (spec 008, TP-302.1 to TP-302.3):
// rewind, takes with keyframes and the emulator PCM on the native megadrive
// machine, over the synthetic scene of tests/support/synth_md_scene.hpp.
//
//   - Rewind (TP-302.1): 700 frames with the default 10 s window, then 590
//     rewind steps; each one shows the machine state of the frame it lands on
//     (VRAM, CRAM, work RAM), and running forward again repeats the original
//     run. The memory of the full window is printed and, with gpgx present,
//     must stay within 1.25 times gpgx's (RNF-5, plan §2.10).
//   - Takes (TP-302.2): a 700-frame take with input carries its provenance
//     and two keyframes, survives save/load as .arp, and a fresh session seeks
//     it (in order, by jumps through the keyframes and backwards) to the
//     states of the live run.
//   - PCM (TP-302.3): the take's first note, captured with the FM channel 1
//     mask, has the player's length (44.1 kHz per frame) and no seam at a
//     frame boundary: no sample step stands out from the waveform's own. The
//     native machine does not take AUDIO_MUTE yet (rc.8), so its capture is
//     the full mix; the length and seam rules hold either way.
//
// Tessera always; gpgx too when the fork core of core.lock (or
// AYTHER_PARITY_GPGX_CORE) is present, on its own checks plus the memory
// ratio.
#include "../support/synth_md_scene.hpp"

#include <ayther/ayther_recording.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using namespace ayther::test::md_scene;
using ayther::AytherRecording;

constexpr int kRewindRun = 700;
constexpr int kRewindSteps = 590;
constexpr int kForwardAgain = 100;
constexpr std::uint32_t kTakeFrames = 700;

std::uint64_t fnv(const std::uint8_t *data, std::size_t size,
                  std::uint64_t hash) {
  for (std::size_t i = 0; i < size; ++i)
    hash = (hash ^ data[i]) * 0x100000001B3ull;
  return hash;
}

/// The machine state a frame leaves: VRAM (with the scroll table), CRAM and
/// work RAM (with the frame counter).
std::uint64_t state_hash(const Session &s) {
  std::uint64_t hash = 0xCBF29CE484222325ull;
  std::size_t size = 0;
  if (const auto *vram = s.video_ram(&size))
    hash = fnv(vram, size, hash);
  if (const auto *cram = s.color_ram(&size))
    hash = fnv(cram, size, hash);
  if (const auto *wram = s.work_ram(&size))
    hash = fnv(wram, size, hash);
  return hash;
}

/// A pad pattern for the take: the scene ignores it, the take keeps it.
std::uint16_t pad_at(std::uint32_t frame) {
  return static_cast<std::uint16_t>((frame / 7u) % 3u == 0 ? 0x0100u : 0u);
}

struct RewindResult {
  bool ok = false;
  std::size_t memory = 0;
  std::size_t frames = 0;
  std::size_t state_bytes = 0;
  double rewind_ms = 0.0;
};

RewindResult rewind_run(const std::string &p,
                        const std::vector<std::uint8_t> &rom,
                        const std::filesystem::path &core) {
  RewindResult out;
  auto s = open(p, rom, core);
  if (!s)
    return out;
  // One frame first, so the window is sized with the machine's own rate.
  (void)s->step();
  s->enable_rewind(true, 10);
  check(s->rewind_enabled(), p + ": rewind turns on");
  out.state_bytes = s->serialize_size();
  std::vector<std::uint64_t> hashes(kRewindRun + 1, 0);
  for (int f = 2; f <= kRewindRun; ++f) {
    (void)s->step();
    hashes[static_cast<std::size_t>(f)] = state_hash(*s);
  }
  out.frames = s->rewind_frames();
  out.memory = s->rewind_memory_bytes();
  check(out.frames >= 595 && out.frames <= 605,
        p + ": the window holds 10 s of frames (" + std::to_string(out.frames) +
            ")");

  int landed = 0;
  const auto start = std::chrono::steady_clock::now();
  for (int k = 1; k <= kRewindSteps; ++k) {
    const auto *view = s->rewind_step();
    if (!view)
      break;
    if (state_hash(*s) == hashes[static_cast<std::size_t>(kRewindRun - k)])
      ++landed;
  }
  out.rewind_ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - start)
                      .count() /
                  kRewindSteps;
  check(landed == kRewindSteps,
        p + ": every rewind step shows the state of its frame (" +
            std::to_string(landed) + "/" + std::to_string(kRewindSteps) + ")");
  int again = 0;
  for (int f = kRewindRun - kRewindSteps + 1;
       f <= kRewindRun - kRewindSteps + kForwardAgain; ++f) {
    (void)s->step();
    if (state_hash(*s) == hashes[static_cast<std::size_t>(f)])
      ++again;
  }
  check(again == kForwardAgain,
        p + ": running forward again repeats the original frames (" +
            std::to_string(again) + "/" + std::to_string(kForwardAgain) + ")");
  std::printf("  %s: rewind %zu frames, %zu B (state %zu B), %.3f ms/step\n",
              p.c_str(), out.frames, out.memory, out.state_bytes,
              out.rewind_ms);
  out.ok = true;
  return out;
}

struct TakeResult {
  bool ok = false;
  std::size_t pcm_frames = 0;
};

TakeResult take_run(const std::string &p, const std::vector<std::uint8_t> &rom,
                    const std::filesystem::path &core, double &fps) {
  TakeResult out;
  auto live = open(p, rom, core);
  if (!live)
    return out;
  for (int f = 0; f < 10; ++f)
    (void)live->step();
  live->record_start();
  std::vector<std::uint64_t> hashes;
  for (std::uint32_t f = 0; f < kTakeFrames; ++f) {
    live->set_input(0, pad_at(f));
    (void)live->step();
    hashes.push_back(state_hash(*live));
  }
  fps = live->timing_fps();
  AytherRecording take = live->take_recording();
  check(take.emulator_id == (p == "tessera" ? "tessera" : "gpgx"),
        p + ": the take says which emulator made it (" + take.emulator_id +
            ")");
  check(take.frame_count() == kTakeFrames && take.inputs[14] == pad_at(14),
        p + ": the take has every frame and its input");
  check(take.keyframes.size() >= 2, p + ": keyframes every 300 frames (" +
                                        std::to_string(take.keyframes.size()) +
                                        ")");

  const auto file =
      std::filesystem::temp_directory_path() / ("ayther_tp302_" + p + ".arp");
  check(take.save(file.string()), p + ": the take is saved");
  auto loaded = AytherRecording::load(file.string());
  check(loaded.has_value() && loaded->emulator_id == take.emulator_id &&
            loaded->inputs == take.inputs &&
            loaded->keyframes.size() == take.keyframes.size(),
        p + ": and loads back with provenance, input and keyframes");
  if (!loaded)
    return out;

  auto replay = open(p, rom, core);
  if (!replay)
    return out;
  // The emulator's own PCM: with the voice router on (gpgx's default)
  // capture_channel_pcm renders the router's resynthesis instead.
  replay->set_voice_router(false);
  std::vector<std::uint32_t> order;
  for (std::uint32_t f = 0; f < 40; ++f)
    order.push_back(f);
  for (const std::uint32_t f : {650u, 651u, 320u, 699u, 100u, 5u, 601u})
    order.push_back(f);
  int matched = 0;
  std::string missed;
  for (const std::uint32_t f : order) {
    const auto *view = replay->replay_seek(*loaded, f, /*quiet=*/true);
    if (view && state_hash(*replay) == hashes[f])
      ++matched;
    else if (missed.size() < 60)
      missed += " " + std::to_string(f);
  }
  check(matched == static_cast<int>(order.size()),
        p + ": seeking the loaded take gives the live frames (" +
            std::to_string(matched) + "/" + std::to_string(order.size()) +
            (missed.empty() ? "" : "; missed" + missed) + ")");

  // PCM: FM channel 1 alone in its first note (take frames are the live
  // frames + 10, so 25..55 of the scene are 15..45 of the take).
  std::vector<std::int16_t> pcm;
  constexpr std::uint32_t kStart = 15, kWindow = 30;
  out.pcm_frames =
      replay->capture_channel_pcm(*loaded, kStart, kWindow, 1u, pcm);
  const double expected = kWindow * 44100.0 / (fps > 1.0 ? fps : 60.0);
  check(std::fabs(static_cast<double>(out.pcm_frames) - expected) <= 3.0,
        p + ": the captured PCM has the player's length (" +
            std::to_string(out.pcm_frames) + " frames, expected " +
            std::to_string(static_cast<int>(expected + 0.5)) + ")");
  std::vector<int> steps;
  int peak = 0;
  // The first frame after the seek is the restore's own settling (gpgx
  // jumps there from silence); a seam is a step between later frames.
  const std::size_t settle =
      2 * static_cast<std::size_t>(44100.0 / (fps > 1.0 ? fps : 60.0) + 1.0);
  for (std::size_t i = settle + 2; i + 1 < pcm.size(); i += 2) {
    steps.push_back(std::abs(pcm[i] - pcm[i - 2]));
    peak = (std::max)(peak, std::abs(static_cast<int>(pcm[i])));
  }
  std::vector<int> sorted = steps;
  std::sort(sorted.begin(), sorted.end());
  const int largest = sorted.empty() ? 0 : sorted.back();
  const auto at = static_cast<std::size_t>(
      std::max_element(steps.begin(), steps.end()) - steps.begin());
  const int p999 = sorted.empty() ? 0 : sorted[sorted.size() * 999 / 1000];
  check(peak > 2000,
        p + ": the note sounds (peak " + std::to_string(peak) + ")");
  check(largest <= p999 + p999 / 4 + 8,
        p + ": no seam: the largest sample step (" + std::to_string(largest) +
            " at sample " + std::to_string(at + 1 + settle / 2) + " of " +
            std::to_string(pcm.size() / 2) + ") is the waveform's own (p99.9 " +
            std::to_string(p999) + ")");
  out.ok = true;
  return out;
}

} // namespace

int main() try {
  const auto rom = build_rom();
  if (rom.empty()) {
    std::printf("[FAIL] the synthetic ROM could not be written\n");
    return 1;
  }
  const auto core = gpgx_core();
  if (!core.available() && core.failure_exit_code != 77) {
    std::printf("[FAIL] %s\n", core.diagnostic.c_str());
    return 1;
  }

  // Device audio on the native session: accepted, non-fatal without one.
  {
    auto audible = open("tessera", rom, {}, /*enable_audio=*/true);
    if (audible) {
      for (int f = 0; f < 30; ++f)
        (void)audible->step();
      check(true, "tessera: a session with device audio runs");
    }
  }

  const RewindResult tessera = rewind_run("tessera", rom, {});
  double tessera_fps = 0.0;
  (void)take_run("tessera", rom, {}, tessera_fps);

  if (!core.available()) {
    std::printf("[SKIP] gpgx: %s\n", core.diagnostic.c_str());
  } else {
    const RewindResult gpgx = rewind_run("gpgx", rom, core.path);
    double gpgx_fps = 0.0;
    (void)take_run("gpgx", rom, core.path, gpgx_fps);
    if (tessera.ok && gpgx.ok && gpgx.memory > 0) {
      const double ratio = static_cast<double>(tessera.memory) /
                           static_cast<double>(gpgx.memory);
      check(ratio <= 1.25, "rewind memory: tessera / gpgx = " +
                               std::to_string(ratio) + " (<= 1.25)");
      std::printf("EVIDENCE {\"rewind_frames\":{\"tessera\":%zu,\"gpgx\":%zu},"
                  "\"rewind_memory_bytes\":{\"tessera\":%zu,\"gpgx\":%zu},"
                  "\"state_bytes\":{\"tessera\":%zu,\"gpgx\":%zu},"
                  "\"rewind_ms_per_step\":{\"tessera\":%.3f,\"gpgx\":%.3f},"
                  "\"ratio\":%.4f}\n",
                  tessera.frames, gpgx.frames, tessera.memory, gpgx.memory,
                  tessera.state_bytes, gpgx.state_bytes, tessera.rewind_ms,
                  gpgx.rewind_ms, ratio);
    }
  }
  std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "OK", failures);
  return failures ? 1 : 0;
} catch (const std::exception &error) {
  std::printf("[FAIL] exception: %s\n", error.what());
  return 1;
}
