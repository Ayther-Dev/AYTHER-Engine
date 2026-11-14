// TesseraBackend as an ObservationSource (spec 008, TP-301.2): the observation
// primitives over Tessera API 1.1. Tessera's signatures are those of
// ayther_interface_v1 with the session first and a context last, and its
// layouts are generated from the same ABI (1.10), so the work here is the
// session handle, the size of each struct and the status translation.
#include "tessera_backend.hpp"

#include <cstdio>

#ifndef AYTHER_HAS_TESSERA
#define AYTHER_HAS_TESSERA 0
#endif

#if AYTHER_HAS_TESSERA
#include <tessera/tessera.h>
#include <tessera/tessera_observation.h>
#endif

namespace ayther::engine::detail {

#if AYTHER_HAS_TESSERA
namespace {

// The layouts are the ABI's: a mismatch here is a contract break, not a
// conversion to write.
static_assert(sizeof(tessera_obs_region_info_v1) ==
              sizeof(ayther_region_info_v1));
static_assert(sizeof(tessera_obs_frame_snapshot_v1) ==
              sizeof(ayther_frame_snapshot_v1));
static_assert(sizeof(tessera_obs_audio_transport_stats_v1) ==
              sizeof(ayther_audio_transport_stats_v1));
static_assert(sizeof(tessera_obs_subscription_state_v1) ==
              sizeof(ayther_subscription_state_v1));
static_assert(sizeof(tessera_obs_frame_delta_v1) ==
              sizeof(ayther_frame_delta_v1));
static_assert(sizeof(tessera_obs_recompose_stats_v1) ==
              sizeof(ayther_recompose_stats_v1));
static_assert(TESSERA_OBS_ABI_VERSION_LATEST == AYTHER_ABI_VERSION_1_10);

tessera_context_v1 context() noexcept {
  tessera_context_v1 value{};
  value.header = {static_cast<std::uint32_t>(sizeof(value)), 1, 0};
  return value;
}

/// The statuses every observation call shares. The ones whose meaning
/// depends on the call (STALE_EPOCH, WRONG_STATE, INVALID_ARGUMENT in a
/// recomposition) are translated by the caller first.
std::int32_t to_abi(tessera_status status) noexcept {
  switch (status) {
  case TESSERA_OK:
    return AYTHER_STATUS_OK;
  case TESSERA_INVALID_ARGUMENT:
    return AYTHER_STATUS_INVALID_ARGUMENT;
  case TESSERA_BUFFER_TOO_SMALL:
    return AYTHER_STATUS_BUFFER_TOO_SMALL;
  case TESSERA_STALE_EPOCH:
    return AYTHER_STATUS_STALE_GENERATION;
  case TESSERA_NOT_SUBSCRIBED:
    return AYTHER_STATUS_NOT_SUBSCRIBED;
  case TESSERA_BUSY:
    return AYTHER_STATUS_BUSY;
  case TESSERA_UNSUPPORTED_CAPABILITY:
  default:
    // A session that cannot answer (closed handle, failed backend) has, for
    // the observing Engine, nothing to offer: the legacy reading of
    // UNSUPPORTED.
    return AYTHER_STATUS_UNSUPPORTED;
  }
}

} // namespace
#endif

void TesseraBackend::bind_observation(
    std::string_view backend_id, const std::array<std::uint8_t, 32> &build_id) {
  observable_ = false;
  subscriptions_ = false;
  info_ = ObservationInfo{};
#if AYTHER_HAS_TESSERA
  if (handle == 0)
    return;
  // A surface exists if the region table answers; the native machine also
  // answers the subscriptions.
  tessera_obs_region_info_v1 region{};
  region.struct_size = sizeof(region);
  auto ctx = context();
  if (tessera_observation_region_query_v1(handle, AYTHER_REGION_SYSTEM, &region,
                                          sizeof(region), &ctx) != TESSERA_OK)
    return;
  tessera_obs_subscription_state_v1 state{};
  state.struct_size = sizeof(state);
  ctx = context();
  subscriptions_ = tessera_observation_subscriptions_get_v1(
                       handle, &state, sizeof(state), &ctx) == TESSERA_OK;
  observable_ = true;

  // The ABI build id is text; Tessera's is 32 bytes. Name the provider, the
  // backend and the build so a recording or a pack says where it came from.
  build_id_text_ = "tessera/";
  build_id_text_.append(backend_id);
  build_id_text_ += '/';
  for (const std::uint8_t byte : build_id) {
    char hex[3];
    std::snprintf(hex, sizeof hex, "%02x", byte);
    build_id_text_ += hex;
  }

  info_.abi_version = AYTHER_ABI_VERSION_1_10;
  info_.capabilities =
      subscriptions_ ? kMachineCapabilities : kFixedRegionCapabilities;
  info_.build_id = build_id_text_;
  info_.region_info_size = sizeof(tessera_obs_region_info_v1);
  info_.frame_snapshot_size = sizeof(tessera_obs_frame_snapshot_v1);
  info_.sprite_size = sizeof(tessera_obs_sprite_v1);
  info_.audio_write_size = sizeof(tessera_obs_audio_write_v1);
  info_.audio_event_size = sizeof(ayther_audio_event_v1);
  info_.audio_transport_stats_size =
      sizeof(tessera_obs_audio_transport_stats_v1);
  info_.subscription_state_size = sizeof(tessera_obs_subscription_state_v1);
  info_.frame_delta_size = sizeof(tessera_obs_frame_delta_v1);
  info_.recompose_stats_size = sizeof(tessera_obs_recompose_stats_v1);
#else
  (void)backend_id;
  (void)build_id;
#endif
}

bool TesseraBackend::provides(ObservationFn fn) const {
  if (!observable_)
    return false;
  switch (fn) {
  case ObservationFn::query_region:
  case ObservationFn::read_region:
  case ObservationFn::write_control:
    return true;
  case ObservationFn::capture_snapshot: // Tessera v0.2.0-rc.3 (TP-302)
  case ObservationFn::recompose_frame:
  case ObservationFn::poll_audio_events:
  case ObservationFn::get_audio_transport_stats:
  case ObservationFn::get_subscriptions:
  case ObservationFn::set_subscriptions:
  case ObservationFn::poll_frame_delta:
  case ObservationFn::get_recompose_stats:
  case ObservationFn::recompose_multilayer:
  case ObservationFn::frame_delta_since:
    // Served by the machine's IObservable only.
    return subscriptions_;
  }
  return false;
}

#if AYTHER_HAS_TESSERA
#define AYTHER_TESSERA_GUARD(fn)                                               \
  if (!provides(fn))                                                           \
    return AYTHER_STATUS_UNSUPPORTED;                                          \
  auto ctx = context()
#endif

std::int32_t TesseraBackend::query_region(std::uint32_t region_id,
                                          ayther_region_info_v1 *out,
                                          std::uint32_t out_size) const {
#if AYTHER_HAS_TESSERA
  AYTHER_TESSERA_GUARD(ObservationFn::query_region);
  return to_abi(tessera_observation_region_query_v1(
      handle, region_id, reinterpret_cast<tessera_obs_region_info_v1 *>(out),
      out_size, &ctx));
#else
  (void)region_id;
  (void)out;
  (void)out_size;
  return AYTHER_STATUS_UNSUPPORTED;
#endif
}

std::int32_t
TesseraBackend::read_region(std::uint32_t region_id, std::uint32_t offset,
                            void *out, std::uint32_t byte_count,
                            std::uint64_t expected_generation,
                            std::uint64_t *actual_generation) const {
#if AYTHER_HAS_TESSERA
  AYTHER_TESSERA_GUARD(ObservationFn::read_region);
  return to_abi(tessera_observation_region_read_v1(
      handle, region_id, offset, out, byte_count, expected_generation,
      actual_generation, &ctx));
#else
  (void)region_id;
  (void)offset;
  (void)out;
  (void)byte_count;
  (void)expected_generation;
  (void)actual_generation;
  return AYTHER_STATUS_UNSUPPORTED;
#endif
}

std::int32_t
TesseraBackend::write_control(std::uint32_t region_id, std::uint32_t offset,
                              const void *data, std::uint32_t byte_count,
                              std::uint64_t expected_generation,
                              std::uint64_t *new_generation) const {
#if AYTHER_HAS_TESSERA
  AYTHER_TESSERA_GUARD(ObservationFn::write_control);
  return to_abi(tessera_observation_write_control_v1(
      handle, region_id, offset, data, byte_count, expected_generation,
      new_generation, &ctx));
#else
  (void)region_id;
  (void)offset;
  (void)data;
  (void)byte_count;
  (void)expected_generation;
  (void)new_generation;
  return AYTHER_STATUS_UNSUPPORTED;
#endif
}

std::int32_t TesseraBackend::capture_snapshot(ayther_frame_snapshot_v1 *out,
                                              std::uint32_t out_size) const {
#if AYTHER_HAS_TESSERA
  // The counters of the last frame run, with the session generation region
  // reads check. Below the 48 bytes it is BUFFER_TOO_SMALL with `out`
  // untouched, as in gpgx; inside a frame, BUSY.
  AYTHER_TESSERA_GUARD(ObservationFn::capture_snapshot);
  return to_abi(tessera_observation_snapshot_capture_v1(
      handle, reinterpret_cast<tessera_obs_frame_snapshot_v1 *>(out), out_size,
      &ctx));
#else
  (void)out;
  (void)out_size;
  return AYTHER_STATUS_UNSUPPORTED;
#endif
}

std::int32_t TesseraBackend::recompose_rejection(std::int32_t status,
                                                 bool multilayer) const {
#if AYTHER_HAS_TESSERA
  if (status == TESSERA_INVALID_ARGUMENT)
    return AYTHER_STATUS_RC_INVALID_PARAMS; // capacity below the frame
  if (status != TESSERA_WRONG_STATE)
    return to_abi(static_cast<tessera_status>(status));
  // WRONG_STATE covers what the ABI names apart: interlace mode 2, Mode 4
  // (multi-layer only; recompose_frame redraws Mode 4) and, for the
  // multi-layer, a journal that lost events.
  ayther_system_v1 system{};
  if (read_system_v1(system).ok()) {
    if (system.interlace == 2)
      return AYTHER_STATUS_RC_INTERLACE2;
    if (multilayer && system.vdp_mode == 4)
      return AYTHER_STATUS_RC_NOT_MODE5;
  }
  return multilayer
             ? static_cast<std::int32_t>(AYTHER_STATUS_RC_JOURNAL_OVERFLOW)
             : static_cast<std::int32_t>(AYTHER_STATUS_UNSUPPORTED);
#else
  (void)status;
  (void)multilayer;
  return AYTHER_STATUS_UNSUPPORTED;
#endif
}

std::int32_t TesseraBackend::recompose_frame(std::uint16_t *out_pixels,
                                             std::uint32_t pixel_capacity,
                                             std::uint32_t flags,
                                             std::uint32_t *out_width,
                                             std::uint32_t *out_height) const {
#if AYTHER_HAS_TESSERA
  AYTHER_TESSERA_GUARD(ObservationFn::recompose_frame);
  const tessera_status status = tessera_observation_recompose_v1(
      handle, out_pixels, pixel_capacity, flags, out_width, out_height, &ctx);
  return status == TESSERA_OK ? AYTHER_STATUS_OK
                              : recompose_rejection(status, false);
#else
  (void)out_pixels;
  (void)pixel_capacity;
  (void)flags;
  (void)out_width;
  (void)out_height;
  return AYTHER_STATUS_UNSUPPORTED;
#endif
}

std::int32_t
TesseraBackend::poll_audio_events(ayther_audio_event_v1 *out,
                                  std::uint32_t event_capacity,
                                  std::uint32_t *out_event_count) const {
#if AYTHER_HAS_TESSERA
  AYTHER_TESSERA_GUARD(ObservationFn::poll_audio_events);
  return to_abi(tessera_observation_audio_events_poll_v1(
      handle, out, event_capacity,
      static_cast<std::uint32_t>(sizeof(ayther_audio_event_v1)),
      out_event_count, &ctx));
#else
  (void)out;
  (void)event_capacity;
  (void)out_event_count;
  return AYTHER_STATUS_UNSUPPORTED;
#endif
}

std::int32_t
TesseraBackend::get_audio_transport_stats(ayther_audio_transport_stats_v1 *out,
                                          std::uint32_t out_size) const {
#if AYTHER_HAS_TESSERA
  AYTHER_TESSERA_GUARD(ObservationFn::get_audio_transport_stats);
  return to_abi(tessera_observation_audio_transport_stats_v1(
      handle, reinterpret_cast<tessera_obs_audio_transport_stats_v1 *>(out),
      out_size, &ctx));
#else
  (void)out;
  (void)out_size;
  return AYTHER_STATUS_UNSUPPORTED;
#endif
}

std::int32_t
TesseraBackend::get_subscriptions(ayther_subscription_state_v1 *out,
                                  std::uint32_t out_size) const {
#if AYTHER_HAS_TESSERA
  AYTHER_TESSERA_GUARD(ObservationFn::get_subscriptions);
  return to_abi(tessera_observation_subscriptions_get_v1(
      handle, reinterpret_cast<tessera_obs_subscription_state_v1 *>(out),
      out_size, &ctx));
#else
  (void)out;
  (void)out_size;
  return AYTHER_STATUS_UNSUPPORTED;
#endif
}

std::int32_t
TesseraBackend::set_subscriptions(std::uint32_t requested_mask) const {
#if AYTHER_HAS_TESSERA
  AYTHER_TESSERA_GUARD(ObservationFn::set_subscriptions);
  return to_abi(
      tessera_observation_subscriptions_set_v1(handle, requested_mask, &ctx));
#else
  (void)requested_mask;
  return AYTHER_STATUS_UNSUPPORTED;
#endif
}

std::int32_t TesseraBackend::poll_frame_delta(ayther_frame_delta_v1 *out,
                                              std::uint32_t out_size) const {
#if AYTHER_HAS_TESSERA
  AYTHER_TESSERA_GUARD(ObservationFn::poll_frame_delta);
  return to_abi(tessera_observation_frame_delta_poll_v1(
      handle, reinterpret_cast<tessera_obs_frame_delta_v1 *>(out), out_size,
      &ctx));
#else
  (void)out;
  (void)out_size;
  return AYTHER_STATUS_UNSUPPORTED;
#endif
}

std::int32_t TesseraBackend::get_recompose_stats(ayther_recompose_stats_v1 *out,
                                                 std::uint32_t out_size) const {
#if AYTHER_HAS_TESSERA
  AYTHER_TESSERA_GUARD(ObservationFn::get_recompose_stats);
  return to_abi(tessera_observation_recompose_stats_v1(
      handle, reinterpret_cast<tessera_obs_recompose_stats_v1 *>(out), out_size,
      &ctx));
#else
  (void)out;
  (void)out_size;
  return AYTHER_STATUS_UNSUPPORTED;
#endif
}

std::int32_t TesseraBackend::recompose_multilayer(
    std::uint16_t *out_bg_a, std::uint16_t *out_bg_b, std::uint16_t *out_window,
    std::uint16_t *out_sprites, std::uint16_t *out_composite,
    std::uint32_t pixel_capacity, std::uint32_t flags, std::uint32_t *out_width,
    std::uint32_t *out_height) const {
#if AYTHER_HAS_TESSERA
  AYTHER_TESSERA_GUARD(ObservationFn::recompose_multilayer);
  const tessera_status status = tessera_observation_recompose_multilayer_v1(
      handle, out_bg_a, out_bg_b, out_window, out_sprites, out_composite,
      pixel_capacity, flags, out_width, out_height, &ctx);
  return status == TESSERA_OK ? AYTHER_STATUS_OK
                              : recompose_rejection(status, true);
#else
  (void)out_bg_a;
  (void)out_bg_b;
  (void)out_window;
  (void)out_sprites;
  (void)out_composite;
  (void)pixel_capacity;
  (void)flags;
  (void)out_width;
  (void)out_height;
  return AYTHER_STATUS_UNSUPPORTED;
#endif
}

std::int32_t TesseraBackend::frame_delta_since(std::uint64_t generation_from,
                                               ayther_frame_delta_v1 *out,
                                               std::uint32_t out_size) const {
#if AYTHER_HAS_TESSERA
  AYTHER_TESSERA_GUARD(ObservationFn::frame_delta_since);
  const tessera_status status = tessera_observation_frame_delta_since_v1(
      handle, generation_from,
      reinterpret_cast<tessera_obs_frame_delta_v1 *>(out), out_size, &ctx);
  // Here STALE_EPOCH is the ring's history lost, `out` filled all dirty: a
  // warning the consumer can go on with, as in the ABI.
  return status == TESSERA_STALE_EPOCH ? AYTHER_STATUS_DELTA_HISTORY_LOST
                                       : to_abi(status);
#else
  (void)generation_from;
  (void)out;
  (void)out_size;
  return AYTHER_STATUS_UNSUPPORTED;
#endif
}

} // namespace ayther::engine::detail
