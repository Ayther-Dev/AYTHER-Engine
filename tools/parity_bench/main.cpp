// parity_bench (spec 008, TP-005.13) — plane 3 of the Tessera parity bench.
//
//   parity_bench dump          --provider libretro|tessera_native ...
//   parity_bench compare       --provider libretro|tessera_native ...
//   parity_bench trace-from-arp ...
//   parity_bench check --exceptions FILE --catalog FILE --dumps DIR
//
// Skeleton: the subcommands validate their arguments and report that they are
// not implemented yet. dump arrives in TP-005.15 and compare in TP-005.16;
// trace-from-arp (TP-005.14) is implemented in trace_arp.cpp.
#include "exception_accounting.h"
#include "frame_compare.h"
#include "frame_dump.h"
#include "trace_arp.h"

#include <ayther/ayther_recording.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitNotImplemented = 3;

void print_help(std::FILE *out) {
  std::fputs(
      "parity_bench - plane 3 of the Tessera parity bench (spec 008)\n"
      "\n"
      "usage: parity_bench <subcommand> [options]\n"
      "\n"
      "subcommands:\n"
      "  dump             write the FrameView dump of a session\n"
      "  compare          compare two dumps, first divergence per subplane\n"
      "  trace-from-arp   convert the inputs of a take (.arp) to a TSTRACE1 "
      "trace\n"
      "  check            strict exception accounting over a catalog of dumps\n"
      "\n"
      "options:\n"
      "  --golden FILE --candidate FILE [--except SUBPLANE]...   (compare)\n"
      "  [--core DLL] --rom FILE (--arp FILE | --frames N) --out FILE\n"
      "      (dump: inputs from the take or N frames of neutral pad; --core\n"
      "       only with libretro, tessera_native reads the ROM itself)\n"
      "  --arp FILE --out FILE --content-sha HEX --config-sha HEX  "
      "(trace-from-arp)\n"
      "  --exceptions FILE --catalog FILE --dumps DIR [--registry FILE]\n"
      "      (check: DIR holds <rom>.libretro.jsonl and\n"
      "       <rom>.tessera_native.jsonl for every catalogued ROM; fails on a\n"
      "       divergence no tuple declares and on a declared tuple that no\n"
      "       longer diverges; --registry = docs/parity-exceptions.md)\n"
      "  --provider libretro|tessera_native   emulation provider of the "
      "session\n"
      "  --help                               show this help\n",
      out);
}

std::optional<std::string> read_file(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return std::nullopt;
  return std::string{std::istreambuf_iterator<char>(f),
                     std::istreambuf_iterator<char>()};
}

