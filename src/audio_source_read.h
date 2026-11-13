#pragma once

#include <ayther/observation/observation_source.h>

namespace ayther::audio_qa {

// The audio poll and subscription reads now live in ObservationSource
// (TP-301.1), so every provider shares them. These names stay for the audio
// QA code and its contract tests; the descriptor overloads wrap a borrowed
// `ayther_interface_v1` (a test's fake table, or a core's).

using AudioPollObservation = ObservationSource::AudioPollObservation;
using SubscriptionObservation = ObservationSource::SubscriptionObservation;

// This is the existing consume-on-poll policy with its branch results retained.
// A failed stats query does not block polling, and layout mismatch still does.
// No extra poll, subscription change, or transport reset is performed here.
inline AudioPollObservation read_audio_poll(const ObservationSource &source,
                                            ayther_audio_event_v1 *out,
                                            uint32_t capacity) {
  return source.read_audio_poll(out, capacity);
}
inline AudioPollObservation read_audio_poll(const ayther_interface_v1 *api,
                                            ayther_audio_event_v1 *out,
                                            uint32_t capacity) {
  return read_audio_poll(InterfaceObservationSource(api), out, capacity);
}

// Read-only metadata for QA. Callers must not interpret an unavailable or
// failed query as an active source, and must not apply requested subscriptions
// here.
inline SubscriptionObservation
read_audio_subscriptions(const ObservationSource &source) {
  return source.read_subscriptions();
}
inline SubscriptionObservation
read_audio_subscriptions(const ayther_interface_v1 *api) {
  return read_audio_subscriptions(InterfaceObservationSource(api));
}

} // namespace ayther::audio_qa
