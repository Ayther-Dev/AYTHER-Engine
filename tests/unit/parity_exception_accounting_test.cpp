// Spec 008 (TP-209.1, D-3, RF-10.5): strict exception accounting of
// parity_bench check. A divergence no tuple declares fails, and so does a
// declared tuple that no longer diverges (no zombie exceptions).
#include "../../tools/parity_bench/exception_accounting.h"
#include "../../tools/parity_bench/frame_dump.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

using namespace ayther::parity;

// One dump line in the frame_dump shape; `odd` subplanes get another hash.
std::string line(int frame, const std::set<std::string> &odd) {
  std::string s = "{\"frame\":" + std::to_string(frame);
  for (const char *name : kSubplaneNames)
    s += std::string(",\"") + name + "\":{\"hash\":\"" +
         (odd.count(name) ? "1111" : "0000") + "\",\"count\":1}";
  return s + "}\n";
}

RomDiff rom(const std::string &name,
            const std::vector<std::pair<std::uint32_t, std::string>> &d,
            std::uint32_t frames = 3) {
  RomDiff r{name, {}, {}};
  for (std::uint32_t f = 1; f <= frames; ++f)
    r.frames.push_back(f);
  for (const auto &[f, s] : d)
    r.divergences.push_back({s, f, "a", "b"});
  return r;
}

AccountResult run(const std::string &text, const std::vector<RomDiff> &roms,
                  const std::set<std::string> &ids = {}) {
  return account(parse_exception_file(text), roms, ids);
}
} // namespace

int main() {
  // diff_dumps lists every (frame, subplane), not only the first per subplane.
  const auto a = line(1, {}) + line(2, {}) + line(3, {});
  const auto b = line(1, {"pcm"}) + line(2, {"fb", "pcm"}) + line(3, {"pcm"});
  const auto d = diff_dumps(a, b);
  check(d.ok && d.divergences.size() == 4 && d.frames.size() == 3,
        "diff_dumps reports all four divergent tuples");
  check(!diff_dumps(a, line(1, {})).ok, "diff_dumps rejects unequal lengths");
  const auto renumbered = line(1, {}) + line(2, {}) + line(4, {});
  check(!diff_dumps(a, renumbered).ok,
        "diff_dumps rejects other frame numbering");
  check(!compare_dumps(a, renumbered, {}).ok,
        "compare_dumps rejects other frame numbering");
  check(!diff_dumps(a, "not a dump\n").ok, "diff_dumps rejects a bad dump");
  check(!compare_dumps(a, "{\"frame\":1}\n", {}).ok,
        "compare_dumps rejects a line without subplanes");

  const std::vector<RomDiff> roms = {
      rom("alpha", {{1, "pcm"}, {2, "pcm"}, {3, "pcm"}, {2, "fb"}}),
      rom("beta", {{1, "pcm"}, {2, "pcm"}, {3, "pcm"}})};
  const std::string base = "# comment\n* * pcm EXC-008\n\n"
                           "alpha 2 fb EXC-007   # trailing comment\n";
  auto r = run(base, roms);
  check(r.clean() && r.declared["EXC-008"] == 6 && r.declared["EXC-007"] == 1,
        "every divergence declared, every tuple produced");
  check(r.uniform_roms == 1, "beta is covered by wildcard entries only");

  r = run("* * pcm EXC-008\n", roms);
  check(r.ok && !r.clean() && r.undeclared.size() == 1 &&
            r.undeclared[0].rom == "alpha" && r.undeclared[0].frame == 2 &&
            r.undeclared[0].subplane == "fb",
        "an undeclared divergence fails");

  r = run(base + "beta 1-2 fb EXC-007\n", roms);
  check(r.ok && !r.clean() && r.unproduced.size() == 2 &&
            r.unproduced[0].id == "EXC-007",
        "a declared tuple that no longer diverges fails (zombie)");

  r = run(base + "beta 4 tiles EXC-007\n", roms);
  check(r.ok && r.unproduced.size() == 1,
        "a declared frame beyond the dump is unproduced");

  auto quiet = roms;
  quiet[1].divergences.pop_back(); // beta frame 3 pcm now matches
  r = run(base, quiet);
  check(r.ok && r.unproduced.size() == 1 && r.unproduced[0].rom == "beta" &&
            r.unproduced[0].frame == 3,
        "a wildcard entry must diverge on every frame it expands to");

  r = run(base + "alpha 1-3 pcm EXC-001\n", roms);
  check(!r.ok, "a tuple declared twice is an error");
  r = run(base + "gamma 1 fb EXC-007\n", roms);
  check(!r.ok, "a ROM outside the catalog is an error");
  r = run(base, roms, {"EXC-007"});
  check(!r.ok, "an id missing from the registry is an error");
  r = run(base, roms, registry_ids("| EXC-007 | x |\n| EXC-008 | y |\ntext\n"));
  check(r.clean(), "ids read from the registry rows");

  check(!parse_exception_file("alpha 1 fb\n").ok, "three fields rejected");
  check(!parse_exception_file("alpha 0 fb EXC-001\n").ok, "frame 0 rejected");
  check(!parse_exception_file("alpha 5-3 fb EXC-001\n").ok,
        "reversed range rejected");
  check(!parse_exception_file("alpha 1 frame_hash EXC-001\n").ok,
        "unknown subplane rejected");
  check(!parse_exception_file("alpha 1 fb EXC-1\n").ok, "bad id rejected");
  check(!parse_exception_file("alpha 1-2000000 fb EXC-001\n").ok,
        "range beyond the dump limit rejected");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
