// ObservationSource (spec 008, TP-301.1): the reads, polls and controls the
// Engine builds over the observation primitives, written once for every
// provider. The bodies come from RetroRunner, unchanged in behaviour: the
// only difference is that they call the source's primitives instead of the
// core's descriptor.
#include "observation/observation_source.h"

#include "log.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace ayther {

// ---------------------------------------------------------------------------
// Reads. Without the surface they return UNSUPPORTED (the caller follows the
// legacy path); with it the read goes through `read_region`, which validates
// the generation against the snapshot: if the frame moved between the
// snapshot and the read, the provider SAYS so instead of returning memory
// from another instant.
// ---------------------------------------------------------------------------
ObservationSource::AytherReadResult
ObservationSource::capture_frame_snapshot(ayther_frame_snapshot_v1 &out) const {
  AytherReadResult r;
  if (!has_ayther_v1())
    return r;
  out = ayther_frame_snapshot_v1{};
  out.struct_size = sizeof(out);
  r.status = capture_snapshot(&out, sizeof(out));
  if (r.status == AYTHER_STATUS_OK)
    r.generation = out.snapshot_generation;
  return r;
}

ObservationSource::AytherReadResult
ObservationSource::read_region_v1(std::uint32_t region, void *out,
                                  std::uint32_t bytes,
                                  std::uint64_t generation) const {
  AytherReadResult r;
  if (!has_ayther_v1() || !out || !bytes)
    return r;
  r.status = read_region(region, 0, out, bytes, generation, &r.generation);
  if (r.status == AYTHER_STATUS_OK)
    r.count = bytes;
  return r;
}

namespace {

// The four VDP regions share a body: the size is declared by the provider
// (query_region), so the Engine does not hardcode 64 KB of VRAM or 128 B of
// CRAM.
ObservationSource::AytherReadResult
read_vdp_region(const ObservationSource &source, std::uint32_t region,
                void *out, const ayther_frame_snapshot_v1 &s) {
  ObservationSource::AytherReadResult r;
  if (!source.has_ayther_v1() || !out)
    return r;
  ayther_region_info_v1 info{};
  info.struct_size = sizeof(info);
  if (source.query_region(region, &info, sizeof(info)) != AYTHER_STATUS_OK)
    return r;
  if (!info.byte_size) {
    r.status = AYTHER_STATUS_OK;
    return r;
  }
  return source.read_region_v1(region, out, info.byte_size,
                               s.snapshot_generation);
}

} // namespace

std::size_t ObservationSource::abi_region_bytes(std::uint32_t region) const {
  if (!has_ayther_v1() || !provides(ObservationFn::query_region))
    return 0;
  ayther_region_info_v1 info{};
  info.struct_size = sizeof(info);
  if (query_region(region, &info, sizeof(info)) != AYTHER_STATUS_OK)
    return 0;
  return static_cast<std::size_t>(info.byte_size);
}

ObservationSource::AytherReadResult
ObservationSource::read_vram_v1(void *out,
                                const ayther_frame_snapshot_v1 &s) const {
  return read_vdp_region(*this, AYTHER_REGION_VRAM, out, s);
}
ObservationSource::AytherReadResult
ObservationSource::read_cram_v1(void *out,
                                const ayther_frame_snapshot_v1 &s) const {
  return read_vdp_region(*this, AYTHER_REGION_CRAM, out, s);
}
ObservationSource::AytherReadResult
ObservationSource::read_vdp_regs_v1(void *out,
                                    const ayther_frame_snapshot_v1 &s) const {
  return read_vdp_region(*this, AYTHER_REGION_VDP_REGS, out, s);
}
ObservationSource::AytherReadResult
ObservationSource::read_vsram_v1(void *out,
                                 const ayther_frame_snapshot_v1 &s) const {
  return read_vdp_region(*this, AYTHER_REGION_VSRAM, out, s);
}

