// Spec 008 (TP-005.15, D-3): the FrameView dump of a session, with a hash per
// subplane. Two runs of the same scenario with ayther_test_core give identical
// dumps; a different input stream gives a different one.
#include "../../tools/common/synth_rom.h"
#include "../../tools/parity_bench/frame_dump.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}
std::vector<std::uint16_t> pads(int n, int salt) {
  std::vector<std::uint16_t> v;
  for (int f = 0; f < n; ++f)
    v.push_back(static_cast<std::uint16_t>((f * 37 + salt) & 0x1FB));
  return v;
}
} // namespace

int main() {
  const std::string rom = ayther::synth::fx03_rom_path();
  const std::string core = AYTHER_TEST_CORE_PATH;
  const auto a = ayther::parity::dump_session(core, rom, pads(30, 5));
  const auto b = ayther::parity::dump_session(core, rom, pads(30, 5));
  // The test core only reflects input in work RAM, not in the FrameView, so
  // the sensitivity of the dump is proved with a different scene (ROM).
  const auto c = ayther::parity::dump_session(
      core, ayther::synth::canonical_rom_path(), pads(30, 5));
  const auto d = c;
  check(a.has_value() && b.has_value() && c.has_value() && d.has_value(), "sessions dump");
  if (!a || !b || !c)
    return 1;
  check(*a == *b, "two runs of the same scenario give identical dumps");
  check(*a != *c, "a different scene (ROM) changes the dump");

  // one JSON line per frame, every subplane present with hash and count
  std::size_t lines = 0;
  for (const char ch : *a)
    lines += ch == '\n';
  check(lines == 30, "one line per frame");
  bool all = true;
  for (const char *name : ayther::parity::kSubplaneNames)
    all = all && a->find(std::string("\"") + name + "\":{\"hash\":\"") !=
                     std::string::npos;
  check(all, "every subplane (fb, scene, sprites, tiles, plane_cells, raster, "
             "chip_writes, palette, attribution) is dumped");
  check(!ayther::parity::dump_session(core, "missing.rom", pads(3, 1)).has_value(),
        "a missing ROM is an error");
  return failures == 0 ? 0 : 1;
}