// parity_bench check: exit 0 when every divergence is declared and every
// declared tuple is produced, 5 when the accounting fails, 1 on bad input.
int check_command(const std::string &exceptions_path,
                  const std::string &catalog_path, const std::string &dumps,
                  const std::string &registry_path) {
  namespace ap = ayther::parity;
  const auto exceptions = read_file(exceptions_path);
  const auto catalog = read_file(catalog_path);
  if (!exceptions || !catalog) {
    std::fprintf(stderr, "check: cannot read %s\n",
                 !exceptions ? exceptions_path.c_str() : catalog_path.c_str());
    return 1;
  }
  std::set<std::string> known;
  if (!registry_path.empty()) {
    const auto registry = read_file(registry_path);
    if (!registry) {
      std::fprintf(stderr, "check: cannot read %s\n", registry_path.c_str());
      return 1;
    }
    known = ap::registry_ids(*registry);
    if (known.empty()) {
      std::fprintf(stderr, "check: no EXC rows in %s\n", registry_path.c_str());
      return 1;
    }
  }
  const auto file = ap::parse_exception_file(*exceptions);
  if (!file.ok) {
    std::fprintf(stderr, "check: %s: %s\n", exceptions_path.c_str(),
                 file.error.c_str());
    return 1;
  }
  std::vector<ap::RomDiff> roms;
  std::istringstream lines(*catalog);
  std::string rom;
  const std::filesystem::path dir(dumps);
  while (std::getline(lines, rom)) {
    if (const auto hash = rom.find('#'); hash != std::string::npos)
      rom.erase(hash);
    while (!rom.empty() && std::isspace(static_cast<unsigned char>(rom.back())))
      rom.pop_back();
    if (rom.empty())
      continue;
    const auto golden = read_file((dir / (rom + ".libretro.jsonl")).string());
    const auto candidate =
        read_file((dir / (rom + ".tessera_native.jsonl")).string());
    if (!golden || !candidate) {
      std::fprintf(stderr, "check: missing dump of %s in %s\n", rom.c_str(),
                   dumps.c_str());
      return 1;
    }
    const auto diff = ap::diff_dumps(*golden, *candidate);
    if (!diff.ok) {
      std::fprintf(stderr, "check: %s: %s\n", rom.c_str(), diff.error.c_str());
      return 1;
    }
    roms.push_back({rom, diff.frames, diff.divergences});
  }
  if (roms.empty()) {
    std::fprintf(stderr, "check: the catalog is empty\n");
    return 1;
  }
  const auto result = ap::account(file, roms, known);
  if (!result.ok) {
    std::fprintf(stderr, "check: %s: %s\n", exceptions_path.c_str(),
                 result.error.c_str());
    return 1;
  }
  for (const auto &t : result.undeclared)
    std::printf("undeclared %s %u %s\n", t.rom.c_str(), t.frame,
                t.subplane.c_str());
  for (const auto &t : result.unproduced)
    std::printf("unproduced %s %u %s %s\n", t.rom.c_str(), t.frame,
                t.subplane.c_str(), t.id.c_str());
  std::size_t declared = 0;
  for (const auto &[id, count] : result.declared) {
    std::printf("declared %s %zu\n", id.c_str(), count);
    declared += count;
  }
  std::printf("check: roms=%zu uniform=%zu declared=%zu undeclared=%zu "
              "unproduced=%zu\n",
              roms.size(), result.uniform_roms, declared,
              result.undeclared.size(), result.unproduced.size());
  return result.clean() ? kExitOk : 5;
}