ObservationSource::AytherReadResult ObservationSource::read_parsed_sprites_v1(
    ayther_sprite_v1 *out, std::uint32_t max,
    const ayther_frame_snapshot_v1 &s) const {
  AytherReadResult r;
  if (!has_ayther_v1() || !has_capability(AYTHER_CAP_PARSED_SPRITES_V1))
    return r;
  // (std::min) in parentheses: windows.h defines min as a MACRO.
  const std::uint32_t n = (std::min)(max, s.parsed_sprite_count);
  if (s.parsed_sprite_count == 0) {
    r.status = AYTHER_STATUS_OK;
    r.generation = s.snapshot_generation;
    return r;
  }
  if (!out)
    return r;
  if (n == 0) {
    r.status = AYTHER_STATUS_OK;
    r.generation = s.snapshot_generation;
    return r;
  }

  ayther_region_info_v1 info{};
  info.struct_size = sizeof(info);
  if (query_region(AYTHER_REGION_PARSED_SPRITES, &info, sizeof(info)) !=
      AYTHER_STATUS_OK)
    return r;

  if (info.element_size > sizeof(ayther_sprite_v1)) {
    // FORWARD-COMPAT: the provider has fields this Engine does not know yet.
    // Read with ITS stride and copy only the known struct's bytes — reading
    // with our own stride would misalign the whole array.
    std::vector<std::uint8_t> tmp(static_cast<std::size_t>(n) *
                                  info.element_size);
    r.status = read_region(AYTHER_REGION_PARSED_SPRITES, 0, tmp.data(),
                           static_cast<std::uint32_t>(tmp.size()),
                           s.snapshot_generation, &r.generation);
    if (r.status == AYTHER_STATUS_OK) {
      for (std::uint32_t i = 0; i < n; ++i)
        std::memcpy(&out[i], tmp.data() + std::size_t(i) * info.element_size,
                    sizeof(ayther_sprite_v1));
      r.count = n;
    }
    return r;
  }
  r.status =
      read_region(AYTHER_REGION_PARSED_SPRITES, 0, out,
                  n * static_cast<std::uint32_t>(sizeof(ayther_sprite_v1)),
                  s.snapshot_generation, &r.generation);
  if (r.status == AYTHER_STATUS_OK)
    r.count = n;
  return r;
}

ObservationSource::AytherReadResult ObservationSource::read_audio_writes_v1(
    ayther_audio_write_v1 *out, std::uint32_t max,
    const ayther_frame_snapshot_v1 &s) const {
  AytherReadResult r;
  if (!has_ayther_v1() || !out)
    return r;
  const std::uint32_t n = (std::min)(max, s.audio_write_count);
  if (n == 0) {
    r.status = AYTHER_STATUS_OK;
    return r;
  }
  r.status =
      read_region(AYTHER_REGION_AUDIO_WRITES, 0, out,
                  n * static_cast<std::uint32_t>(sizeof(ayther_audio_write_v1)),
                  s.snapshot_generation, &r.generation);
  if (r.status == AYTHER_STATUS_OK)
    r.count = n;
  return r;
}

std::uint32_t ObservationSource::read_raster_fallback_v1(
    const ayther_frame_snapshot_v1 &s) const {
  return has_ayther_v1() ? s.fallback_reasons : 0u;
}

ObservationSource::AytherReadResult
ObservationSource::read_system_v1(ayther_system_v1 &out) const {
  AytherReadResult r;
  if (!has_ayther_v1() || !provides(ObservationFn::read_region))
    return r;
  if (!has_capability(AYTHER_CAP_SYSTEM_V1))
    return r;
  out = ayther_system_v1{};
  out.struct_size = sizeof(out);
  out.layout_version = AYTHER_LAYOUT_SYSTEM_V1;
  // No subscription and no generation: SYSTEM is filled on read and
  // describes the loaded content, not a frame.
  r.status = read_region(AYTHER_REGION_SYSTEM, 0, &out, sizeof(out),
                         AYTHER_GENERATION_ANY, &r.generation);
  if (r.status == AYTHER_STATUS_OK &&
      (out.struct_size < sizeof(out) ||
       out.layout_version != AYTHER_LAYOUT_SYSTEM_V1)) {
    // A layout this Engine does not understand is discarded instead of read
    // misaligned.
    r.status = AYTHER_STATUS_UNSUPPORTED;
  }
  if (r.status == AYTHER_STATUS_OK)
    r.count = 1;
  return r;
}

