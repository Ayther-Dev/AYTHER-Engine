#pragma once
// ---------------------------------------------------------------------------
// pack_runtime.h — everything about a .ay pack that a pack alone can answer.
//
// Activation, validation, profiles, the trust registry, and asset lookup used
// to live inside AytherSession::Impl, next to the emulator handle and the
// renderer. That made all of them untestable: asking "does this pack declare a
// default profile" required booting a core and opening an audio device.
//
// This owns the archive and answers those questions on its own. It knows
// nothing about RetroRunner or AytherRenderer, and it must stay that way --
// APPLYING a profile touches audio buses and subsystem masks, so that decision
// belongs to the session, which coordinates. Reporting what the profile SAYS
// belongs here.
// ---------------------------------------------------------------------------
#include "ayther_core_ffi.h"
#include "ayther_result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ayther::session {

class PackRuntime {
public:
    /// One line of a validation report.
    struct Finding {
        bool        blocking = false;  ///< severity 0 == blocking
        std::string code;
        std::string message;
    };

    /// A profile as the pack declares it. `systems` and `muted_buses` are the
    /// masks the session applies; this type does not apply them.
    struct Profile {
        uint32_t    index = 0;
        std::string id;
        std::string name;
        uint32_t    systems = 0;
        uint32_t    muted_buses = 0;
    };

    /// What the caller knows about the machine the pack will run on. Plain
    /// data on purpose: validation is exercised in tests with a synthetic
    /// context instead of a live emulator.
    struct ValidateContext {
        std::string platform;        ///< "megadrive", "segacd"
        std::string core_build_id;   ///< empty when the core did not report one
        uint32_t    rom_crc32 = 0;
        bool        has_rom = false;
        bool        release_build = false;
    };

    PackRuntime() = default;
    ~PackRuntime();

    PackRuntime(const PackRuntime&)            = delete;
    PackRuntime& operator=(const PackRuntime&) = delete;

    // ---- Activation -----------------------------------------------------

    /// Opens `path`, replacing whatever was loaded. An empty or missing path
    /// closes the current pack and succeeds: "no pack" is a valid state, not
    /// an error, and callers rely on that to clear a pack.
    Result<void> open(const std::string& path);

    /// Opens under an explicit production trust registry. A pack whose
    /// signature the registry does not vouch for does not open.
    Result<void> open_trusted(const std::string& path,
                              const std::string& trust_registry);

    /// Re-opens the current path with the same trust settings. Succeeds with
    /// nothing loaded when no pack is open.
    Result<void> reload();

    void close() noexcept;

    [[nodiscard]] bool loaded() const noexcept { return archive_ != nullptr; }
    /// Mirrors the handle-owning shape this replaced, so call sites that only
    /// need the raw archive read the same as before.
    [[nodiscard]] AyArchive* get() const noexcept { return archive_; }
    explicit operator bool() const noexcept { return archive_ != nullptr; }

    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] const std::string& trust_registry() const noexcept {
        return trust_registry_;
    }
    /// The pack's own game id, or "" when no pack is loaded.
    [[nodiscard]] const char* game_id() const noexcept;

    // ---- Profiles -------------------------------------------------------

    [[nodiscard]] uint32_t profile_count() const noexcept;
    [[nodiscard]] std::optional<Profile> profile(uint32_t index) const;
    [[nodiscard]] std::optional<Profile> profile_by_id(const std::string& id) const;
    [[nodiscard]] std::optional<Profile> default_profile() const;
    [[nodiscard]] std::vector<Profile> profiles() const;

    /// Records which profile the user chose. It is a HINT because live state
    /// can drift away from it, and two profiles can describe the same state.
    void set_profile_hint(std::string id) noexcept { profile_hint_ = std::move(id); }
    void clear_profile_hint() noexcept { profile_hint_.clear(); }
    [[nodiscard]] const std::string& profile_hint() const noexcept {
        return profile_hint_;
    }

    /// Which declared profile the given live state corresponds to, or "" when
    /// none does. The hint wins only if the state still supports it: believing
    /// the hint without checking is how a session ends up reporting a profile
    /// the user has already edited away from.
    [[nodiscard]] std::string active_profile(uint32_t systems,
                                             uint32_t muted_buses) const;

    // ---- Declared systems -----------------------------------------------

    [[nodiscard]] bool declares_systems() const noexcept;
    [[nodiscard]] uint32_t systems() const noexcept;

    // ---- Assets ---------------------------------------------------------

    /// Size of a logical entry, or a negative value when it is absent.
    [[nodiscard]] int64_t asset_size(const std::string& logical_path) const;
    [[nodiscard]] bool has_asset(const std::string& logical_path) const;

    // ---- Validation -----------------------------------------------------

    /// Validates a pack on disk -- not necessarily the loaded one, which is
    /// why this is static. The session supplies the context it alone knows.
    [[nodiscard]] static std::vector<Finding> validate(
        const std::string& pack_path, const ValidateContext& context);

private:
    AyArchive*  archive_ = nullptr;
    std::string path_;
    std::string trust_registry_;
    std::string profile_hint_;
};

}  // namespace ayther::session
