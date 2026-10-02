#include "audio_source_read.h"

#include <array>
#include <iostream>

#if defined(QA_SOURCE_SESSION)
#include "../../../tools/common/synth_rom.h"
#include "audio_source_observation.h"
#include "session/emulation_observer.h"
#endif

namespace qa = ayther::audio_qa;
using Poll = RetroRunner::AudioPollObservation;

namespace {
struct FakeSource {
  unsigned polls = 0;
  unsigned stats_reads = 0;
  unsigned subscription_reads = 0;
  int32_t poll_status = AYTHER_STATUS_OK;
  int32_t stats_status = AYTHER_STATUS_OK;
  int32_t subscription_status = AYTHER_STATUS_OK;
  uint32_t count = 0;
  ayther_audio_transport_stats_v1 stats{
      sizeof(ayther_audio_transport_stats_v1),
      1,
      sizeof(ayther_audio_event_v1),
      4095,
      0,
      0,
      0,
      AYTHER_AUDIO_TRANSPORT_OBSERVATION_ACTIVE};
  ayther_subscription_state_v1 subscriptions{};
};
FakeSource source;
int32_t AYTHER_CALL poll(ayther_audio_event_v1 *, uint32_t, uint32_t *count) {
  ++source.polls;
  *count = source.count;
  return source.poll_status;
}
int32_t AYTHER_CALL stats(ayther_audio_transport_stats_v1 *out, uint32_t) {
  ++source.stats_reads;
  *out = source.stats;
  return source.stats_status;
}
int32_t AYTHER_CALL subscriptions(ayther_subscription_state_v1 *out, uint32_t) {
  ++source.subscription_reads;
  *out = source.subscriptions;
  return source.subscription_status;
}
ayther_interface_v1 api() {
  ayther_interface_v1 result{};
  result.capabilities = AYTHER_CAP_AUDIO_PROBE_V1 | AYTHER_CAP_SUBSCRIPTIONS_V1;
  result.poll_audio_events = poll;
  result.get_audio_transport_stats = stats;
  result.get_subscriptions = subscriptions;
  return result;
}
bool poll_results() {
  source = {};
  auto core = api();
  std::array<ayther_audio_event_v1, 4> events{};
  auto result = qa::read_audio_poll(&core, events.data(), 4);
  if (result.outcome != Poll::Outcome::polled || result.count != 0 ||
      !result.poll_attempted || !result.stats_attempted || source.polls != 1 ||
      source.stats_reads != 1)
    return false;
  source.count = 3;
  source.stats.dropped_events = UINT32_MAX;
  result = qa::read_audio_poll(&core, events.data(), 4);
  if (result.count != 3 || result.stats.dropped_events != UINT32_MAX ||
      source.polls != 2)
    return false;
  source.poll_status = AYTHER_STATUS_NOT_SUBSCRIBED;
  result = qa::read_audio_poll(&core, events.data(), 4);
  if (result.outcome != Poll::Outcome::poll_error ||
      result.poll_status != AYTHER_STATUS_NOT_SUBSCRIBED || result.count != 0 ||
      source.polls != 3)
    return false;
  source.poll_status = AYTHER_STATUS_BUSY;
  result = qa::read_audio_poll(&core, events.data(), 4);
  if (result.poll_status != AYTHER_STATUS_BUSY || result.count != 0 ||
      source.polls != 4)
    return false;
  source.stats.event_size = 1;
  result = qa::read_audio_poll(&core, events.data(), 4);
  if (result.outcome != Poll::Outcome::layout_mismatch ||
      result.poll_attempted || source.polls != 4)
    return false;
  source.stats_status = AYTHER_STATUS_BUSY;
  source.poll_status = AYTHER_STATUS_OK;
  result = qa::read_audio_poll(&core, events.data(), 4);
  if (result.outcome != Poll::Outcome::polled || result.count != 3 ||
      result.stats_status != AYTHER_STATUS_BUSY || source.polls != 5)
    return false;
  core.capabilities = 0;
  result = qa::read_audio_poll(&core, events.data(), 4);
  if (result.outcome != Poll::Outcome::unsupported || result.poll_attempted ||
      result.stats_attempted || source.polls != 5)
    return false;
  result = qa::read_audio_poll(nullptr, events.data(), 4);
  if (result.outcome != Poll::Outcome::no_api || result.count != 0)
    return false;
  core = api();
  result = qa::read_audio_poll(&core, nullptr, 4);
  if (result.outcome != Poll::Outcome::invalid_output || source.polls != 5)
    return false;
  core.get_audio_transport_stats = nullptr;
  result = qa::read_audio_poll(&core, events.data(), 4);
  return result.outcome == Poll::Outcome::polled && !result.stats_attempted &&
         result.count == 3 && source.polls == 6;
}
bool subscription_results() {
  source = {};
  auto core = api();
  source.subscriptions.struct_size = sizeof(source.subscriptions);
  source.subscriptions.state_version = 1;
  source.subscriptions.supported_mask =
      AYTHER_SUB_AUDIO_EVENTS | AYTHER_SUB_AUDIO_WRITES;
  source.subscriptions.active_mask = AYTHER_SUB_AUDIO_WRITES;
  source.subscriptions.requested_mask =
      AYTHER_SUB_AUDIO_EVENTS | AYTHER_SUB_AUDIO_WRITES;
  source.subscriptions.activation_frame = 9;
  auto result = qa::read_audio_subscriptions(&core);
  if (!result.attempted || result.status != AYTHER_STATUS_OK ||
      result.state.active_mask != AYTHER_SUB_AUDIO_WRITES ||
      result.state.activation_frame != 9 || source.subscription_reads != 1 ||
      source.polls != 0)
    return false;
  source.subscription_status = AYTHER_STATUS_BUSY;
  result = qa::read_audio_subscriptions(&core);
  if (result.status != AYTHER_STATUS_BUSY || source.subscription_reads != 2)
    return false;
  core.capabilities = 0;
  result = qa::read_audio_subscriptions(&core);
  return !result.attempted && result.status == AYTHER_STATUS_UNSUPPORTED &&
         source.subscription_reads == 2;
}

#if defined(QA_SOURCE_SESSION)
struct RawIntervalSink {
  std::string_view state;
  std::uint64_t received = UINT64_MAX;
  std::uint64_t sequence = 0;
  static void receive(void *context,
                      const qa::observation::FactView &fact) noexcept {
    auto &self = *static_cast<RawIntervalSink *>(context);
    bool raw = false;
    for (const auto &item : fact.fields)
      if (item.name == "source")
        if (const auto *value = std::get_if<std::string_view>(&item.value))
          raw = *value == "chip_writes";
    if (!raw)
      return;
    self.sequence = fact.id.sequence;
    for (const auto &item : fact.fields) {
      if (item.name == "state")
        if (const auto *value = std::get_if<std::string_view>(&item.value))
          self.state = *value;
      if (item.name == "received")
        if (const auto *value = std::get_if<std::uint64_t>(&item.value))
          self.received = *value;
    }
  }
};
struct ActualIntervals {
  qa::SourceTracker tracker;
  qa::IdentitySource ids;
  RawIntervalSink sink;
  std::uint64_t frame = 0;
  bool
  observe(RetroRunner &runner, const ayther_frame_snapshot_v1 &snapshot,
          const ayther::session::EmulationObserver::AudioWritesView &writes) {
    qa::SourceInterval input;
    input.frame = frame++;
    input.subscriptions = qa::read_audio_subscriptions(runner.ayther_api());
    input.snapshot_available = true;
    input.snapshot = snapshot;
    input.raw_abi = writes.abi;
    input.raw_read = writes.abi_read;
    input.raw_received = writes.count;
    input.raw_data_available = writes.data != nullptr || writes.count == 0;
    input.raw_prepared = input.raw_data_available ? writes.count : 0;
    const auto previous = sink.sequence;
    sink = {};
    const auto result =
        tracker.observe({&sink, RawIntervalSink::receive, nullptr}, ids, input);
    return result.complete && sink.sequence > previous;
  }
};
bool actual_source_metadata() {
  RetroRunner runner;
  std::array<ayther_audio_event_v1, 2> events{};
  Poll polling;
  if (runner.poll_audio_events_observed_v1(events.data(), 2, &polling) != 0 ||
      polling.outcome != Poll::Outcome::no_api)
    return false;
  if (!runner.init(AYTHER_TEST_CORE_PATH, ayther::synth::canonical_rom_path()))
    return false;
  ayther::session::EmulationObserver observer;
  observer.activate_subscriptions(runner);
  runner.run_frame();
  ayther_frame_snapshot_v1 snapshot{};
  if (!runner.capture_frame_snapshot(snapshot).ok())
    return false;
  const auto active = qa::read_audio_subscriptions(runner.ayther_api());
  if (active.status != AYTHER_STATUS_OK ||
      !(active.state.active_mask & AYTHER_SUB_AUDIO_WRITES))
    return false;
  const auto ready = observer.audio_writes(runner, snapshot, true);
  if (!ready.abi || !ready.abi_read || !ready.abi_read->ok() ||
      ready.count != 16)
    return false;
  ActualIntervals intervals;
  if (!intervals.observe(runner, snapshot, ready) ||
      intervals.sink.state != "active" || intervals.sink.received != 16)
    return false;
  runner.run_frame();
  const auto stale = observer.audio_writes(runner, snapshot, true);
  if (stale.abi || !stale.abi_read ||
      stale.abi_read->status != AYTHER_STATUS_STALE_GENERATION ||
      stale.count != 16)
    return false;
  if (!intervals.observe(runner, snapshot, stale) ||
      intervals.sink.state != "interrupted" || intervals.sink.received != 16)
    return false;
  const auto rejected = observer.audio_writes(runner, snapshot, false);
  if (rejected.count != 0 || !rejected.abi_read ||
      rejected.abi_read->status != AYTHER_STATUS_STALE_GENERATION)
    return false;
  if (runner.ayther_api()->set_subscriptions(0) != AYTHER_STATUS_OK)
    return false;
  runner.run_frame();
  if (!runner.capture_frame_snapshot(snapshot).ok())
    return false;
  const auto inactive = qa::read_audio_subscriptions(runner.ayther_api());
  const auto empty = observer.audio_writes(runner, snapshot, true);
  if (inactive.status != AYTHER_STATUS_OK || inactive.state.active_mask != 0 ||
      empty.count != 0)
    return false;
  if (!intervals.observe(runner, snapshot, empty) ||
      intervals.sink.state != "disabled" || intervals.sink.received != 0)
    return false;
  const auto ordinary_count = runner.poll_audio_events_v1(events.data(), 2);
  const auto observed_count =
      runner.poll_audio_events_observed_v1(events.data(), 2, &polling);
  if (ordinary_count != observed_count || observed_count != 0 ||
      polling.outcome != Poll::Outcome::unsupported || polling.poll_attempted)
    return false;
  observer.activate_subscriptions(runner);
  runner.run_frame();
  if (!runner.capture_frame_snapshot(snapshot).ok())
    return false;
  const auto recovered = observer.audio_writes(runner, snapshot, true);
  return intervals.observe(runner, snapshot, recovered) &&
         intervals.sink.state == "active" && intervals.sink.received == 16;
}
#endif
} // namespace

int main() try {
  if (!poll_results()) {
    std::cerr << "poll result observation failed\n";
    return 1;
  }
  if (!subscription_results()) {
    std::cerr << "subscription observation failed\n";
    return 1;
  }
#if defined(QA_SOURCE_SESSION)
  if (!actual_source_metadata()) {
    std::cerr << "actual source metadata failed\n";
    return 1;
  }
#endif
  return 0;
} catch (...) {
  return 2;
}
