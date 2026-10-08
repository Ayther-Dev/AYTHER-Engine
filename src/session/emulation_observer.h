#pragma once

#include "ayther_core_ffi.h"
#include "libretro_host/retro_runner.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace ayther::session {

/// Whether the frame snapshot contains the complete parsed-sprite stream.
/// A truncated prefix is never authoritative because mid-frame SAT rewrites
/// can place a required identity beyond the core's capture capacity.
[[nodiscard]] constexpr bool parsed_sprite_capture_complete(
    const ayther_frame_snapshot_v1 &snapshot) noexcept {
  return (snapshot.overflow_flags & AYTHER_OVERFLOW_PARSED_SPRITES) == 0;
}

// Owns the observable-emulator boundary: subscription negotiation, one-frame
// ABI snapshots, reusable mirror buffers, and the legacy fallback policy.
class EmulationObserver {
public:
  struct AudioWritesView {
    const RetroRunner::AudioWrite *data = nullptr;
    uint32_t count = 0;
    bool abi = false;
    std::optional<RetroRunner::AytherReadResult> abi_read{};
  };

  struct ParsedSpritesView {
    const uint8_t *data = nullptr;
    uint32_t count = 0;
    bool abi = false;
    bool complete = true;
  };

  /// A captured ABI view is authoritative even when it is empty or marks an
  /// incomplete frame. Only the absence of a captured view permits legacy.
  [[nodiscard]] static constexpr ParsedSpritesView
  prefer_snapshot_parsed_sprites(
      const std::optional<ParsedSpritesView> &snapshot,
      ParsedSpritesView legacy) noexcept {
    return snapshot.value_or(legacy);
  }

  void activate_subscriptions(RetroRunner &runner);
  void reapply_subscriptions(RetroRunner &runner);
  void initialize_system(RetroRunner &runner);
  void verify_subscriptions(RetroRunner &runner);
  void refresh(RetroRunner &runner);

  [[nodiscard]] const uint8_t *vram(const RetroRunner &runner) const;
  [[nodiscard]] const uint8_t *cram(const RetroRunner &runner) const;
  [[nodiscard]] const uint8_t *regs(const RetroRunner &runner) const;
  [[nodiscard]] const uint8_t *vsram(const RetroRunner &runner) const;

  AudioWritesView audio_writes(RetroRunner &runner);
  AudioWritesView audio_writes(RetroRunner &runner,
                               const ayther_frame_snapshot_v1 &snapshot,
                               bool legacy_fallback);
  ParsedSpritesView parsed_sprites(RetroRunner &runner);

  [[nodiscard]] bool snapshot_available() const noexcept {
    return snapshot_ok_;
  }
  [[nodiscard]] const ayther_frame_snapshot_v1 &snapshot() const noexcept {
    return snapshot_;
  }
  [[nodiscard]] bool system_available() const noexcept { return system_ok_; }
  /// True when this frame either came from a genuine legacy core or has a
  /// successfully captured ABI snapshot and freshly read SYSTEM. False
  /// prevents stale observation state from being mixed with a newer
  /// framebuffer.
  [[nodiscard]] bool system_current_for_frame() const noexcept {
    return system_current_for_frame_;
  }
  [[nodiscard]] const ayther_system_v1 &system() const noexcept {
    return system_;
  }
  [[nodiscard]] uint32_t requested_subscriptions() const noexcept {
    return requested_;
  }

  [[nodiscard]] std::optional<ParsedSpritesView>
  cached_parsed_sprites() const noexcept;
  /// Exact internal view of the frame capture. ABI counts remain uint32_t;
  /// legacy memory is consulted only when no captured ABI view exists.
  [[nodiscard]] ParsedSpritesView
  cached_or_legacy_parsed_sprites(const RetroRunner &runner) const;
  /// Completeness includes both the core's overflow flag and success of the
  /// generation-validated region read.
  [[nodiscard]] bool parsed_sprites_complete_for_frame() const noexcept {
    return !snapshot_ok_ || (sprite_cache_valid_ && sprite_cache_complete_);
  }
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
  bool system_current_for_frame_ = true;
  bool system_logged_ = false;
  bool raster_overflow_logged_ = false;
  bool raster_unsupported_logged_ = false;
  std::vector<uint8_t> vram_;
  std::vector<uint8_t> cram_;
  std::vector<uint8_t> regs_;
  std::vector<uint8_t> vsram_;
  std::vector<ayther_sprite_v1> sprites_;
  std::vector<uint8_t> sprite_bytes_le_;
  mutable std::vector<uint8_t> legacy_sprite_bytes_le_;
  uint32_t sprite_count_ = 0;
  bool sprite_cache_valid_ = false;
  bool sprite_cache_complete_ = false;
  std::vector<ayther_audio_write_v1> audio_;
  bool sprites_warned_ = false;
  bool sprites_overflow_warned_ = false;
  bool audio_warned_ = false;
};

} // namespace ayther::session
