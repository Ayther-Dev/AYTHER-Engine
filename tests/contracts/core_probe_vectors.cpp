// The in-process vectors of the core probe contract (TP-013.4,
// docs/CORE_PROBE_CONTRACT.md). The contract has eight vectors; this test runs
// the six that do not need a binary outside the repository or a supervising
// parent:
//
//   - gpgx r4 x64 is an external binary, checked by the products that have it;
//   - ayther_test_core_hang never returns, so `timeout` belongs to the parent
//     process (TP-013.5/.7) and the fixture is only checked to exist here.
//
// A seventh vector, a loadable library without the Libretro entry points, is
// checked as well because it is the other way to reach `not_a_core`.
#include <ayther/engine/core_probe.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

using ayther::engine::ProbeReport;
using ayther::engine::ProbeVerdict;

int failures = 0;

void check(bool condition, const std::string &message) {
  std::printf("  [%s] %s\n", condition ? " OK " : "FAIL", message.c_str());
  if (!condition) {
    ++failures;
  }
}

bool contains(const std::string &text, std::string_view part) {
  return text.find(part) != std::string::npos;
}

struct Vector {
  const char *name;
  ProbeVerdict verdict;
  std::string_view reason;
  int exit_code;
};

void expect(const Vector &vector, const ProbeReport &report) {
  const std::string name{vector.name};
  check(report.verdict == vector.verdict,
        name + ": verdict is " +
            std::string{ayther::engine::verdict_name(vector.verdict)} +
            " (got " +
            std::string{ayther::engine::verdict_name(report.verdict)} + "; " +
            report.message + ")");
  check(ayther::engine::verdict_reason(report.verdict) == vector.reason,
        name + ": reason is '" + std::string{vector.reason} + "'");
  check(ayther::engine::verdict_exit_code(report.verdict) == vector.exit_code,
        name + ": exit code is " + std::to_string(vector.exit_code));

  const std::string json = report.serialize();
  check(contains(json, "\"protocol_version\":2") &&
            contains(json, "\"event\":\"probe\""),
        name + ": the JSON is protocol version 2");
  check(contains(json,
                 std::string{"\"verdict\":\""} +
                     std::string{ayther::engine::verdict_name(vector.verdict)} +
                     "\""),
        name + ": the JSON names the verdict");
  check(vector.reason.empty()
            ? !contains(json, "\"reason\"")
            : contains(json, std::string{"\"reason\":\""} +
                                 std::string{vector.reason} + "\""),
        name + ": the reason appears only when the probe failed");
  check(report.status_line().rfind("AYTHER_STATUS {", 0) == 0,
        name + ": the status line carries the AYTHER_STATUS prefix");
}

/// A 70-byte file that is a valid PE header for the given machine, enough for
/// the architecture check, which reads headers only.
std::filesystem::path write_pe_stub(const std::string &file_name,
                                    std::uint16_t machine) {
  std::string bytes(0x46, '\0');
  bytes[0] = 'M';
  bytes[1] = 'Z';
  bytes[0x3C] = 0x40;
  bytes[0x40] = 'P';
  bytes[0x41] = 'E';
  bytes[0x44] = static_cast<char>(machine & 0xFFU);
  bytes[0x45] = static_cast<char>(machine >> 8U);
  std::ofstream file(file_name, std::ios::binary);
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return file_name;
}

} // namespace

