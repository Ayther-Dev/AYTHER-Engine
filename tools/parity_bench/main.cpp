// parity_bench (spec 008, TP-005.13) — plane 3 of the Tessera parity bench.
//
//   parity_bench dump          --provider libretro|tessera_native ...
//   parity_bench compare       --provider libretro|tessera_native ...
//   parity_bench trace-from-arp ...
//   parity_bench check --exceptions FILE --catalog FILE --dumps DIR
//   parity_bench compare-identities --golden FILE --candidate FILE
//   parity_bench cross-pack bake|play|compare ...   (TP-210.1)
//
// Skeleton: the subcommands validate their arguments and report that they are
// not implemented yet. dump arrives in TP-005.15 and compare in TP-005.16;
// trace-from-arp (TP-005.14) is implemented in trace_arp.cpp.
#include "cross_pack.h"
#include "exception_accounting.h"
#include "frame_compare.h"
#include "frame_dump.h"
#include "identities.h"
#include "trace_arp.h"

#include <ayther/ayther_recording.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
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
      "  compare-identities  compare two identity files (dump --identities)\n"
      "  cross-pack bake|play|compare  packs baked with one provider and\n"
      "                   played with either (RF-10.3, plan 3.13)\n"
      "\n"
      "options:\n"
      "  --golden FILE --candidate FILE [--except SUBPLANE]...   (compare)\n"
      "  [--core DLL] --rom FILE (--arp FILE | --frames N) --out FILE\n"
      "      (dump: inputs from the take or N frames of neutral pad; --core\n"
      "       only with libretro, tessera_native reads the ROM itself)\n"
      "  [--identities FILE --segments A-B[,A-B]...]\n"
      "      (dump: also write the canonical identities per segment and\n"
      "       class of the identity contract, RF-10.1; frames from 1)\n"
      "  --golden FILE --candidate FILE [--out FILE]\n"
      "      (compare-identities: exit 0 equal sets, 5 different; --out\n"
      "       writes the comparison as JSON)\n"
      "  [--core DLL] --rom FILE (--arp FILE | --frames N) --pack FILE\n"
      "      --registry FILE   (cross-pack bake: one rule and one asset per\n"
      "       tile, sprite and plane-tile identity the provider observes,\n"
      "       signed, and the trust registry that vouches for it;\n"
      "       --shard K/N bakes only the identities whose hash mod N is K-1,\n"
      "       for games with more than a pack's 8192 entries)\n"
      "  ... --pack FILE --registry FILE --out FILE [--detail FILE]\n"
      "      (cross-pack play: per frame the multiset of substitutions and\n"
      "       the hash of the presented frame; --detail lists the multiset)\n"
      "  --golden FILE --candidate FILE   (cross-pack compare: exit 0\n"
      "       equal, 5 different)\n"
      "  --arp FILE --out FILE --content-sha HEX --config-sha HEX  "
      "(trace-from-arp)\n"
      "  --exceptions FILE --catalog FILE --dumps DIR [--registry FILE]\n"
      "      (check: DIR holds <rom>.libretro.jsonl and\n"
      "       <rom>.tessera_native.jsonl for every catalogued ROM; fails on a\n"
      "       divergence no tuple declares and on a declared tuple that no\n"
      "       longer diverges; --registry = Tessera's parity-exceptions.md)\n"
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

/// The port-0 inputs of a run: those of the take `arp`, or `frames` frames
/// of neutral pad. 0 when loaded, otherwise the exit code (the message is
/// already printed, prefixed by `who`).
int load_pads(const std::string &arp, const std::string &frames,
              const char *who, std::vector<std::uint16_t> &pads) {
  if (!arp.empty()) {
    const auto take = ayther::AytherRecording::load(arp);
    if (!take || take->inputs.empty()) {
      std::fprintf(stderr, "%s: cannot read take: %s\n", who, arp.c_str());
      return 1;
    }
    pads = take->inputs;
    return 0;
  }
  char *end = nullptr;
  const unsigned long count = std::strtoul(frames.c_str(), &end, 10);
  if (end == frames.c_str() || *end != '\0' || count == 0 || count > 1000000) {
    std::fprintf(stderr, "%s: --frames needs a count from 1 to 1000000\n", who);
    return kExitUsage;
  }
  pads.assign(count, 0); // neutral pad on every frame
  return 0;
}

bool write_text(const std::string &path, const std::string &text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  file.close();
  return file.good();
}

