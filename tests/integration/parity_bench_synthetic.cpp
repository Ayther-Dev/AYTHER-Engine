// Spec 008 (TP-005.16, D-3): the plane-3 comparator finds the first divergence
// per subplane between two FrameView dumps, honouring an exception list.
// Synthetic: a golden dump from ayther_test_core and a seeded divergence.
#include "../../tools/common/synth_rom.h"
#include "../../tools/parity_bench/frame_dump.h"
#include "../../tools/parity_bench/frame_compare.h"

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
// Rewrites the hash of `subplane` on `frame` (1-based) to a different value.
std::string seed(std::string dump, int frame, const std::string &subplane) {
  const std::string head = "{\"frame\":" + std::to_string(frame) + ",";
  const auto line = dump.find(head);
  const auto key = dump.find("\"" + subplane + "\":{\"hash\":\"", line);
  const auto at = key + subplane.size() + 12;
  dump[at] = dump[at] == '0' ? '1' : '0';
  return dump;
}
} // namespace

int main() {
  using namespace ayther::parity;
  const std::vector<std::uint16_t> pads(20, 0);
  const auto golden = dump_session(AYTHER_TEST_CORE_PATH,
                                   ayther::synth::fx03_rom_path(), pads);
  check(golden.has_value(), "golden dump");
  if (!golden)
    return 1;

  auto same = compare_dumps(*golden, *golden, {});
  check(same.ok && same.divergences.empty(), "identical dumps match");

  const auto seeded = seed(*golden, 7, "sprites");
  auto r = compare_dumps(*golden, seeded, {});
  check(r.ok && r.divergences.size() == 1 && r.divergences[0].subplane == "sprites" &&
            r.divergences[0].frame == 7,
        "a seeded divergence is found at its subplane and frame");

  const auto two = seed(seed(*golden, 5, "palette"), 3, "palette");
  r = compare_dumps(*golden, two, {});
  check(r.divergences.size() == 1 && r.divergences[0].frame == 3,
        "only the FIRST divergence of a subplane is reported");

  const auto both = seed(seed(*golden, 9, "scene"), 4, "raster");
  r = compare_dumps(*golden, both, {});
  check(r.divergences.size() == 2, "each subplane reports its own divergence");

  r = compare_dumps(*golden, seeded, {"sprites"});
  check(r.ok && r.divergences.empty(), "an excepted subplane is not compared");

  r = compare_dumps(*golden, golden->substr(0, golden->size() / 2), {});
  check(!r.ok, "dumps of different length are an error, not a match");
  r = compare_dumps("not a dump\n", *golden, {});
  check(!r.ok, "a malformed dump is an error");
  return failures == 0 ? 0 : 1;
}
