// ---------------------------------------------------------------------------
// make_test_pack — bakes a production-signed pack and the registry that vouches
// for it, at paths the caller chooses.
//
// The release-candidate consumer has to open a TRUSTED pack: an optimized build
// refuses an unsigned one and refuses the development key, so nothing else
// opens. That leaves a chicken-and-egg problem for a demonstration, because the
// only signed content is produced by the test fixtures inside this repository.
//
// This is that fixture, as a standalone tool, so a consumer running from an
// installed prefix can be handed real signed content without linking any test
// scaffolding of its own.
// ---------------------------------------------------------------------------
#include "../common/synth_rom.h"

#include <ayther/ayther_core_ffi.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

extern "C" bool ayther_test_public_key_hex(unsigned char seed_byte, char* out_buf,
                                           size_t cap);

extern "C" bool ayther_test_pack_builder_finish_signed_as(
    AytherPackBuilder* builder, const char* out_path, const char* key_id,
    unsigned char seed_byte, char* error_buffer, size_t error_capacity);

namespace {

constexpr unsigned char kSeed = 11;
constexpr const char* kKeyId = "ayther-rc-consumer";
/// 2100-01-01: past any clock this fixture will meet.
constexpr const char* kNotAfter = "4102444800";

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: make_test_pack <pack-path> <registry-path> "
                     "[game_id] [rom-path]\n");
        return 2;
    }
    const std::string pack_path = argv[1];
    const std::string registry_path = argv[2];
    const std::string game_id = argc > 3 ? argv[3] : "crc32:rc000001";

    std::error_code ec;
    for (const std::string& path : {pack_path, registry_path}) {
        const std::filesystem::path parent =
            std::filesystem::path(path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        std::filesystem::remove(path, ec);
    }

    // --- The registry -----------------------------------------------------
    char public_key[65] = {};
    if (!ayther_test_public_key_hex(kSeed, public_key, sizeof(public_key))) {
        std::fprintf(stderr, "could not derive the signing key\n");
        return 1;
    }
    {
        std::ofstream registry(registry_path, std::ios::binary | std::ios::trunc);
        registry << "version = 1\n\n[[keys]]\n"
                 << "id = \"" << kKeyId << "\"\n"
                 << "algorithm = \"ed25519\"\n"
                 << "public_key = \"" << public_key << "\"\n"
                 << "not_before_unix = 0\n"
                 << "not_after_unix = " << kNotAfter << "\n"
                 << "revoked = false\n"
                 << "games = [\"" << game_id << "\"]\n";
        if (!registry) {
            std::fprintf(stderr, "could not write the registry\n");
            return 1;
        }
    }

    // --- The pack ---------------------------------------------------------
    const std::string manifest =
        "[pack]\n"
        "name       = \"rc_consumer\"\n"
        "version    = \"1.0.0\"\n"
        "game_id    = \"" + game_id + "\"\n"
        "ayther_min = \"0.1.0\"\n"
        "\n[regions]\n"
        "default = \"NTSC\"\n"
        "supported = [\"NTSC\"]\n";
    const std::string asset = "release-candidate consumer asset";

    AytherPackBuilder* builder = ayther_pack_builder_new();
    if (builder == nullptr) {
        std::fprintf(stderr, "could not create the pack builder\n");
        return 1;
    }
    char error[512] = {};
    const bool staged =
        ayther_pack_builder_add_bytes(
            builder, "manifest.toml",
            reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size()) &&
        ayther_pack_builder_add_bytes(
            builder, "assets/tone.bin",
            reinterpret_cast<const uint8_t*>(asset.data()), asset.size());
    const bool baked =
        staged && ayther_test_pack_builder_finish_signed_as(
                      builder, pack_path.c_str(), kKeyId, kSeed, error,
                      sizeof(error));
    ayther_pack_builder_free(builder);

    if (!baked) {
        std::fprintf(stderr, "could not bake the pack: %s\n",
                     error[0] != '\0' ? error : "staging failed");
        return 1;
    }

    // A ROM too, when asked. The release job has no game to hand the consumer
    // and no way to obtain one, and a consumer without a ROM cannot start a
    // session at all -- so it would fall back to link mode and prove less.
    if (argc > 4) {
        const std::string rom_path = argv[4];
        const std::filesystem::path parent =
            std::filesystem::path(rom_path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        const std::string canonical = ayther::synth::canonical_rom_path();
        if (canonical.empty()) {
            std::fprintf(stderr, "could not build the synthetic ROM\n");
            return 1;
        }
        std::filesystem::copy_file(
            canonical, rom_path,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            std::fprintf(stderr, "could not place the ROM: %s\n",
                         ec.message().c_str());
            return 1;
        }
        std::printf("rom=%s\n",
                    std::filesystem::path(rom_path).filename().string().c_str());
    }

    // Only the basenames: this output is quoted in a report that must not carry
    // absolute paths.
    std::printf("pack=%s\nregistry=%s\ngame_id=%s\n",
                std::filesystem::path(pack_path).filename().string().c_str(),
                std::filesystem::path(registry_path).filename().string().c_str(),
                game_id.c_str());
    return 0;
}