// ---------------------------------------------------------------------------
// Subscriptions
// ---------------------------------------------------------------------------
std::uint32_t ObservationSource::subscribe_all_supported() const {
  if (!has_ayther_v1() || !has_capability(AYTHER_CAP_SUBSCRIPTIONS_V1))
    return 0;
  ayther_subscription_state_v1 st{};
  st.struct_size = sizeof(st);
  if (get_subscriptions(&st, sizeof(st)) != AYTHER_STATUS_OK)
    return 0;
  const std::uint32_t want = kEngineSubscriptions & st.supported_mask;
  return set_subscriptions(want) == AYTHER_STATUS_OK ? want : 0;
}

ObservationSource::SubscriptionObservation
ObservationSource::read_subscriptions() const {
  SubscriptionObservation result;
  if (!has_ayther_v1() || !provides(ObservationFn::get_subscriptions) ||
      !has_capability(AYTHER_CAP_SUBSCRIPTIONS_V1))
    return result;
  result.attempted = true;
  result.state.struct_size = sizeof(result.state);
  result.status = get_subscriptions(&result.state, sizeof(result.state));
  return result;
}

// ---------------------------------------------------------------------------
// Typed audio events
// ---------------------------------------------------------------------------
ObservationSource::AudioPollObservation
ObservationSource::read_audio_poll(ayther_audio_event_v1 *out,
                                   std::uint32_t capacity) const {
  using Outcome = AudioPollObservation::Outcome;
  AudioPollObservation result;
  if (!has_ayther_v1())
    return result;
  if (out == nullptr || capacity == 0) {
    result.outcome = Outcome::invalid_output;
    return result;
  }
  if (!provides(ObservationFn::poll_audio_events) ||
      !has_capability(AYTHER_CAP_AUDIO_PROBE_V1)) {
    result.outcome = Outcome::unsupported;
    return result;
  }
  if (provides(ObservationFn::get_audio_transport_stats)) {
    result.stats_attempted = true;
    result.stats.struct_size = sizeof(result.stats);
    result.stats_status =
        get_audio_transport_stats(&result.stats, sizeof(result.stats));
    if (result.stats_status == AYTHER_STATUS_OK &&
        result.stats.event_size != 0 &&
        result.stats.event_size != sizeof(ayther_audio_event_v1)) {
      result.outcome = Outcome::layout_mismatch;
      return result;
    }
  }
  result.poll_attempted = true;
  std::uint32_t count = 0;
  result.poll_status = poll_audio_events(out, capacity, &count);
  if (result.poll_status != AYTHER_STATUS_OK) {
    result.outcome = Outcome::poll_error;
    return result;
  }
  result.outcome = Outcome::polled;
  result.count = count;
  return result;
}

std::uint32_t
ObservationSource::poll_audio_events_v1(ayther_audio_event_v1 *out,
                                        std::uint32_t max) const {
  return poll_audio_events_observed_v1(out, max, nullptr);
}

std::uint32_t ObservationSource::poll_audio_events_observed_v1(
    ayther_audio_event_v1 *out, std::uint32_t max,
    AudioPollObservation *observation) const {
  const auto result = read_audio_poll(out, max);
  if (observation != nullptr)
    *observation = result;
  if (result.outcome == AudioPollObservation::Outcome::layout_mismatch) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      ayther::log::write(
          ayther::log::Severity::Warning, "emulator", "core_dice_event_size",
          "el core dice event_size=%u y este Engine "
          "conoce %zu — no se leen eventos de audio",
          result.stats.event_size, sizeof(ayther_audio_event_v1));
    }
  }
  return result.count;
}

