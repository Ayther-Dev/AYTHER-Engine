// Spec 008 (TP-210.1, RF-10.3, plan §3.13): the cross-pack matrix on the
// FX-03 synthetic ROM. The parity_bench executable bakes one pack with gpgx
// and one with Tessera, plays each with both providers, and the two crossed
// plays must equal their same-provider reference frame by frame: the same
// multiset of substitutions and the same presented frame.
//
// gpgx is the fork core named by third_party/cores/core.lock, or the file in
// AYTHER_PARITY_GPGX_CORE. Without either the test reports 77 (skip); the
// Windows CI job downloads it, checks it against core.lock and sets the
// variable, so there it runs.
#include "../../tools/common/core_path.h"
#include "../../tools/common/synth_rom.h"
#include "../../tools/parity_bench/cross_pack.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace {
int failures = 0;
void check(bool condition, const std::string &message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message.c_str());
}
namespace fs = std::filesystem;

int run(const std::string &args) {
#ifdef _WIN32
  const std::string command = "\"\"" PARITY_BENCH_PATH "\" " + args + "\"";
#else
  const std::string command = "\"" PARITY_BENCH_PATH "\" " + args;
#endif
  // The bench is a separate executable by design (it is what the job runs).
  // NOLINTNEXTLINE(bugprone-command-processor,concurrency-mt-unsafe)
  const int status = std::system(command.c_str());
#ifdef _WIN32
  return status;
#else
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}
std::string q(const fs::path &p) { return "\"" + p.string() + "\""; }
std::string read(const fs::path &p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
} // namespace

int main() {
  // AYTHER_PARITY_GPGX_CORE (set by the CI job to the verified download) wins
  // over core.lock and must name a file; without it, core.lock decides.
  const char *override_path = ayther::env_get("AYTHER_PARITY_GPGX_CORE");
  const auto core = ayther::test::resolve_core_path(
      AYTHER_SOURCE_DIR, override_path != nullptr && *override_path != '\0'
                             ? std::optional<std::string_view>(override_path)
                             : std::nullopt);
  if (!core.available()) {
    std::printf("[%s] %s\n", core.failure_exit_code == 77 ? "SKIP" : "FAIL",
                core.diagnostic.c_str());
    return core.failure_exit_code;
  }
  const std::string rom = ayther::synth::fx03_rom_path();
  check(!rom.empty(), "FX-03 ROM written");
  if (rom.empty())
    return 1;
  const fs::path dir = fs::temp_directory_path() / "ayther_parity_cross_pack";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  constexpr int kFrames = 60;
  const struct {
    const char *key, *args;
  } providers[] = {{"gpgx", nullptr}, {"tessera", "--provider tessera_native"}};
  const std::string libretro =
      "--provider libretro --core " + q(core.path.string());
  const auto provider_args = [&](int k) {
    return std::string(k == 0 ? libretro : providers[1].args) + " --rom " +
           q(rom) + " --frames " + std::to_string(kFrames);
  };

  for (int k = 0; k < 2; ++k) {
    const std::string key = providers[k].key;
    check(run("cross-pack bake " + provider_args(k) + " --pack " +
              q(dir / (key + ".ay")) + " --registry " +
              q(dir / (key + ".toml"))) == 0,
          "bake with " + key);
  }
  for (int pack = 0; pack < 2; ++pack)
    for (int k = 0; k < 2; ++k) {
      const std::string baked = providers[pack].key;
      const std::string played = providers[k].key;
      check(run("cross-pack play " + provider_args(k) + " --pack " +
                q(dir / (baked + ".ay")) + " --registry " +
                q(dir / (baked + ".toml")) + " --out " +
                q(dir / (baked + "_on_" + played + ".jsonl"))) == 0,
            "play the " + baked + " pack with " + played);
    }

  for (int pack = 0; pack < 2; ++pack) {
    const std::string baked = providers[pack].key;
    const std::string reference = baked + "_on_" + baked + ".jsonl";
    const std::string crossed =
        baked + "_on_" + providers[1 - pack].key + ".jsonl";
    check(run("cross-pack compare --golden " + q(dir / reference) +
              " --candidate " + q(dir / crossed)) == 0,
          crossed + " equals its reference " + reference);
    const auto r = ayther::parity::compare_plays(read(dir / reference),
                                                 read(dir / crossed));
    check(r.equal() && r.frames == kFrames,
          "the comparison covers every frame of " + crossed);
    // Equal and empty would prove nothing: the reference must substitute.
    check(r.substituted_frames == kFrames,
          "every frame of " + reference + " has substitutions");
  }

  // The comparison must also catch a difference: the gpgx reference against
  // a copy whose presented hash of frame 30 has one digit changed.
  const std::string golden = read(dir / "gpgx_on_gpgx.jsonl");
  std::string seeded = golden;
  const auto line = seeded.find("{\"frame\":30,");
  const auto at = seeded.find("\"presented\":{\"hash\":\"", line);
  if (line != std::string::npos && at != std::string::npos) {
    char &digit = seeded[at + 21];
    digit = digit == '0' ? '1' : '0';
  }
  const auto r = ayther::parity::compare_plays(golden, seeded);
  check(r.ok && r.divergences.size() == 1 &&
            r.divergences[0].field == "presented" &&
            r.divergences[0].frame == 30 && r.divergences[0].frames == 1,
        "a seeded presented difference is found at its frame");
  check(run("cross-pack compare --golden " + q(dir / "gpgx_on_gpgx.jsonl") +
            " --candidate " + q(dir / "missing.jsonl")) == 1,
        "compare of a missing file exits 1");
  return failures == 0 ? 0 : 1;
}
