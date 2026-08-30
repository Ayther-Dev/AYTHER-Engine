#pragma once
// ---------------------------------------------------------------------------
// runtime_options.h — every AYTHER_* environment option, read once and frozen.
//
// The engine used to call getenv wherever a decision needed one, parse it with
// atoi —which cannot fail, only return 0— and do it again on the next frame.
// That spread the option contract across a dozen files and made a typo
// indistinguishable from a deliberate zero.
//
// This reads each option ONCE, validates it, and hands subsystems a value they
// cannot modify. A malformed option is reported through diagnostics() and falls
// back to its documented default; it is never silently read as zero.
//
// This header is engine-internal and is NOT installed: it must not appear in
// any public header.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ayther {

class RuntimeOptions {
public:
    /// Reads one variable, returning nullptr when unset. Matches ayther::env_get
    /// so tests can supply a table instead of touching the real environment.
    using Lookup = std::function<const char*(const char*)>;

    // Documented defaults and accepted ranges, in one place so the header is
    // the contract and the parser cannot drift from it.
    static constexpr uint32_t kUploadBudgetDefault = 2;
    static constexpr uint32_t kUploadBudgetMin     = 1;
    static constexpr uint32_t kUploadBudgetMax     = 16;
    static constexpr uint32_t kVideoThreadsMin     = 1;
    static constexpr uint32_t kVideoThreadsMax     = 64;
    /// 0 means "decide from the hardware", not "no threads".
    static constexpr uint32_t kVideoThreadsAuto    = 0;

    /// Defaults only. Useful as an explicit "no options" value in tests.
    RuntimeOptions() = default;

    [[nodiscard]] static RuntimeOptions parse(const Lookup& lookup);
    [[nodiscard]] static RuntimeOptions from_environment();

    /// The process-wide options, parsed on first use. This is the composition
    /// root's default; subsystems should take a const& rather than call it, so
    /// a test can hand them something else.
    [[nodiscard]] static const RuntimeOptions& process();

    /// AYTHER_VOICE_ROUTER — the per-voice mixing path. On unless disabled.
    [[nodiscard]] bool voice_router() const noexcept { return voice_router_; }

    /// AYTHER_ABI_MIRROR — mirror ABI reads into reusable buffers. On unless
    /// disabled.
    [[nodiscard]] bool abi_mirror() const noexcept { return abi_mirror_; }

    /// AYTHER_VK_VERBOSE — Vulkan backend chatter. Off unless enabled.
    [[nodiscard]] bool vulkan_verbose() const noexcept { return vulkan_verbose_; }

    /// AYTHER_VIDEO_DEBUG — per-frame video tracing. Off unless enabled.
    [[nodiscard]] bool video_debug() const noexcept { return video_debug_; }

    /// AYTHER_UPLOAD_BUDGET — texture uploads allowed per frame.
    [[nodiscard]] uint32_t upload_budget() const noexcept { return upload_budget_; }

    /// AYTHER_VIDEO_THREADS — decoder threads, or kVideoThreadsAuto.
    [[nodiscard]] uint32_t video_threads() const noexcept { return video_threads_; }

    /// AYTHER_SYSTEM_DIR — libretro system directory. Empty = derive one.
    [[nodiscard]] const std::string& system_dir() const noexcept { return system_dir_; }

    /// AYTHER_AUDIO_DUMP — tee the emulator PCM to this WAV path. Empty = off.
    [[nodiscard]] const std::string& audio_dump() const noexcept { return audio_dump_; }

    /// AYTHER_SF2_DUMP — tee the SoundFont synth output. Empty = off.
    [[nodiscard]] const std::string& sf2_dump() const noexcept { return sf2_dump_; }

    /// AYTHER_VOICE_DUMP — tee the voice router output. Empty = off.
    [[nodiscard]] const std::string& voice_dump() const noexcept { return voice_dump_; }

    /// One line per option that was set but unusable. Empty when everything
    /// parsed. The composition root reports these; a subsystem should not.
    [[nodiscard]] const std::vector<std::string>& diagnostics() const noexcept {
        return diagnostics_;
    }

    [[nodiscard]] bool valid() const noexcept { return diagnostics_.empty(); }

private:
    void read_bool(const Lookup& lookup, std::string_view name, bool& target);
    void read_uint(const Lookup& lookup, std::string_view name, uint32_t min,
                   uint32_t max, uint32_t& target);
    void read_path(const Lookup& lookup, std::string_view name,
                   std::string& target);

    bool        voice_router_   = true;
    bool        abi_mirror_     = true;
    bool        vulkan_verbose_ = false;
    bool        video_debug_    = false;
    uint32_t    upload_budget_  = kUploadBudgetDefault;
    uint32_t    video_threads_  = kVideoThreadsAuto;
    std::string system_dir_;
    std::string audio_dump_;
    std::string sf2_dump_;
    std::string voice_dump_;
    std::vector<std::string> diagnostics_;
};

}  // namespace ayther
