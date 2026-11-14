// Spec 008 (TP-005.14, D-7): the inputs of a take (.arp) converted to a
// TSTRACE1 trace and replayed in an AytherSession with the test core give the
// same frame hashes as the take itself.
#include <ayther/ayther_recording.h>
#include <ayther/ayther_session.h>

#include "../../tools/common/synth_rom.h"
#include "../../tools/parity_bench/trace_arp.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}
namespace fs = std::filesystem;
using ayther::parity::kMappedPadMask;

std::unique_ptr<ayther::AytherSession> open_session(const std::string &rom) {
  ayther::AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = rom;
  config.enable_audio = false;
  config.derive_core_pack = false;
  auto created = ayther::AytherSession::create(config);
  if (!created)
    return nullptr;
  return std::move(*created);
}

std::uint64_t hash_frame(const ayther::FrameView &v, std::uint64_t previous) {
  if (v.fb_pixels == nullptr)
    return previous;
  std::uint64_t h = 1469598103934665603ULL;
  const auto *p = static_cast<const std::uint8_t *>(v.fb_pixels);
  for (std::uint32_t y = 0; y < v.fb_height; ++y)
    for (std::uint32_t i = 0; i < v.fb_pitch; ++i)
      h = (h ^ p[std::size_t(y) * v.fb_pitch + i]) * 1099511628211ULL;
  return h ^ (std::uint64_t(v.fb_width) << 32) ^ v.fb_height;
}

std::uint16_t pad_for(int f) {
  return static_cast<std::uint16_t>((f * 37 + 5) & kMappedPadMask);
}

std::vector<std::uint8_t> read_all(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
} // namespace

int main() {
  const std::string rom = ayther::synth::canonical_rom_path();
  const fs::path dir = fs::temp_directory_path() / "ayther_parity_trace";
  fs::create_directories(dir);
  const fs::path take = dir / "take.arp";
  const fs::path trace_path = dir / "take.trace";
  constexpr int frames = 40;

  // 1. record a take and the hashes of its frames
  std::vector<std::uint64_t> take_hashes;
  {
    auto s = open_session(rom);
    check(s != nullptr, "session opens with the test core");
    if (!s)
      return 1;
    s->record_start();
    std::uint64_t h = 0;
    for (int f = 0; f < frames; ++f) {
      s->set_input(0, pad_for(f));
      h = hash_frame(s->step(), h);
      take_hashes.push_back(h);
    }
    s->record_stop();
    check(s->take_recording().save(take.string()), "take saved");
  }

  // 2. control mapping is a bijection over the mapped bits
  {
    bool ok = true;
    for (std::uint32_t pad = 0; pad <= kMappedPadMask; ++pad) {
      const auto p = static_cast<std::uint16_t>(pad & kMappedPadMask);
      ok = ok && ayther::parity::pad_from_controls(
                     ayther::parity::controls_from_pad(p)) == p;
    }
    check(ok, "pad -> 8 controls -> pad round-trips");
    const auto c = ayther::parity::controls_from_pad(1U << 4); // up
    check(c[0] == 1 && c[1] == 0 && c[4] == 0, "RetroPad up maps to control 0");
  }

  // 3. conversion through the CLI entry point
  const std::string sha_a(64, 'a'), sha_b(64, 'b');
  const int rc = ayther::parity::trace_from_arp_command(
      take.string(), trace_path.string(), sha_a, sha_b);
  check(rc == 0, "trace-from-arp succeeds");
  const auto bytes = read_all(trace_path);
  check(bytes.size() > 8 &&
            std::string(bytes.begin(), bytes.begin() + 8) == "TSTRACE1",
        "output starts with the TSTRACE1 magic");

  const auto trace = ayther::parity::decode_trace(bytes);
  check(trace.has_value() && trace->pads.size() == frames,
        "trace decodes with one entry per frame");
  check(trace.has_value() && trace->content_sha_hex == sha_a &&
            trace->config_sha_hex == sha_b,
        "content and config hashes are carried");
  check(!ayther::parity::decode_trace(
             std::vector<std::uint8_t>(bytes.begin(), bytes.end() - 3))
             .has_value(),
        "a truncated trace is rejected");

  // 4. replay the trace: same frame hashes as the take
  if (trace && trace->pads.size() == frames) {
    auto s = open_session(rom);
    check(s != nullptr, "replay session opens");
    if (s) {
      std::uint64_t h = 0;
      bool same = true;
      for (int f = 0; f < frames; ++f) {
        s->set_input(0, trace->pads[std::size_t(f)]);
        h = hash_frame(s->step(), h);
        same = same && h == take_hashes[std::size_t(f)];
      }
      check(same, "replayed trace gives the take's frame hashes");
    }
  }

  // 5. a missing take fails with a distinct code
  check(ayther::parity::trace_from_arp_command((dir / "missing.arp").string(),
                                               trace_path.string(), sha_a,
                                               sha_b) != 0,
        "a missing take is an error");
  return failures == 0 ? 0 : 1;
}
