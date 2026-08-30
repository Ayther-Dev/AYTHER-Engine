#pragma once
// ---------------------------------------------------------------------------
// trust_scratch.h — bake packs signed by chosen keys, and write the registries
// that do or do not vouch for them.
//
// TrustedPackFixture bakes ONE shape: a single key, wide open, scoped to every
// game. Rotation, revocation, and per-game scope each need a different registry
// against the same pack bytes, and that registry is the thing under test. This
// gives a native test the two primitives it cannot compute for itself -- a
// deterministic public key, and a pack signed by the matching private one --
// and leaves composing the TOML to the test.
// ---------------------------------------------------------------------------
#include <ayther/ayther_core_ffi.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

extern "C" bool ayther_test_public_key_hex(unsigned char seed_byte, char* out_buf,
                                           size_t cap);

extern "C" bool ayther_test_pack_builder_finish_signed_as(
    AytherPackBuilder* builder,
    const char* out_path,
    const char* key_id,
    unsigned char seed_byte,
    char* error_buffer,
    size_t error_capacity);

namespace ayther::test {

class TrustScratch {
public:
    explicit TrustScratch(std::string_view name)
        : directory_{make_unique_directory(name)} {}

    ~TrustScratch() {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    TrustScratch(const TrustScratch&)            = delete;
    TrustScratch& operator=(const TrustScratch&) = delete;

    [[nodiscard]] std::string path_for(std::string_view name) const {
        return (directory_ / std::filesystem::path(name)).string();
    }

    /// The lowercase hex public key of the deterministic key for `seed`.
    [[nodiscard]] static std::string public_key_hex(unsigned char seed) {
        char buffer[65] = {};
        if (!ayther_test_public_key_hex(seed, buffer, sizeof(buffer))) return {};
        return buffer;
    }

    /// One `[[keys]]` block. `games` is written verbatim into the array, so a
    /// caller passes R"("sonic2")" or R"("*")".
    [[nodiscard]] static std::string registry_entry(std::string_view id,
                                                    unsigned char seed,
                                                    uint64_t not_before,
                                                    uint64_t not_after,
                                                    bool revoked,
                                                    std::string_view games) {
        std::string out = "\n[[keys]]\n";
        out += "id = \"";            out += id;                       out += "\"\n";
        out += "algorithm = \"ed25519\"\n";
        out += "public_key = \"";    out += public_key_hex(seed);     out += "\"\n";
        out += "not_before_unix = "; out += std::to_string(not_before); out += "\n";
        out += "not_after_unix = ";  out += std::to_string(not_after);  out += "\n";
        out += "revoked = ";         out += (revoked ? "true" : "false"); out += "\n";
        out += "games = [";          out += games;                    out += "]\n";
        return out;
    }

    /// Writes `entries` as a version-1 registry and returns its path.
    [[nodiscard]] std::string write_registry(std::string_view file,
                                             std::string_view entries) const {
        const std::string path = path_for(file);
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << "version = 1\n" << entries;
        stream.close();
        return path;
    }

    /// Bakes `manifest` into a pack at `file`, signed by `key_id`/`seed`.
    /// Returns an empty string on failure and fills `error` when given.
    [[nodiscard]] std::string bake_pack(std::string_view file,
                                        std::string_view key_id,
                                        unsigned char seed,
                                        std::string_view manifest,
                                        std::string* error = nullptr) const {
        const std::string path = path_for(file);
        AytherPackBuilder* builder = ayther_pack_builder_new();
        if (builder == nullptr) {
            if (error != nullptr) *error = "pack builder allocation failed";
            return {};
        }

        char buffer[512] = {};
        const bool staged = ayther_pack_builder_add_bytes(
            builder, "manifest.toml",
            reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size());
        const bool baked =
            staged && ayther_test_pack_builder_finish_signed_as(
                          builder, path.c_str(), std::string(key_id).c_str(),
                          seed, buffer, sizeof(buffer));
        ayther_pack_builder_free(builder);

        if (!baked) {
            if (error != nullptr) {
                *error = buffer[0] != '\0' ? buffer : "staging the manifest failed";
            }
            return {};
        }
        return path;
    }

private:
    [[nodiscard]] static std::filesystem::path make_unique_directory(
        std::string_view name) {
        const auto nonce =
            std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
            ("ayther_trust_scratch_" + std::string{name} + "_" +
             std::to_string(nonce));
        std::filesystem::create_directories(directory);
        return directory;
    }

    std::filesystem::path directory_;
};

}  // namespace ayther::test
