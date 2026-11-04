// parity_bench (spec 008, TP-005.13) — plane 3 of the Tessera parity bench.
//
//   parity_bench dump          --provider libretro|tessera_native ...
//   parity_bench compare       --provider libretro|tessera_native ...
//   parity_bench trace-from-arp ...
//
// Skeleton: the subcommands validate their arguments and report that they are
// not implemented yet. dump arrives in TP-005.15 and compare in TP-005.16;
// trace-from-arp (TP-005.14) is implemented in trace_arp.cpp.
#include "trace_arp.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

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
        "  trace-from-arp   convert the inputs of a take (.arp) to a TSTRACE1 trace\n"
        "\n"
        "options:\n"
        "  --arp FILE --out FILE --content-sha HEX --config-sha HEX  (trace-from-arp)\n"
        "  --provider libretro|tessera_native   emulation provider of the session\n"
        "  --help                               show this help\n",
        out);
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
        subcommand != "trace-from-arp") {
        std::fprintf(stderr, "unknown subcommand: %s\n", argv[1]);
        print_help(stderr);
        return kExitUsage;
    }

    std::string provider = "libretro";
    std::string arp, out, content_sha, config_sha;
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
        std::string *target = arg == "--arp"             ? &arp
                              : arg == "--out"           ? &out
                              : arg == "--content-sha"   ? &content_sha
                              : arg == "--config-sha"    ? &config_sha
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

    if (subcommand == "trace-from-arp") {
        if (arp.empty() || out.empty() || content_sha.empty() || config_sha.empty()) {
            std::fprintf(stderr, "trace-from-arp needs --arp, --out, --content-sha, --config-sha\n");
            return kExitUsage;
        }
        const int rc = ayther::parity::trace_from_arp_command(arp, out, content_sha, config_sha);
        if (rc == 1) std::fprintf(stderr, "trace-from-arp: cannot read take: %s\n", arp.c_str());
        if (rc == 2) std::fprintf(stderr, "trace-from-arp: hashes must be 64 hex digits\n");
        if (rc == 4) std::fprintf(stderr, "trace-from-arp: cannot write: %s\n", out.c_str());
        return rc;
    }

    std::fprintf(stderr, "%s: not implemented yet (provider %s)\n",
                 subcommand.data(), provider.c_str());
    return kExitNotImplemented;
}
