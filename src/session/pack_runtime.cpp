// ---------------------------------------------------------------------------
// pack_runtime.cpp — activation, profiles, assets, and validation for a .ay.
// ---------------------------------------------------------------------------
#include "session/pack_runtime.h"

#include <filesystem>
#include <utility>

namespace ayther::session {
namespace {

std::string field(AyArchive* archive, uint32_t index, const char* name) {
    const char* value = ayther_pack_profile_field(archive, index, name);
    return value != nullptr ? value : std::string{};
}

}  // namespace

PackRuntime::~PackRuntime() { close(); }

void PackRuntime::close() noexcept {
    if (archive_ != nullptr) {
        ayther_pack_close(archive_);
        archive_ = nullptr;
    }
    path_.clear();
    trust_registry_.clear();
    // The chosen profile belonged to the pack being closed. Two packs can each
    // declare an "enhanced" and mean different things, so carrying the hint
    // across would make the next pack report a profile nobody picked for it.
    profile_hint_.clear();
}

Result<void> PackRuntime::open(const std::string& path) {
    close();
    if (path.empty() || !std::filesystem::exists(path)) {
        return Result<void>::ok();  // no pack is a state, not a failure
    }

    AyArchive* opened = ayther_pack_open(path.c_str());
    if (opened == nullptr) {
        return Result<void>::fail(ErrorCode::BadFormat,
                                  "pack exists but failed to open: " + path);
    }
    archive_ = opened;
    path_ = path;
    return Result<void>::ok();
}

Result<void> PackRuntime::open_trusted(const std::string& path,
                                       const std::string& trust_registry) {
    close();
    if (path.empty() || !std::filesystem::exists(path)) {
        return Result<void>::ok();
    }

    AyArchive* opened =
        ayther_pack_open_trusted(path.c_str(), trust_registry.c_str());
    if (opened == nullptr) {
        // A trust failure and a malformed container are both "this pack does
        // not open", but the caller needs to know trust was in play.
        return Result<void>::fail(
            ErrorCode::BadSignature,
            "pack rejected by the trust registry '" + trust_registry +
                "': " + path);
    }
    archive_ = opened;
    path_ = path;
    trust_registry_ = trust_registry;
    return Result<void>::ok();
}

Result<void> PackRuntime::reload() {
    if (path_.empty()) return Result<void>::ok();
    const std::string path = path_;
    const std::string registry = trust_registry_;
    return registry.empty() ? open(path) : open_trusted(path, registry);
}

const char* PackRuntime::game_id() const noexcept {
    if (archive_ == nullptr) return "";
    const char* id = ayther_pack_game_id(archive_);
    return id != nullptr ? id : "";
}

uint32_t PackRuntime::profile_count() const noexcept {
    return archive_ != nullptr ? ayther_pack_profile_count(archive_) : 0;
}

std::optional<PackRuntime::Profile> PackRuntime::profile(uint32_t index) const {
    if (archive_ == nullptr || index >= profile_count()) return std::nullopt;
    Profile out;
    out.index = index;
    out.id = field(archive_, index, "id");
    out.name = field(archive_, index, "name");
    out.systems = ayther_pack_profile_systems(archive_, index);
    out.muted_buses = ayther_pack_profile_muted_buses(archive_, index);
    return out;
}

std::optional<PackRuntime::Profile> PackRuntime::profile_by_id(
    const std::string& id) const {
    if (archive_ == nullptr || id.empty()) return std::nullopt;
    // A profile that does not exist is NOT approximated: applying "the closest
    // one" would show the user something they never asked for, silently.
    const int32_t index = ayther_pack_profile_index(archive_, id.c_str());
    if (index < 0) return std::nullopt;
    return profile(static_cast<uint32_t>(index));
}

std::optional<PackRuntime::Profile> PackRuntime::default_profile() const {
    if (archive_ == nullptr) return std::nullopt;
    return profile(ayther_pack_default_profile(archive_));
}

std::vector<PackRuntime::Profile> PackRuntime::profiles() const {
    std::vector<Profile> out;
    const uint32_t count = profile_count();
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (std::optional<Profile> entry = profile(i)) {
            out.push_back(std::move(*entry));
        }
    }
    return out;
}

std::string PackRuntime::active_profile(uint32_t systems,
                                        uint32_t muted_buses) const {
    if (archive_ == nullptr) return {};

    const auto matches = [&](uint32_t index) {
        return ayther_pack_profile_systems(archive_, index) == systems &&
               ayther_pack_profile_muted_buses(archive_, index) == muted_buses;
    };

    // The user's CHOICE first, but only while the state still supports it: two
    // profiles can have the same effect, and deducing from state alone would
    // return whichever came first. Verifying the hint is what keeps one truth.
    if (!profile_hint_.empty()) {
        const int32_t hinted =
            ayther_pack_profile_index(archive_, profile_hint_.c_str());
        if (hinted >= 0 && matches(static_cast<uint32_t>(hinted))) {
            return profile_hint_;
        }
    }

    const uint32_t count = profile_count();
    for (uint32_t i = 0; i < count; ++i) {
        if (matches(i)) return field(archive_, i, "id");
    }
    // Empty is a legitimate answer: the user changed something and the state
    // stopped being one any profile describes. That is "custom" -- it is not
    // declared, it is arrived at.
    return {};
}

bool PackRuntime::declares_systems() const noexcept {
    return archive_ != nullptr && ayther_pack_declares_systems(archive_);
}

uint32_t PackRuntime::systems() const noexcept {
    return archive_ != nullptr ? ayther_pack_systems(archive_) : 0;
}

int64_t PackRuntime::asset_size(const std::string& logical_path) const {
    if (archive_ == nullptr || logical_path.empty()) return -1;
    return ayther_pack_file_size(archive_, logical_path.c_str());
}

bool PackRuntime::has_asset(const std::string& logical_path) const {
    return asset_size(logical_path) > 0;
}

std::vector<PackRuntime::Finding> PackRuntime::validate(
    const std::string& pack_path, const ValidateContext& context) {
    std::vector<Finding> out;
    if (pack_path.empty()) return out;

    AytherValidateCtx ctx{};
    ctx.rom_crc32 = context.rom_crc32;
    ctx.has_rom = context.has_rom;
    ctx.platform = context.platform.empty() ? nullptr : context.platform.c_str();
    ctx.core_build_id =
        context.core_build_id.empty() ? nullptr : context.core_build_id.c_str();
    ctx.engine_version = nullptr;
    ctx.release_build = context.release_build;

    AytherPackReport* report = ayther_pack_validate(pack_path.c_str(), &ctx);
    if (report == nullptr) return out;

    const uint32_t count = ayther_pack_report_count(report);
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const char* code = ayther_pack_report_code(report, i);
        const char* message = ayther_pack_report_message(report, i);
        out.push_back(Finding{ayther_pack_report_severity(report, i) == 0,
                              code != nullptr ? code : "",
                              message != nullptr ? message : ""});
    }
    ayther_pack_report_free(report);
    return out;
}

}  // namespace ayther::session
