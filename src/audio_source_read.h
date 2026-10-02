#pragma once

#include <ayther/libretro_host/retro_runner.h>

namespace ayther::audio_qa {

// This is the existing consume-on-poll policy with its branch results retained.
// A failed stats query does not block polling, and layout mismatch still does.
// No extra poll, subscription change, or transport reset is performed here.
inline RetroRunner::AudioPollObservation
read_audio_poll(const ayther_interface_v1 *api, ayther_audio_event_v1 *out,
                uint32_t capacity) {
  using Outcome = RetroRunner::AudioPollObservation::Outcome;
  RetroRunner::AudioPollObservation result;
  if (api == nullptr)
    return result;
  if (out == nullptr || capacity == 0) {
    result.outcome = Outcome::invalid_output;
    return result;
  }
  if (api->poll_audio_events == nullptr ||
      !(api->capabilities & AYTHER_CAP_AUDIO_PROBE_V1)) {
    result.outcome = Outcome::unsupported;
    return result;
  }
  if (api->get_audio_transport_stats != nullptr) {
    result.stats_attempted = true;
    result.stats.struct_size = sizeof(result.stats);
    result.stats_status =
        api->get_audio_transport_stats(&result.stats, sizeof(result.stats));
    if (result.stats_status == AYTHER_STATUS_OK &&
        result.stats.event_size != 0 &&
        result.stats.event_size != sizeof(ayther_audio_event_v1)) {
      result.outcome = Outcome::layout_mismatch;
      return result;
    }
  }
  result.poll_attempted = true;
  uint32_t count = 0;
  result.poll_status = api->poll_audio_events(out, capacity, &count);
  if (result.poll_status != AYTHER_STATUS_OK) {
    result.outcome = Outcome::poll_error;
    return result;
  }
  result.outcome = Outcome::polled;
  result.count = count;
  return result;
}

struct SubscriptionObservation {
  bool attempted = false;
  int32_t status = AYTHER_STATUS_UNSUPPORTED;
  ayther_subscription_state_v1 state{};
};

// Read-only metadata for QA. Callers must not interpret an unavailable or
// failed query as an active source, and must not apply requested subscriptions
// here.
inline SubscriptionObservation
read_audio_subscriptions(const ayther_interface_v1 *api) {
  SubscriptionObservation result;
  if (api == nullptr || api->get_subscriptions == nullptr ||
      !(api->capabilities & AYTHER_CAP_SUBSCRIPTIONS_V1))
    return result;
  result.attempted = true;
  result.state.struct_size = sizeof(result.state);
  result.status = api->get_subscriptions(&result.state, sizeof(result.state));
  return result;
}

} // namespace ayther::audio_qa
