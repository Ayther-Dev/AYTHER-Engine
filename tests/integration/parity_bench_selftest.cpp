// Spec 008 (TP-005.17, RF-7.3): self-test of plane 3 in the native job.
// Records a take with ayther_test_core, then runs the parity_bench executable
// itself (dump twice, compare) on the FX-02-style and FX-03 synthetic scenes
// and expects zero divergences; a seeded divergence must be caught (exit 5).
// The comparison between providers (gpgx vs Tessera) is TP-209.
#include <ayther/ayther_recording.h>
#include <ayther/ayther_session.h>

#include "../../tools/common/synth_rom.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
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
  // POSIX system() returns the wait status, not the exit code.
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}
std::string q(const fs::path &p) { return "\"" + p.string() + "\""; }

bool record_take(const std::string &rom, const fs::path &out) {
  ayther::AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = rom;
  config.enable_audio = false;
  config.derive_core_pack = false;
  auto created = ayther::AytherSession::create(config);
  if (!created)
    return false;
  std::unique_ptr<ayther::AytherSession> s = std::move(*created);
  s->record_start();
  for (int f = 0; f < 30; ++f) {
    s->set_input(0, static_cast<std::uint16_t>((f * 37 + 5) & 0x1FB));
    (void)s->step();
  }
  s->record_stop();
  return s->take_recording().save(out.string());
}

void scenario(const char *name, const std::string &rom, const fs::path &dir) {
  const std::string tag = name;
  const fs::path take = dir / (tag + ".arp"), a = dir / (tag + ".a.jsonl"),
                 b = dir / (tag + ".b.jsonl"), bad = dir / (tag + ".bad.jsonl");
  check(!rom.empty() && record_take(rom, take),
        (tag + ": take recorded").c_str());
  const std::string common = "--core " + q(AYTHER_TEST_CORE_PATH) + " --rom " +
                             q(rom) + " --arp " + q(take) + " --out ";
  check(run("dump " + common + q(a)) == 0 && run("dump " + common + q(b)) == 0,
        (tag + ": two dumps written").c_str());
  check(run("compare --golden " + q(a) + " --candidate " + q(b)) == 0,
        (tag + ": the two dumps compare equal").c_str());
  std::string text;
  {
    std::ifstream in(a, std::ios::binary);
    text.assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
  }
  const auto at = text.find("\"palette\":{\"hash\":\"") + 19;
  text[at] = text[at] == '0' ? '1' : '0';
  { std::ofstream(bad, std::ios::binary) << text; }
  check(run("compare --golden " + q(a) + " --candidate " + q(bad)) == 5,
        (tag + ": a seeded divergence is reported (exit 5)").c_str());
}
} // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "ayther_parity_selftest";
  fs::create_directories(dir);
  scenario("fx02", ayther::synth::canonical_rom_path(), dir);
  scenario("fx03", ayther::synth::fx03_rom_path(), dir);
  return failures == 0 ? 0 : 1;
}
