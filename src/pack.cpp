#include <ayther/engine/pack.hpp>

#include "ayther_core_ffi.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ayther::engine {
namespace {

struct ArchiveDeleter {
    void operator()(AyArchive* archive) const noexcept { ayther_pack_close(archive); }
};

struct ReportDeleter {
    void operator()(AytherPackReport* report) const noexcept { ayther_pack_report_free(report); }
};

using ArchiveOwner = std::unique_ptr<AyArchive, ArchiveDeleter>;
using ReportOwner = std::unique_ptr<AytherPackReport, ReportDeleter>;

[[nodiscard]] std::string copy_string(const char* value) {
    return value != nullptr ? value : std::string{};
}

[[nodiscard]] std::string read_pack_name(const AyArchive* archive) {
    constexpr const char* kManifestPath = "manifest.toml";
    const auto byte_count = ayther_pack_file_size(archive, kManifestPath);
    if (byte_count <= 0) {
        return {};
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(byte_count));
    if (ayther_pack_read(archive, kManifestPath, bytes.data(), bytes.size()) != byte_count) {
        return {};
    }

    try {
        const auto manifest = toml::parse(
            std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        return manifest["pack"]["name"].value_or(std::string{});
    } catch (const toml::parse_error&) {
        return {};
    }
}

[[nodiscard]] PackInfo read_info(AyArchive* archive) {
    if (archive == nullptr) {
        return {};
    }

    PackInfo result;
    result.game_id = copy_string(ayther_pack_game_id(archive));
    result.name = read_pack_name(archive);
    result.recommended_output_profile = copy_string(ayther_pack_meta_field(archive, "output"));
    result.build_id = copy_string(ayther_pack_build_id(archive));
    result.schema_version = ayther_pack_schema(archive);
    result.systems_mask = ayther_pack_systems(archive);
    result.declares_systems = ayther_pack_declares_systems(archive);
    result.render_tiers = PackRenderTiers{ayther_pack_tiers(archive)};
    return result;
}

[[nodiscard]] PackFindingSeverity severity_from_raw(std::int32_t severity) {
    switch (severity) {
    case 0:
        return PackFindingSeverity::error;
    case 2:
        return PackFindingSeverity::recommendation;
    default:
        return PackFindingSeverity::warning;
    }
}

}  // namespace

PackInfo PackView::info() const { return read_info(static_cast<AyArchive*>(handle_)); }

PackRenderTiers PackView::render_tiers() const noexcept {
    const auto* archive = static_cast<const AyArchive*>(handle_);
    return PackRenderTiers{archive != nullptr ? ayther_pack_tiers(archive) : std::uint8_t{0}};
}

void PackView::select_render_tier_for_height(const std::uint32_t height) const noexcept {
    auto* archive = static_cast<AyArchive*>(handle_);
    if (archive == nullptr) {
        return;
    }
    constexpr auto kMaxHeight = static_cast<std::uint32_t>(std::numeric_limits<int>::max());
    const auto clamped_height = std::min(height, kMaxHeight);
    ayther_pack_set_tier_for_height(archive, static_cast<int>(clamped_height));
}

bool PackValidationResult::has_errors() const noexcept {
    return std::any_of(findings.begin(), findings.end(),
                       [](const PackFinding& finding) { return finding.is_error(); });
}

Result<PackInfo> inspect_pack(const std::filesystem::path& pack_path,
                              const std::filesystem::path& trust_registry) {
    std::error_code filesystem_error;
    if (pack_path.empty() || !std::filesystem::is_regular_file(pack_path, filesystem_error)) {
        return Error{ErrorCode::NotFound, "pack does not exist: " + pack_path.string()};
    }

    const std::string path = pack_path.string();
    const std::string registry = trust_registry.string();
    ArchiveOwner archive{registry.empty()
                             ? ayther_pack_open(path.c_str())
                             : ayther_pack_open_trusted(path.c_str(), registry.c_str())};
    if (!archive) {
        return Error{ErrorCode::BadFormat, "pack failed to open: " + path};
    }
    return read_info(archive.get());
}

Result<PackValidationResult> validate_pack(const std::filesystem::path& pack_path,
                                           const PackValidationContext& context) {
    if (pack_path.empty()) {
        return Error{ErrorCode::NotFound, "pack path is empty"};
    }

    const std::string path = pack_path.string();
    AytherValidateCtx raw_context{};
    raw_context.rom_crc32 = context.rom_crc32;
    raw_context.has_rom = context.has_rom;
    raw_context.platform = context.platform.empty() ? nullptr : context.platform.c_str();
    raw_context.core_build_id =
        context.core_build_id.empty() ? nullptr : context.core_build_id.c_str();
    raw_context.engine_version = nullptr;
    raw_context.release_build = context.release_build;

    ReportOwner report{ayther_pack_validate(path.c_str(), &raw_context)};
    if (!report) {
        return Error{ErrorCode::BadFormat, "pack validation could not read: " + path};
    }

    PackValidationResult result;
    const auto finding_count = ayther_pack_report_count(report.get());
    result.findings.reserve(finding_count);
    for (std::uint32_t index = 0; index < finding_count; ++index) {
        result.findings.push_back(PackFinding{
            severity_from_raw(ayther_pack_report_severity(report.get(), index)),
            copy_string(ayther_pack_report_code(report.get(), index)),
            copy_string(ayther_pack_report_message(report.get(), index)),
        });
    }
    return result;
}

class PackWatcher::Impl final {
public:
    explicit Impl(AytherPackWatcher* watcher) noexcept : watcher_(watcher) {}
    ~Impl() { ayther_pack_watcher_free(watcher_); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] bool poll() noexcept { return ayther_pack_watcher_poll(watcher_); }

private:
    AytherPackWatcher* watcher_{};
};

PackWatcher::PackWatcher(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

PackWatcher::~PackWatcher() = default;
PackWatcher::PackWatcher(PackWatcher&&) noexcept = default;
PackWatcher& PackWatcher::operator=(PackWatcher&&) noexcept = default;

Result<PackWatcher> PackWatcher::create(const std::filesystem::path& pack_path) {
    if (pack_path.empty()) {
        return Error{ErrorCode::NotFound, "pack watcher path is empty"};
    }

    const std::string path = pack_path.string();
    auto* watcher = ayther_pack_watcher_new(path.c_str());
    if (watcher == nullptr) {
        return Error{ErrorCode::Io, "could not watch pack path: " + path};
    }
    return PackWatcher{std::make_unique<Impl>(watcher)};
}

bool PackWatcher::is_active() const noexcept { return impl_ != nullptr; }

bool PackWatcher::poll() noexcept { return impl_ != nullptr && impl_->poll(); }

}  // namespace ayther::engine