// parity_bench cross-pack (TP-210.1): exit 0 on success (compare: equal), 5
// when compare finds a difference, 1 on a run that fails, 4 on a write error.
int cross_pack_command(std::string_view action, const std::string &provider,
                       const std::string &core, const std::string &rom,
                       const std::string &arp, const std::string &frames,
                       const std::string &pack, const std::string &registry,
                       const std::string &out, const std::string &golden,
                       const std::string &candidate, const std::string &detail,
                       const std::string &shard) {
  namespace ap = ayther::parity;
  if (action == "compare") {
    if (golden.empty() || candidate.empty()) {
      std::fprintf(stderr,
                   "cross-pack compare needs --golden and --candidate\n");
      return kExitUsage;
    }
    const auto a = read_file(golden);
    const auto b = read_file(candidate);
    if (!a || !b) {
      std::fprintf(stderr, "cross-pack compare: cannot read %s\n",
                   !a ? golden.c_str() : candidate.c_str());
      return 1;
    }
    const auto r = ap::compare_plays(*a, *b);
    if (!r.ok) {
      std::fprintf(stderr, "cross-pack compare: %s\n", r.error.c_str());
      return 1;
    }
    for (const auto &d : r.divergences)
      std::printf("divergence %s first frame %u frames %zu\n", d.field.c_str(),
                  d.frame, d.frames);
    std::printf("cross-pack compare: frames=%zu substituted=%zu %s\n", r.frames,
                r.substituted_frames, r.equal() ? "equal" : "different");
    return r.equal() ? kExitOk : 5;
  }
  const bool native = provider == "tessera_native";
  const bool bake = action == "bake";
  if ((!native && core.empty()) || rom.empty() ||
      arp.empty() == frames.empty() || pack.empty() || registry.empty() ||
      (!bake && out.empty())) {
    std::fprintf(stderr,
                 bake ? "cross-pack bake needs %s--rom, --arp or "
                        "--frames, --pack, --registry\n"
                      : "cross-pack play needs %s--rom, --arp or "
                        "--frames, --pack, --registry, --out\n",
                 native ? "" : "--core, ");
    return kExitUsage;
  }
  std::vector<std::uint16_t> pads;
  if (const int rc = load_pads(arp, frames, "cross-pack", pads); rc != 0)
    return rc;
  const ap::CrossSession session{native, core, rom, pack, registry};
  if (bake) {
    const auto part = ap::parse_shard(shard.empty() ? "1/1" : shard);
    if (!part) {
      std::fprintf(stderr, "cross-pack bake: --shard needs K/N, 1 <= K <= N\n");
      return kExitUsage;
    }
    const auto r = ap::bake_pack(session, pads, pack, registry, *part);
    if (!r.ok) {
      std::fprintf(stderr, "cross-pack bake: %s\n", r.error.c_str());
      return 1;
    }
    std::printf("cross-pack bake: provider=%s shard=%u/%u tiles=%zu "
                "sprites=%zu planes=%zu observed=%zu\n",
                provider.c_str(), part->index + 1, part->count, r.tiles,
                r.sprites, r.planes, r.observed);
    return kExitOk;
  }
  std::string error;
  std::string entries;
  const auto text =
      ap::play_pack(session, pads, &error, detail.empty() ? nullptr : &entries);
  if (!text) {
    std::fprintf(stderr, "cross-pack play: %s\n", error.c_str());
    return 1;
  }
  if (!write_text(out, *text) ||
      (!detail.empty() && !write_text(detail, entries)))
    return 4;
  return kExitOk;
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
      subcommand != "trace-from-arp" && subcommand != "check" &&
      subcommand != "compare-identities" && subcommand != "cross-pack") {
    std::fprintf(stderr, "unknown subcommand: %s\n", argv[1]);
    print_help(stderr);
    return kExitUsage;
  }
  std::string_view action;
  int first_option = 2;
  if (subcommand == "cross-pack") {
    if (argc >= 3)
      action = argv[2];
    if (action != "bake" && action != "play" && action != "compare") {
      std::fprintf(stderr, "cross-pack needs bake, play or compare\n");
      return kExitUsage;
    }
    first_option = 3;
  }

  std::string provider = "libretro";
  std::string arp, out, content_sha, config_sha, core, rom, golden, candidate,
      frames, exceptions, catalog, dumps, registry, identities, segments, pack,
      detail, shard;
  std::vector<std::string> except;
  for (int i = first_option; i < argc; ++i) {
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
                          : arg == "--identities"  ? &identities
                          : arg == "--segments"    ? &segments
                          : arg == "--pack"        ? &pack
                          : arg == "--detail"      ? &detail
                          : arg == "--shard"       ? &shard
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

  if (subcommand == "cross-pack")
    return cross_pack_command(action, provider, core, rom, arp, frames, pack,
                              registry, out, golden, candidate, detail, shard);

  if (subcommand == "check") {
    if (exceptions.empty() || catalog.empty() || dumps.empty()) {
      std::fprintf(stderr, "check needs --exceptions, --catalog, --dumps\n");
      return kExitUsage;
    }
    return check_command(exceptions, catalog, dumps, registry);
  }

  if (subcommand == "compare-identities") {
    if (golden.empty() || candidate.empty()) {
      std::fprintf(stderr,
                   "compare-identities needs --golden and --candidate\n");
      return kExitUsage;
    }
    namespace ap = ayther::parity;
    std::optional<ap::Identities> sets[2];
    const std::string *paths[2] = {&golden, &candidate};
    for (int k = 0; k < 2; ++k) {
      const auto text = read_file(*paths[k]);
      std::string error = "cannot read";
      if (text)
        sets[k] = ap::parse_identities(*text, &error);
      if (!sets[k]) {
        std::fprintf(stderr, "compare-identities: %s: %s\n", paths[k]->c_str(),
                     error.c_str());
        return 1;
      }
    }
    const auto cmp = ap::compare_identities(*sets[0], *sets[1]);
    if (!cmp.ok) {
      std::fprintf(stderr, "compare-identities: %s\n", cmp.error.c_str());
      return 1;
    }
    for (const auto &seg : cmp.segments)
      for (const auto &c : seg.classes)
        std::printf("segment %u-%u %s distinct %zu/%zu missing %zu extra %zu\n",
                    seg.range.first, seg.range.last, c.name.c_str(), c.golden,
                    c.candidate, c.missing, c.extra);
    if (!out.empty()) {
      const std::string json = ap::comparison_json(cmp) + "\n";
      std::ofstream file(out, std::ios::binary | std::ios::trunc);
      file.write(json.data(), static_cast<std::streamsize>(json.size()));
      file.close();
      if (!file.good())
        return 4;
    }
    std::printf("compare-identities: %s\n",
                cmp.equal() ? "equal" : "different");
    return cmp.equal() ? kExitOk : 5;
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
    if (identities.empty() != segments.empty()) {
      std::fprintf(stderr, "dump: --identities and --segments go together\n");
      return kExitUsage;
    }
    std::unique_ptr<ayther::parity::IdentityCollector> collector;
    if (!segments.empty()) {
      const auto ranges = ayther::parity::parse_segments(segments);
      if (!ranges) {
        std::fprintf(stderr, "dump: bad --segments: %s\n", segments.c_str());
        return kExitUsage;
      }
      collector = std::make_unique<ayther::parity::IdentityCollector>(*ranges);
    }
    std::vector<std::uint16_t> pads;
    if (const int rc = load_pads(arp, frames, "dump", pads); rc != 0)
      return rc;
    std::optional<std::string> text;
    if (native) {
      std::ifstream in(rom, std::ios::binary);
      if (!in) {
        std::fprintf(stderr, "dump: cannot read rom: %s\n", rom.c_str());
        return 1;
      }
      const std::vector<std::uint8_t> image{std::istreambuf_iterator<char>(in),
                                            std::istreambuf_iterator<char>()};
      text = ayther::parity::dump_native_session(image, pads, collector.get());
    } else {
      text = ayther::parity::dump_session(core, rom, pads, collector.get());
    }
    if (!text) {
      std::fprintf(stderr, "dump: the session could not be created\n");
      return 1;
    }
    std::ofstream file(out, std::ios::binary | std::ios::trunc);
    file.write(text->data(), static_cast<std::streamsize>(text->size()));
    file.close();
    if (!file.good())
      return 4;
    if (collector) {
      const std::string ids =
          ayther::parity::format_identities(collector->finish());
      std::ofstream idf(identities, std::ios::binary | std::ios::trunc);
      idf.write(ids.data(), static_cast<std::streamsize>(ids.size()));
      idf.close();
      if (!idf.good())
        return 4;
    }
    return kExitOk;
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