int main() {
  namespace engine = ayther::engine;
  // Unbuffered, so a timeout under CTest still shows the last vector reached.
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  // Verdict table: names, reasons and exit codes of the contract.
  check(engine::verdict_exit_code(ProbeVerdict::timeout) == 5 &&
            engine::verdict_reason(ProbeVerdict::timeout) ==
                "core.probe_timeout",
        "timeout is exit 5 with reason core.probe_timeout");
  check(engine::verdict_exit_code(ProbeVerdict::crashed) == 6 &&
            engine::verdict_reason(ProbeVerdict::crashed) ==
                "core.probe_crashed",
        "crashed is exit 6 with reason core.probe_crashed");
  check(engine::kRequiredCapabilities == 0xEFFFFULL,
        "the required capability mask is 0xEFFFF");

  // Vector: ayther_test_core -> ok.
  {
    const auto report = engine::probe_core_v2(AYTHER_TEST_CORE_PATH);
    expect({"ayther_test_core", ProbeVerdict::ok, "", 0}, report);
    check(report.ayther_abi == "1.10", "ayther_test_core: ABI is 1.10");
    check(report.capabilities == 0xEFFFFULL,
          "ayther_test_core: announces exactly the required mask");
    check(report.missing_capabilities.empty(),
          "ayther_test_core: nothing is missing");
    check(report.library_name == "AYTHER Test Core" && report.loaded,
          "ayther_test_core: the libretro information is reported");
    check(!report.ayther_build_id.empty(),
          "ayther_test_core: the build id is reported");
    const std::string json = report.serialize();
    check(contains(json, "\"capabilities\":\"0xEFFFF\"") &&
              contains(json, "\"required_capabilities\":\"0xEFFFF\"") &&
              contains(json, "\"missing_capabilities\":[]") &&
              contains(json, "\"ayther_abi\":\"1.10\""),
          "ayther_test_core: the JSON carries the capability fields");
  }

  // Vector: non-existent path -> missing_file.
  {
    const auto report = engine::probe_core_v2(AYTHER_MISSING_CORE_PATH);
    expect({"missing path", ProbeVerdict::missing_file, "core.missing", 2},
           report);
    check(!report.loaded, "missing path: nothing was loaded");
  }

  // Vector: an x86 library in an x64 process -> wrong_architecture. The
  // check reads the PE header, so a stub is enough and nothing is loaded.
  {
    const auto stub = write_pe_stub("core_probe_x86_stub.dll", 0x014C);
    const auto report = engine::probe_core_v2(stub);
    expect({"x86 library", ProbeVerdict::wrong_architecture,
            "core.wrong_architecture", 2},
           report);
    check(report.architecture == "x86" && !report.loaded,
          "x86 library: the architecture is read without loading the file");
  }

  // Vector: a text file -> not_a_core.
  {
    const std::filesystem::path text{"core_probe_text.txt"};
    std::ofstream{text} << "this is not a library\n";
    const auto report = engine::probe_core_v2(text);
    expect({"text file", ProbeVerdict::not_a_core, "core.invalid", 3}, report);
  }

  // A loadable library without the Libretro entry points -> not_a_core.
  {
    const auto report = engine::probe_core_v2(AYTHER_NOT_A_CORE_PATH);
    expect({"library without libretro symbols", ProbeVerdict::not_a_core,
            "core.invalid", 3},
           report);
    check(contains(report.message, "retro_api_version"),
          "library without libretro symbols: the missing symbol is named");
  }

  // Vector: ayther_test_core_stock -> abi_missing.
  {
    const auto report = engine::probe_core_v2(AYTHER_TEST_CORE_STOCK_PATH);
    expect({"ayther_test_core_stock", ProbeVerdict::abi_missing,
            "core.abi_missing", 4},
           report);
    check(report.loaded && report.ayther_abi.empty(),
          "ayther_test_core_stock: a libretro core without the AYTHER ABI");
    check(!contains(report.serialize(), "\"ayther_abi\""),
          "ayther_test_core_stock: the JSON has no ABI fields");
  }

  // Vector: ayther_test_core_partial -> capabilities_incomplete.
  {
    const auto report = engine::probe_core_v2(AYTHER_TEST_CORE_PARTIAL_PATH);
    expect({"ayther_test_core_partial", ProbeVerdict::capabilities_incomplete,
            "core.capabilities_incomplete", 4},
           report);
    check(report.ayther_abi == "1.10" && report.capabilities == 0x6FFFFULL,
          "ayther_test_core_partial: ABI 1.10 with the mask 0x6FFFF");
    check(report.missing_capabilities.size() == 1 &&
              report.missing_capabilities.front() == "SPRITE_OUTCOME_V1",
          "ayther_test_core_partial: SPRITE_OUTCOME_V1 is the missing bit");
    check(contains(report.serialize(),
                   "\"missing_capabilities\":[\"SPRITE_OUTCOME_V1\"]"),
          "ayther_test_core_partial: the JSON names the missing bit");
  }

  // Vector: ayther_test_core_hang -> timeout, set by the parent. It is not
  // loaded here: retro_init never returns.
  check(std::filesystem::is_regular_file(AYTHER_TEST_CORE_HANG_PATH),
        "ayther_test_core_hang exists (the parent supervises it)");

  std::printf("%s\n", failures == 0 ? "=== PASS ===" : "=== FAIL ===");
  return failures == 0 ? 0 : 1;
}
