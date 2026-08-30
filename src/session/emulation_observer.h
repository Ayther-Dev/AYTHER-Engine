#pragma once

#include "ayther_core_ffi.h"
#include "libretro_host/retro_runner.h"

#include <cstdint>
#include <vector>

namespace ayther::session {

// Owns the observable-emulator boundary: subscription negotiation, one-frame
// ABI snapshots, reusable mirror buffers, and the legacy fallback policy.
class EmulationObserver {
public:
    struct AudioWritesView {
        const RetroRunner::AudioWrite* data = nullptr;
        uint32_t count = 0;
        bool abi = false;
    };

    struct ParsedSpritesView {
        const uint8_t* data = nullptr;
        uint32_t count = 0;
        bool abi = false;
    };

    void activate_subscriptions(RetroRunner& runner);
    void reapply_subscriptions(RetroRunner& runner);
    void initialize_system(RetroRunner& runner);
    void verify_subscriptions(RetroRunner& runner);
    void refresh(RetroRunner& runner);

    [[nodiscard]] const uint8_t* vram(const RetroRunner& runner) const;
    [[nodiscard]] const uint8_t* cram(const RetroRunner& runner) const;
    [[nodiscard]] const uint8_t* regs(const RetroRunner& runner) const;
    [[nodiscard]] const uint8_t* vsram(const RetroRunner& runner) const;

    AudioWritesView audio_writes(RetroRunner& runner);
    AudioWritesView audio_writes(RetroRunner& runner,
                                  const ayther_frame_snapshot_v1& snapshot,
                                  bool legacy_fallback);
    ParsedSpritesView parsed_sprites(RetroRunner& runner);

    [[nodiscard]] bool snapshot_available() const noexcept { return snapshot_ok_; }
    [[nodiscard]] const ayther_frame_snapshot_v1& snapshot() const noexcept { return snapshot_; }
    [[nodiscard]] bool system_available() const noexcept { return system_ok_; }
    [[nodiscard]] const ayther_system_v1& system() const noexcept { return system_; }
    [[nodiscard]] uint32_t requested_subscriptions() const noexcept { return requested_; }

    [[nodiscard]] const uint8_t* cached_parsed_sprites(uint8_t* count) const noexcept;
    [[nodiscard]] bool mark_raster_overflow_logged() noexcept;
    [[nodiscard]] bool mark_raster_unsupported_logged() noexcept;

private:
    static bool mirror_enabled();

    uint32_t requested_ = 0;
    bool subscriptions_verified_ = false;
    ayther_frame_snapshot_v1 snapshot_{};
    bool snapshot_ok_ = false;
    ayther_system_v1 system_{};
    bool system_ok_ = false;
    bool system_logged_ = false;
    bool raster_overflow_logged_ = false;
    bool raster_unsupported_logged_ = false;
    std::vector<uint8_t> vram_;
    std::vector<uint8_t> cram_;
    std::vector<uint8_t> regs_;
    std::vector<uint8_t> vsram_;
    std::vector<ayther_sprite_v1> sprites_;
    uint32_t sprite_count_ = 0;
    std::vector<ayther_audio_write_v1> audio_;
    bool sprites_warned_ = false;
    bool audio_warned_ = false;
};

}  // namespace ayther::session
