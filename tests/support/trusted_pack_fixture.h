#pragma once

#include <ayther/ayther_core_ffi.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

extern "C" bool ayther_test_pack_builder_finish_trusted(
    AytherPackBuilder* builder,
    const char* pack_path,
    const char* registry_path,
    char* error_buffer,
    size_t error_capacity);

namespace ayther::test {

class TrustedPackFixture {
public:
    explicit TrustedPackFixture(std::string_view name)
        : directory_{make_unique_directory(name)},
          pack_path_{directory_ / "fixture.ay"},
          registry_path_{directory_ / "trust.toml"},
          builder_{ayther_pack_builder_new()} {}

    ~TrustedPackFixture() {
        if (archive_ != nullptr) {
            ayther_pack_close(archive_);
        }
        if (builder_ != nullptr) {
            ayther_pack_builder_free(builder_);
        }
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    TrustedPackFixture(const TrustedPackFixture&) = delete;
    TrustedPackFixture& operator=(const TrustedPackFixture&) = delete;
    TrustedPackFixture(TrustedPackFixture&&) = delete;
    TrustedPackFixture& operator=(TrustedPackFixture&&) = delete;

    [[nodiscard]] AytherPackBuilder* builder() const noexcept { return builder_; }

    [[nodiscard]] bool add_bytes(
        const char* path, const uint8_t* data, size_t length) const {
        return builder_ != nullptr &&
               ayther_pack_builder_add_bytes(builder_, path, data, length);
    }

    [[nodiscard]] bool finish(char* error_buffer, size_t error_capacity) const {
        return builder_ != nullptr && ayther_test_pack_builder_finish_trusted(
            builder_, pack_path_.string().c_str(), registry_path_.string().c_str(),
            error_buffer, error_capacity);
    }

    /// The baked pack and the registry that vouches for it. A caller that
    /// opens the pack through its own code path -- rather than through open()
    /// below -- needs both.
    [[nodiscard]] std::string pack_path() const { return pack_path_.string(); }
    [[nodiscard]] std::string registry_path() const {
        return registry_path_.string();
    }

    [[nodiscard]] AyArchive* open() {
        if (archive_ == nullptr) {
            archive_ = ayther_pack_open_trusted(
                pack_path_.string().c_str(), registry_path_.string().c_str());
        }
        return archive_;
    }

private:
    [[nodiscard]] static std::filesystem::path make_unique_directory(
        std::string_view name) {
        const auto nonce = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        const auto directory = std::filesystem::temp_directory_path() /
            ("ayther_trusted_pack_" + std::string{name} + "_" +
             std::to_string(nonce));
        std::filesystem::create_directories(directory);
        return directory;
    }

    std::filesystem::path directory_;
    std::filesystem::path pack_path_;
    std::filesystem::path registry_path_;
    AytherPackBuilder* builder_{nullptr};
    AyArchive* archive_{nullptr};
};

}  // namespace ayther::test
