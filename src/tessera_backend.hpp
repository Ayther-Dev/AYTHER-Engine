#pragma once

#include <ayther/engine/native_emulation.hpp>
#include <ayther/observation/observation_source.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace ayther::engine::detail {

// Owns a native session token and the last structured result, and is the
// session's ObservationSource (spec 008, TP-301.2): the observation primitives
// over Tessera API 1.1, with Tessera's statuses translated to the ABI's so the
// code shared with RetroRunner behaves the same for both providers.
class TesseraBackend final : public ObservationSource {
public:
  TesseraBackend() = default;
  ~TesseraBackend() override;
  TesseraBackend(const TesseraBackend &) = delete;
  TesseraBackend &operator=(const TesseraBackend &) = delete;
  std::uint64_t handle{};
  NativeResult last;

  /// Describes the session's observation surface once it exists. Tessera API
  /// 1.1 does not publish the ABI version or the capability mask yet (pending
  /// in its observation contract), so they are fixed here: a backend that
  /// serves subscriptions is the native machine, which announces what gpgx
  /// r4 does (0xEFFFF, without MODE4_CONTROLS); one that serves the region
  /// table without subscriptions is a fixed-region test backend
  /// (`synthetic.complete`). Anything else has no surface.
  void bind_observation(std::string_view backend_id,
                        const std::array<std::uint8_t, 32> &build_id);

  [[nodiscard]] const ObservationInfo *observation_info() const override {
    return observable_ ? &info_ : nullptr;
  }
  [[nodiscard]] bool provides(ObservationFn fn) const override;

  std::int32_t query_region(std::uint32_t region_id, ayther_region_info_v1 *out,
                            std::uint32_t out_size) const override;
  std::int32_t read_region(std::uint32_t region_id, std::uint32_t offset,
                           void *out, std::uint32_t byte_count,
                           std::uint64_t expected_generation,
                           std::uint64_t *actual_generation) const override;
  std::int32_t write_control(std::uint32_t region_id, std::uint32_t offset,
                             const void *data, std::uint32_t byte_count,
                             std::uint64_t expected_generation,
                             std::uint64_t *new_generation) const override;
  std::int32_t capture_snapshot(ayther_frame_snapshot_v1 *out,
                                std::uint32_t out_size) const override;
  std::int32_t recompose_frame(std::uint16_t *out_pixels,
                               std::uint32_t pixel_capacity,
                               std::uint32_t flags, std::uint32_t *out_width,
                               std::uint32_t *out_height) const override;
  std::int32_t poll_audio_events(ayther_audio_event_v1 *out,
                                 std::uint32_t event_capacity,
                                 std::uint32_t *out_event_count) const override;
  std::int32_t get_audio_transport_stats(ayther_audio_transport_stats_v1 *out,
                                         std::uint32_t out_size) const override;
  std::int32_t get_subscriptions(ayther_subscription_state_v1 *out,
                                 std::uint32_t out_size) const override;
  std::int32_t set_subscriptions(std::uint32_t requested_mask) const override;
  std::int32_t poll_frame_delta(ayther_frame_delta_v1 *out,
                                std::uint32_t out_size) const override;
  std::int32_t get_recompose_stats(ayther_recompose_stats_v1 *out,
                                   std::uint32_t out_size) const override;
  std::int32_t
  recompose_multilayer(std::uint16_t *out_bg_a, std::uint16_t *out_bg_b,
                       std::uint16_t *out_window, std::uint16_t *out_sprites,
                       std::uint16_t *out_composite,
                       std::uint32_t pixel_capacity, std::uint32_t flags,
                       std::uint32_t *out_width,
                       std::uint32_t *out_height) const override;
  std::int32_t frame_delta_since(std::uint64_t generation_from,
                                 ayther_frame_delta_v1 *out,
                                 std::uint32_t out_size) const override;

  /// Capability masks fixed while the API does not publish them.
  static constexpr std::uint64_t kMachineCapabilities = UINT64_C(0xEFFFF);
  static constexpr std::uint64_t kFixedRegionCapabilities =
      AYTHER_CAP_REGION_QUERY | AYTHER_CAP_REGION_READ |
      AYTHER_CAP_CONTROL_WRITE;

private:
  bool observable_ = false;
  bool subscriptions_ = false;
  std::string build_id_text_;
  ObservationInfo info_{};
  /// The recomposition rejections Tessera folds into WRONG_STATE are told
  /// apart with SYSTEM, as the ABI names them.
  std::int32_t recompose_rejection(std::int32_t status, bool multilayer) const;
};

/// The Engine's private door into a NativeEmulation.
struct NativeEmulationAccess {
  static TesseraBackend &backend(NativeEmulation &emulation) noexcept {
    return *emulation.backend_;
  }
};

/// The observation surface of a native session (TP-301.2).
inline ObservationSource &observation_source(NativeEmulation &emulation) {
  return NativeEmulationAccess::backend(emulation);
}

} // namespace ayther::engine::detail