std::uint32_t ObservationSource::audio_events_dropped() const {
  if (!has_ayther_v1() || !provides(ObservationFn::get_audio_transport_stats))
    return 0;
  ayther_audio_transport_stats_v1 st{};
  st.struct_size = sizeof(st);
  if (get_audio_transport_stats(&st, sizeof(st)) != AYTHER_STATUS_OK)
    return 0;
  return st.dropped_events;
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------
ObservationSource::AytherWriteResult
ObservationSource::write_control_v1(std::uint32_t region, const void *data,
                                    std::uint32_t bytes,
                                    std::uint64_t expected_generation) const {
  AytherWriteResult r;
  if (!has_ayther_v1() || !data || !bytes)
    return r;
  r.status = write_control(region, 0, data, bytes, expected_generation,
                           &r.new_generation);
  return r;
}

// ---------------------------------------------------------------------------
// InterfaceObservationSource: the primitives over a borrowed descriptor.
// ---------------------------------------------------------------------------
void InterfaceObservationSource::bind_interface(
    const ayther_interface_v1 *api) {
  api_ = api;
  info_ = ObservationInfo{};
  if (!api)
    return;
  info_.abi_version = api->abi_version;
  info_.capabilities = api->capabilities;
  if (api->build_id && api->build_id_size)
    info_.build_id = std::string_view(api->build_id, api->build_id_size);
  info_.region_info_size = api->region_info_size;
  info_.frame_snapshot_size = api->frame_snapshot_size;
  info_.sprite_size = api->sprite_size;
  info_.audio_write_size = api->audio_write_size;
  info_.audio_event_size = api->audio_event_size;
  info_.audio_transport_stats_size = api->audio_transport_stats_size;
  info_.subscription_state_size = api->subscription_state_size;
  info_.frame_delta_size = api->frame_delta_size;
  if (AYTHER_IFACE_HAS(api, recompose_stats_size))
    info_.recompose_stats_size = api->recompose_stats_size;
}

bool InterfaceObservationSource::provides(ObservationFn fn) const {
  if (!api_)
    return false;
    // The 1.0 table is always there: a null pointer is "not provided", as the
    // callers always checked. Fields added later (1.1+) are only readable if
    // `struct_size` reaches them (AYTHER_IFACE_HAS).
#define AYTHER_PROVIDES_V10(field) (api_->field != nullptr)
#define AYTHER_PROVIDES_LATER(field)                                           \
  (AYTHER_IFACE_HAS(api_, field) && api_->field != nullptr)
  switch (fn) {
  case ObservationFn::query_region:
    return AYTHER_PROVIDES_V10(query_region);
  case ObservationFn::read_region:
    return AYTHER_PROVIDES_V10(read_region);
  case ObservationFn::write_control:
    return AYTHER_PROVIDES_V10(write_control);
  case ObservationFn::capture_snapshot:
    return AYTHER_PROVIDES_V10(capture_snapshot);
  case ObservationFn::recompose_frame:
    return AYTHER_PROVIDES_V10(recompose_frame);
  case ObservationFn::poll_audio_events:
    return AYTHER_PROVIDES_V10(poll_audio_events);
  case ObservationFn::get_audio_transport_stats:
    return AYTHER_PROVIDES_V10(get_audio_transport_stats);
  case ObservationFn::get_subscriptions:
    return AYTHER_PROVIDES_V10(get_subscriptions);
  case ObservationFn::set_subscriptions:
    return AYTHER_PROVIDES_V10(set_subscriptions);
  case ObservationFn::poll_frame_delta:
    return AYTHER_PROVIDES_V10(poll_frame_delta);
  case ObservationFn::get_recompose_stats:
    return AYTHER_PROVIDES_LATER(get_recompose_stats);
  case ObservationFn::recompose_multilayer:
    return AYTHER_PROVIDES_LATER(recompose_multilayer);
  case ObservationFn::frame_delta_since:
    return AYTHER_PROVIDES_LATER(frame_delta_since);
  }
#undef AYTHER_PROVIDES_V10
#undef AYTHER_PROVIDES_LATER
  return false;
}

std::int32_t
InterfaceObservationSource::query_region(std::uint32_t region_id,
                                         ayther_region_info_v1 *out,
                                         std::uint32_t out_size) const {
  if (!provides(ObservationFn::query_region))
    return AYTHER_STATUS_UNSUPPORTED;
  return api_->query_region(region_id, out, out_size);
}

std::int32_t InterfaceObservationSource::read_region(
    std::uint32_t region_id, std::uint32_t offset, void *out,
    std::uint32_t byte_count, std::uint64_t expected_generation,
    std::uint64_t *actual_generation) const {
  if (!provides(ObservationFn::read_region))
    return AYTHER_STATUS_UNSUPPORTED;
  return api_->read_region(region_id, offset, out, byte_count,
                           expected_generation, actual_generation);
}

std::int32_t InterfaceObservationSource::write_control(
    std::uint32_t region_id, std::uint32_t offset, const void *data,
    std::uint32_t byte_count, std::uint64_t expected_generation,
    std::uint64_t *new_generation) const {
  if (!provides(ObservationFn::write_control))
    return AYTHER_STATUS_UNSUPPORTED;
  return api_->write_control(region_id, offset, data, byte_count,
                             expected_generation, new_generation);
}

std::int32_t
InterfaceObservationSource::capture_snapshot(ayther_frame_snapshot_v1 *out,
                                             std::uint32_t out_size) const {
  if (!provides(ObservationFn::capture_snapshot))
    return AYTHER_STATUS_UNSUPPORTED;
  return api_->capture_snapshot(out, out_size);
}

std::int32_t InterfaceObservationSource::recompose_frame(
    std::uint16_t *out_pixels, std::uint32_t pixel_capacity,
    std::uint32_t flags, std::uint32_t *out_width,
    std::uint32_t *out_height) const {
  if (!provides(ObservationFn::recompose_frame))
    return AYTHER_STATUS_UNSUPPORTED;
  return api_->recompose_frame(out_pixels, pixel_capacity, flags, out_width,
                               out_height);
}

std::int32_t InterfaceObservationSource::poll_audio_events(
    ayther_audio_event_v1 *out, std::uint32_t event_capacity,
    std::uint32_t *out_event_count) const {
  if (!provides(ObservationFn::poll_audio_events))
    return AYTHER_STATUS_UNSUPPORTED;
  return api_->poll_audio_events(out, event_capacity, out_event_count);
}

std::int32_t InterfaceObservationSource::get_audio_transport_stats(
    ayther_audio_transport_stats_v1 *out, std::uint32_t out_size) const {
  if (!provides(ObservationFn::get_audio_transport_stats))
    return AYTHER_STATUS_UNSUPPORTED;
  return api_->get_audio_transport_stats(out, out_size);
}

std::int32_t
InterfaceObservationSource::get_subscriptions(ayther_subscription_state_v1 *out,
                                              std::uint32_t out_size) const {
  if (!provides(ObservationFn::get_subscriptions))
    return AYTHER_STATUS_UNSUPPORTED;
  return api_->get_subscriptions(out, out_size);
}

std::int32_t InterfaceObservationSource::set_subscriptions(
    std::uint32_t requested_mask) const {
  if (!provides(ObservationFn::set_subscriptions))
    return AYTHER_STATUS_UNSUPPORTED;
  return api_->set_subscriptions(requested_mask);
}

std::int32_t
InterfaceObservationSource::poll_frame_delta(ayther_frame_delta_v1 *out,
                                             std::uint32_t out_size) const {
  if (!provides(ObservationFn::poll_frame_delta))
    return AYTHER_STATUS_UNSUPPORTED;
  return api_->poll_frame_delta(out, out_size);
}

std::int32_t
InterfaceObservationSource::get_recompose_stats(ayther_recompose_stats_v1 *out,
                                                std::uint32_t out_size) const {
  if (!provides(ObservationFn::get_recompose_stats))
    return AYTHER_STATUS_UNSUPPORTED;
  return api_->get_recompose_stats(out, out_size);
}

std::int32_t InterfaceObservationSource::recompose_multilayer(
    std::uint16_t *out_bg_a, std::uint16_t *out_bg_b, std::uint16_t *out_window,
    std::uint16_t *out_sprites, std::uint16_t *out_composite,
    std::uint32_t pixel_capacity, std::uint32_t flags, std::uint32_t *out_width,
    std::uint32_t *out_height) const {
  if (!provides(ObservationFn::recompose_multilayer))
    return AYTHER_STATUS_UNSUPPORTED;
  return api_->recompose_multilayer(out_bg_a, out_bg_b, out_window, out_sprites,
                                    out_composite, pixel_capacity, flags,
                                    out_width, out_height);
}

std::int32_t
InterfaceObservationSource::frame_delta_since(std::uint64_t generation_from,
                                              ayther_frame_delta_v1 *out,
                                              std::uint32_t out_size) const {
  if (!provides(ObservationFn::frame_delta_since))
    return AYTHER_STATUS_UNSUPPORTED;
  return api_->frame_delta_since(generation_from, out, out_size);
}

} // namespace ayther
