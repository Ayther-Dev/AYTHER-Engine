#pragma once

#include <ayther/ayther_result.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ayther {
class AytherRenderer;
class AytherSession;
}  // namespace ayther

namespace ayther::engine {

/// Resolution tier declared by an installed pack.
enum class PackRenderTier : std::uint8_t {
    hd = 0,
    full_hd = 1,
    two_k = 2,
    four_k = 3,
    eight_k = 4,
};

/// Strongly typed set of resolution tiers carried by a pack.
class PackRenderTiers final {
public:
    constexpr PackRenderTiers() noexcept = default;
    explicit constexpr PackRenderTiers(std::uint8_t bits) noexcept : bits_(bits) {}

    [[nodiscard]] constexpr bool is_legacy() const noexcept { return bits_ == 0U; }
    [[nodiscard]] constexpr bool contains(PackRenderTier tier) const noexcept {
        return (bits_ & (1U << static_cast<std::uint8_t>(tier))) != 0U;
    }
    [[nodiscard]] constexpr std::uint8_t bits() const noexcept { return bits_; }

private:
    std::uint8_t bits_{};
};

/// Owned metadata copied from a loaded pack.
struct PackInfo {
    std::string game_id;
    std::string name;
    std::string build_id;
    std::string recommended_output_profile;
    std::uint32_t schema_version{};
    std::uint32_t systems_mask{};
    bool declares_systems{};
    PackRenderTiers render_tiers;
};

/// Non-owning view of the pack currently held by an AytherSession.
///
/// Copies do not extend the pack lifetime. The view becomes invalid when the
/// owning session replaces, reloads, or destroys the pack. info() copies its
/// strings before the raw core cache can be invalidated, so the returned value
/// remains independent of the view.
class PackView final {
public:
    constexpr PackView() noexcept = default;
    /// A null literal denotes the same empty view as the default constructor.
    constexpr PackView(std::nullptr_t) noexcept {}

    [[nodiscard]] constexpr bool is_valid() const noexcept { return handle_ != nullptr; }
    explicit constexpr operator bool() const noexcept { return is_valid(); }

    /// Copies the currently loaded pack metadata into an independent value.
    [[nodiscard]] PackInfo info() const;

    /// Returns the declared tier set. An empty set denotes a legacy pack.
    [[nodiscard]] PackRenderTiers render_tiers() const noexcept;

    /// Selects the best declared tier for a presentation height. No-op for an
    /// empty view or a legacy pack.
    void select_render_tier_for_height(std::uint32_t height) const noexcept;

private:
    explicit constexpr PackView(void* handle) noexcept : handle_(handle) {}

    void* handle_{};

    friend class ::ayther::AytherRenderer;
    friend class ::ayther::AytherSession;
};

enum class PackFindingSeverity : std::uint8_t {
    error,
    warning,
    recommendation,
};

struct PackFinding {
    PackFindingSeverity severity{PackFindingSeverity::warning};
    std::string code;
    std::string message;

    [[nodiscard]] bool is_error() const noexcept { return severity == PackFindingSeverity::error; }
};

/// What a caller knows before opening a pack. Empty strings and an absent ROM
/// checksum are reported as unverified rather than assumed compatible.
struct PackValidationContext {
    std::string platform;
    std::string core_build_id;
    std::uint32_t rom_crc32{};
    bool has_rom{};
    bool release_build{};
};

struct PackValidationResult {
    std::vector<PackFinding> findings;

    [[nodiscard]] bool has_errors() const noexcept;
};

/// Opens a pack temporarily and returns owned metadata without starting a core.
[[nodiscard]] Result<PackInfo> inspect_pack(const std::filesystem::path& pack_path,
                                            const std::filesystem::path& trust_registry = {});

/// Validates a pack without opening it for use by a session.
[[nodiscard]] Result<PackValidationResult> validate_pack(const std::filesystem::path& pack_path,
                                                         const PackValidationContext& context = {});

/// Move-only RAII owner of the core's cross-platform file-change watcher.
///
/// poll() is non-blocking and drains all pending events. Destruction stops the
/// watcher thread and releases the hidden core handle.
class PackWatcher final {
public:
    ~PackWatcher();

    PackWatcher(const PackWatcher&) = delete;
    PackWatcher& operator=(const PackWatcher&) = delete;
    PackWatcher(PackWatcher&&) noexcept;
    PackWatcher& operator=(PackWatcher&&) noexcept;

    [[nodiscard]] static Result<PackWatcher> create(const std::filesystem::path& pack_path);

    [[nodiscard]] bool is_active() const noexcept;
    [[nodiscard]] bool poll() noexcept;

private:
    class Impl;

    explicit PackWatcher(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace ayther::engine