bool valid_provider(std::string_view name) {
  return name == "libretro" || name == "tessera_native";
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    print_help(stderr);
    return kExitUsage;
  }
  const std::string_view subcommand = argv[1];
  if (subcommand == "--help" || subcommand == "-h" || subcommand == "help") {
    print_help(stdout);
    return kExitOk;
  }
  if (subcommand != "dump" && subcommand != "compare" &&
      subcommand != "trace-from-arp" && subcommand != "check") {
    std::fprintf(stderr, "unknown subcommand: %s\n", argv[1]);
    print_help(stderr);
    return kExitUsage;
  }

  std::string provider = "libretro";
  std::string arp, out, content_sha, config_sha, core, rom, golden, candidate,
      frames, exceptions, catalog, dumps, registry;
  std::vector<std::string> except;
  for (int i = 2; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_help(stdout);
      return kExitOk;
    }
    if (arg == "--provider") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "--provider needs a value\n");
        return kExitUsage;
      }
      provider = argv[++i];
      if (!valid_provider(provider)) {
        std::fprintf(stderr,
                     "invalid --provider: %s (libretro or tessera_native)\n",
                     provider.c_str());
        return kExitUsage;
      }
      continue;
    }
    if (arg == "--except" && i + 1 < argc) {
      except.emplace_back(argv[++i]);
      continue;
    }
    std::string *target = arg == "--arp"           ? &arp
                          : arg == "--out"         ? &out
                          : arg == "--content-sha" ? &content_sha
                          : arg == "--config-sha"  ? &config_sha
                          : arg == "--core"        ? &core
                          : arg == "--rom"         ? &rom
                          : arg == "--golden"      ? &golden
                          : arg == "--candidate"   ? &candidate
                          : arg == "--frames"      ? &frames
                          : arg == "--exceptions"  ? &exceptions
                          : arg == "--catalog"     ? &catalog
                          : arg == "--dumps"       ? &dumps
                          : arg == "--registry"    ? &registry
                                                   : nullptr;
    if (target != nullptr) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", argv[i]);
        return kExitUsage;
      }
      *target = argv[++i];
      continue;
    }
    std::fprintf(stderr, "unknown option: %s\n", argv[i]);
    return kExitUsage;
  }

  if (subcommand == "check") {
    if (exceptions.empty() || catalog.empty() || dumps.empty()) {
      std::fprintf(stderr, "check needs --exceptions, --catalog, --dumps\n");
      return kExitUsage;
    }
    return check_command(exceptions, catalog, dumps, registry);
  }

  if (subcommand == "trace-from-arp") {
    if (arp.empty() || out.empty() || content_sha.empty() ||
        config_sha.empty()) {
      std::fprintf(
          stderr,
          "trace-from-arp needs --arp, --out, --content-sha, --config-sha\n");
      return kExitUsage;
    }
    const int rc = ayther::parity::trace_from_arp_command(arp, out, content_sha,
                                                          config_sha);
    if (rc == 1)
      std::fprintf(stderr, "trace-from-arp: cannot read take: %s\n",
                   arp.c_str());
    if (rc == 2)
      std::fprintf(stderr, "trace-from-arp: hashes must be 64 hex digits\n");
    if (rc == 4)
      std::fprintf(stderr, "trace-from-arp: cannot write: %s\n", out.c_str());
    return rc;
  }

  if (subcommand == "dump") {
    const bool native = provider == "tessera_native";
    if ((!native && core.empty()) || rom.empty() ||
        (arp.empty() == frames.empty()) || out.empty()) {
      std::fprintf(stderr, native
                               ? "dump needs --rom, --arp or --frames, --out\n"
                               : "dump needs --core, --rom, --arp or "
                                 "--frames, --out\n");
      return kExitUsage;
    }
    std::vector<std::uint16_t> pads;
    if (!arp.empty()) {
      const auto take = ayther::AytherRecording::load(arp);
      if (!take || take->inputs.empty()) {
        std::fprintf(stderr, "dump: cannot read take: %s\n", arp.c_str());
        return 1;
      }
      pads = take->inputs;
    } else {
      char *end = nullptr;
      const unsigned long count = std::strtoul(frames.c_str(), &end, 10);
      if (end == frames.c_str() || *end != '\0' || count == 0 ||
          count > 1000000) {
        std::fprintf(stderr,
                     "dump: --frames needs a count from 1 to 1000000\n");
        return kExitUsage;
      }
      pads.assign(count, 0); // neutral pad on every frame
    }
    std::optional<std::string> text;
    if (native) {
      std::ifstream in(rom, std::ios::binary);
      if (!in) {
        std::fprintf(stderr, "dump: cannot read rom: %s\n", rom.c_str());
        return 1;
      }
      const std::vector<std::uint8_t> image{std::istreambuf_iterator<char>(in),
                                            std::istreambuf_iterator<char>()};
      text = ayther::parity::dump_native_session(image, pads);
    } else {
      text = ayther::parity::dump_session(core, rom, pads);
    }
    if (!text) {
      std::fprintf(stderr, "dump: the session could not be created\n");
      return 1;
    }
    std::ofstream file(out, std::ios::binary | std::ios::trunc);
    file.write(text->data(), static_cast<std::streamsize>(text->size()));
    file.close();
    return file.good() ? kExitOk : 4;
  }

  if (subcommand == "compare") {
    if (golden.empty() || candidate.empty()) {
      std::fprintf(stderr, "compare needs --golden and --candidate\n");
      return kExitUsage;
    }
    const auto read = [](const std::string &path) {
      std::ifstream f(path, std::ios::binary);
      return std::string{std::istreambuf_iterator<char>(f),
                         std::istreambuf_iterator<char>()};
    };
    const auto result =
        ayther::parity::compare_dumps(read(golden), read(candidate), except);
    if (!result.ok) {
      std::fprintf(stderr, "compare: %s\n", result.error.c_str());
      return 1;
    }
    for (const auto &d : result.divergences)
      std::printf("divergence %s frame %u golden %s candidate %s\n",
                  d.subplane.c_str(), d.frame, d.a_hash.c_str(),
                  d.b_hash.c_str());
    std::printf("compare: %zu divergent subplane(s)\n",
                result.divergences.size());
    return result.divergences.empty() ? kExitOk : 5;
  }

  std::fprintf(stderr, "%s: not implemented yet (provider %s)\n",
               subcommand.data(), provider.c_str());
  return kExitNotImplemented;
}
