// Spec 008 (TP-301.1, D-3): the FrameView a session builds through
// ObservationSource is the one it built before the source was extracted from
// RetroRunner. The goldens are FrameView dumps (a hash per subplane and frame,
// tools/parity_bench) recorded with the pre-refactor Engine on three scenes:
// the ABI test core on the canonical and FX-03 ROMs, and the stock test core
// (no ABI, legacy memory path) on the canonical ROM.
//
// `--write-golden <dir>` rewrites the goldens. Only do that when a FrameView
// change is intended and reviewed: this test exists to catch the unintended
// ones.
#include "../../tools/common/synth_rom.h"
#include "../../tools/parity_bench/frame_compare.h"
#include "../../tools/parity_bench/frame_dump.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string &message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message.c_str());
}

constexpr int kFrames = 120;

std::vector<std::uint16_t> pads() {
  std::vector<std::uint16_t> v;
  for (int f = 0; f < kFrames; ++f)
    v.push_back(static_cast<std::uint16_t>((f * 37 + 5) & 0x1FB));
  return v;
}

struct Scene {
  const char *name;
  const char *core;
  std::string (*rom)();
};

const Scene kScenes[] = {
    {"abi_canonical", AYTHER_TEST_CORE_PATH, ayther::synth::canonical_rom_path},
    {"abi_fx03", AYTHER_TEST_CORE_PATH, ayther::synth::fx03_rom_path},
    {"stock_canonical", AYTHER_TEST_CORE_STOCK_PATH,
     ayther::synth::canonical_rom_path},
};

std::optional<std::string> read_file(const std::filesystem::path &p) {
  std::ifstream in(p, std::ios::binary);
  if (!in)
    return std::nullopt;
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

bool write_file(const std::filesystem::path &p, const std::string &text) {
  std::ofstream out(p, std::ios::binary);
  out << text;
  return static_cast<bool>(out);
}

} // namespace

int main(int argc, char **argv) {
  std::filesystem::path golden_dir = AYTHER_OBSERVATION_SOURCE_GOLDEN_DIR;
  bool write = false;
  if (argc == 3 && std::string(argv[1]) == "--write-golden") {
    write = true;
    golden_dir = argv[2];
  }

  for (const Scene &scene : kScenes) {
    const auto dump =
        ayther::parity::dump_session(scene.core, scene.rom(), pads());
    check(dump.has_value(), std::string(scene.name) + ": session dumps");
    if (!dump)
      continue;
    const auto path = golden_dir / (std::string(scene.name) + ".jsonl");
    if (write) {
      check(write_file(path, *dump),
            std::string(scene.name) + ": golden written to " + path.string());
      continue;
    }
    const auto golden = read_file(path);
    check(golden.has_value(),
          std::string(scene.name) + ": golden present at " + path.string());
    if (!golden)
      continue;
    const auto r = ayther::parity::compare_dumps(*golden, *dump, {});
    if (!r.ok)
      std::printf("  %s\n", r.error.c_str());
    for (const auto &d : r.divergences)
      std::printf("  first divergence: %s at frame %u (golden %s, now %s)\n",
                  d.subplane.c_str(), d.frame, d.a_hash.c_str(),
                  d.b_hash.c_str());
    check(r.ok && r.divergences.empty(),
          std::string(scene.name) +
              ": FrameView identical to the pre-ObservationSource golden");
  }
  return failures == 0 ? 0 : 1;
}
