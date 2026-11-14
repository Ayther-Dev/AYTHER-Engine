// ayther_core_probe — the installed core probe command (TP-013.5).
//
//   ayther_core_probe <core-path>
//
// Prints one `AYTHER_STATUS {...}` line (protocol version 2) on standard output
// and exits 0 (ok), 2 (missing file or wrong architecture), 3 (not a core) or
// 4 (AYTHER ABI missing or capabilities incomplete). Exit code 1 is a usage
// error. The verdicts `timeout` (5) and `crashed` (6) belong to the parent
// process that supervises this command; see docs/CORE_PROBE_CONTRACT.md.
//
// The command negotiates with the core in its own process on purpose: a core
// that hangs or crashes at load time takes this process down and nobody else.
#include <ayther/engine/core_probe.hpp>

#include <cstdio>
#include <filesystem>
#include <string_view>

namespace {

constexpr int kUsageError = 1;

int usage() {
  std::fputs("usage: ayther_core_probe <core-path>\n", stderr);
  return kUsageError;
}

int probe(const std::filesystem::path &core_path) {
  const ayther::engine::ProbeReport report =
      ayther::engine::probe_core_v2(core_path);
  std::printf("%s\n", report.status_line().c_str());
  std::fflush(stdout);
  return ayther::engine::verdict_exit_code(report.verdict);
}

} // namespace

#if defined(_WIN32)
// The wide entry point keeps non-ASCII core paths intact.
int wmain(int argc, wchar_t **argv) {
  if (argc != 2) {
    return usage();
  }
  const std::wstring_view argument{argv[1]};
  if (argument == L"--help" || argument == L"-h") {
    return usage();
  }
  return probe(std::filesystem::path{argv[1]});
}
#else
int main(int argc, char **argv) {
  if (argc != 2) {
    return usage();
  }
  const std::string_view argument{argv[1]};
  if (argument == "--help" || argument == "-h") {
    return usage();
  }
  return probe(std::filesystem::path{argv[1]});
}
#endif
