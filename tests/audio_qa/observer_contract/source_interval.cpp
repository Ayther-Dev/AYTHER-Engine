#include "audio_source_observation.h"

#include <array>
#include <iostream>

namespace qa = ayther::audio_qa;
namespace obs = ayther::engine::audio_observation;

namespace {
template <typename T>
const T *field(const obs::FactView &fact, std::string_view name) noexcept {
  for (const auto &item : fact.fields)
    if (item.name == name)
      return std::get_if<T>(&item.value);
  return nullptr;
}
struct State {
  obs::FactId id;
  std::string_view state;
  std::string_view reason;
  std::optional<std::uint64_t> received;
  std::optional<std::uint64_t> lost;
  std::optional<std::uint64_t> end;
};
struct Sink {
  State raw;
  State typed;
  std::uint64_t submitted = 0;
  std::uint64_t not_submitted = 0;
  bool detector_active = false;
  bool valid = true;
  static void receive(void *context, const obs::FactView &fact) noexcept {
    auto &self = *static_cast<Sink *>(context);
    if (fact.kind == "audio_detector_interval") {
      const auto *count = field<std::uint64_t>(fact, "submitted_inputs");
      const auto *active = field<bool>(fact, "call_completed");
      const auto *skipped = field<std::uint64_t>(fact, "not_submitted_inputs");
      self.valid = self.valid && count && active && skipped;
      if (count)
        self.submitted = *count;
      if (active)
        self.detector_active = *active;
      if (skipped)
        self.not_submitted = *skipped;
      return;
    }
    if (fact.kind != "audio_source_interval")
      return;
    const auto *source = field<std::string_view>(fact, "source");
    const auto *state = field<std::string_view>(fact, "state");
    const auto *reason = field<std::string_view>(fact, "reason");
    self.valid = self.valid && source && state && reason;
    if (!source || !state || !reason)
      return;
    auto &target = *source == "chip_writes" ? self.raw : self.typed;
    target = {};
    target.id = fact.id;
    target.state = *state;
    target.reason = *reason;
    if (const auto *value = field<std::uint64_t>(fact, "received"))
      target.received = *value;
    if (const auto *value = field<std::uint64_t>(fact, "lost_in_interval"))
      target.lost = *value;
    if (const auto *value = field<std::uint64_t>(fact, "end_frame"))
      target.end = *value;
  }
};
qa::SourceInterval ready(std::uint64_t frame) {
  qa::SourceInterval data;
  data.frame = frame;
  data.subscriptions.attempted = true;
  data.subscriptions.status = AYTHER_STATUS_OK;
  data.subscriptions.state.struct_size = sizeof(ayther_subscription_state_v1);
  data.subscriptions.state.state_version = 1;
  data.subscriptions.state.supported_mask =
      AYTHER_SUB_AUDIO_WRITES | AYTHER_SUB_AUDIO_EVENTS;
  data.subscriptions.state.active_mask =
      data.subscriptions.state.supported_mask;
  data.snapshot_available = true;
  data.snapshot.struct_size = sizeof(data.snapshot);
  data.snapshot.snapshot_version = 1;
  data.raw_abi = true;
  data.raw_read = RetroRunner::AytherReadResult{};
  data.raw_read->status = AYTHER_STATUS_OK;
  data.poll.outcome = RetroRunner::AudioPollObservation::Outcome::polled;
  data.poll.poll_attempted = true;
  data.poll.poll_status = AYTHER_STATUS_OK;
  data.poll.stats_attempted = true;
  data.poll.stats_status = AYTHER_STATUS_OK;
  data.poll.stats.struct_size = sizeof(data.poll.stats);
  data.poll.stats.transport_version = 1;
  data.poll.stats.event_size = sizeof(ayther_audio_event_v1);
  data.poll.stats.flags = AYTHER_AUDIO_TRANSPORT_OBSERVATION_ACTIVE;
  data.poll_capacity = 4096;
  return data;
}
bool interval_facts() {
  qa::IdentitySource ids;
  qa::SourceTracker tracker;
  Sink sink;
  const obs::Observer observer{&sink, Sink::receive, nullptr};
  auto input = ready(10);
  auto result = tracker.observe(observer, ids, input);
  if (!result.complete || !result.raw || !result.typed || !sink.valid ||
      sink.raw.state != "active" || sink.typed.state != "active" ||
      sink.raw.received != 0 || sink.typed.received != 0 ||
      sink.raw.lost != 0 || sink.typed.lost != 0 || sink.raw.end != 11)
    return false;
  input = ready(11);
  input.subscriptions.state.active_mask = 0;
  result = tracker.observe(observer, ids, input);
  if (!result.complete || sink.raw.state != "disabled" ||
      sink.typed.state != "disabled")
    return false;
  input = ready(12);
  input.poll.outcome = RetroRunner::AudioPollObservation::Outcome::poll_error;
  input.poll.poll_status = AYTHER_STATUS_BUSY;
  input.raw_abi = false;
  if (!input.raw_read)
    return false;
  input.raw_read->status = AYTHER_STATUS_STALE_GENERATION;
  input.snapshot.audio_write_count = 2;
  input.raw_received = 2;
  result = tracker.observe(observer, ids, input);
  if (!result.complete || sink.raw.state != "interrupted" ||
      sink.typed.state != "interrupted" || sink.typed.received)
    return false;
  input = ready(13);
  input.poll.stats.dropped_events = 2;
  result = tracker.observe(observer, ids, input);
  if (!result.complete || sink.typed.state != "interrupted" ||
      sink.typed.lost != 2)
    return false;
  input = ready(14);
  input.poll.stats.dropped_events = 2;
  input.poll.count = 4;
  input.pcm_prepared = 1;
  input.filtered_non_pcm = 1;
  input.filtered_schema = 1;
  input.filtered_type = 1;
  result = tracker.observe(observer, ids, input);
  if (!result.complete || sink.typed.received != 4 || sink.typed.lost != 0 ||
      sink.typed.state != "active")
    return false;
  const auto detector = qa::observe_detector_interval(
      observer, ids,
      {14, qa::DetectorPath::live, result, {}, true, false, 3, 1});
  if (!detector || !sink.valid || !sink.detector_active || sink.submitted != 4)
    return false;
  input = ready(15);
  input.poll.stats.struct_size = 0;
  result = tracker.observe(observer, ids, input);
  return result.complete && sink.typed.state == "unknown" && !sink.typed.lost;
}

struct Fixture {
  qa::IdentitySource ids;
  qa::SourceTracker tracker;
  Sink sink;
  qa::SourceObservation observe(const qa::SourceInterval &input) {
    return tracker.observe({&sink, Sink::receive, nullptr}, ids, input);
  }
};

bool raw_boundaries() {
  Fixture test;
  auto input = ready(0);
  input.raw_read.reset();
  input.raw_abi = false;
  if (!test.observe(input).complete || test.sink.raw.state != "active" ||
      test.sink.raw.received != 0)
    return false;
  input.raw_prepared = 1;
  if (!test.observe(input).complete || test.sink.raw.state != "interrupted" ||
      test.sink.raw.reason != "raw_count_mismatch")
    return false;
  input = ready(1);
  if (!input.raw_read)
    return false;
  input.raw_read->status = AYTHER_STATUS_STALE_GENERATION;
  if (!test.observe(input).complete || test.sink.raw.state != "interrupted")
    return false;
  input = ready(2);
  input.snapshot.overflow_flags = AYTHER_OVERFLOW_AUDIO_WRITES;
  if (!test.observe(input).complete ||
      test.sink.raw.reason != "raw_write_overflow" || test.sink.raw.lost)
    return false;
  input = ready(3);
  input.raw_data_available = false;
  if (!test.observe(input).complete ||
      test.sink.raw.reason != "raw_data_unavailable" || test.sink.raw.received)
    return false;
  input = ready(4);
  input.snapshot.snapshot_version = 2;
  if (!test.observe(input).complete || test.sink.raw.state != "unknown" ||
      test.sink.raw.lost)
    return false;
  input = ready(5);
  input.snapshot.struct_size = 0;
  if (!test.observe(input).complete || test.sink.raw.state != "unknown")
    return false;
  input = ready(6);
  input.snapshot.audio_write_count = 1;
  input.raw_received = 1;
  input.raw_read.reset();
  if (!test.observe(input).complete ||
      test.sink.raw.reason != "abi_read_unavailable")
    return false;
  input = ready(7);
  input.snapshot.audio_write_count = 1;
  input.raw_received = 1;
  if (!test.observe(input).complete ||
      test.sink.raw.reason != "raw_count_mismatch")
    return false;
  input.raw_prepared = 1;
  return test.observe(input).complete && test.sink.raw.state == "active" &&
         test.sink.valid;
}

bool metadata_boundaries() {
  Fixture test;
  auto input = ready(0);
  input.subscriptions.attempted = false;
  if (!test.observe(input).complete || test.sink.raw.state != "unknown" ||
      test.sink.typed.state != "unknown")
    return false;
  input = ready(1);
  input.subscriptions.state.state_version = 2;
  if (!test.observe(input).complete || test.sink.raw.state != "unknown")
    return false;
  input = ready(2);
  input.subscriptions.state.supported_mask = 0;
  if (!test.observe(input).complete || test.sink.raw.state != "unavailable" ||
      test.sink.typed.state != "unavailable")
    return false;
  for (const auto outcome :
       {RetroRunner::AudioPollObservation::Outcome::no_api,
        RetroRunner::AudioPollObservation::Outcome::unsupported}) {
    input = ready(++input.frame);
    input.poll.outcome = outcome;
    if (!test.observe(input).complete ||
        test.sink.typed.state != "unavailable" || test.sink.typed.received)
      return false;
  }
  input = ready(5);
  input.poll.poll_status = AYTHER_STATUS_NOT_SUBSCRIBED;
  input.poll.outcome = RetroRunner::AudioPollObservation::Outcome::poll_error;
  if (!test.observe(input).complete || test.sink.typed.state != "disabled" ||
      test.sink.typed.received)
    return false;
  input = ready(6);
  input.poll.stats.flags = 0;
  if (!test.observe(input).complete ||
      test.sink.typed.reason != "transport_observation_inactive")
    return false;
  for (unsigned variant = 0; variant < 3; ++variant) {
    input = ready(7 + variant);
    if (variant == 0)
      input.poll.stats.transport_version = 2;
    else if (variant == 1)
      input.poll.stats.event_size = 0;
    else
      input.poll.stats_attempted = false;
    if (!test.observe(input).complete || test.sink.typed.state != "unknown" ||
        test.sink.typed.lost)
      return false;
  }
  input = ready(10);
  input.poll.count = input.poll_capacity + 1;
  input.pcm_prepared = input.poll.count;
  if (!test.observe(input).complete ||
      test.sink.typed.reason != "typed_count_mismatch")
    return false;
  input = ready(11);
  input.pcm_prepared = UINT32_MAX;
  input.filtered_schema = 1;
  if (!test.observe(input).complete ||
      test.sink.typed.reason != "typed_count_mismatch")
    return false;
  return test.observe(ready(12)).complete &&
         test.sink.typed.state == "active" && test.sink.valid;
}

bool loss_transitions() {
  Fixture test;
  auto input = ready(0);
  input.poll.stats.dropped_events = 5;
  if (!test.observe(input).complete ||
      test.sink.typed.reason != "loss_baseline_unavailable" ||
      test.sink.typed.lost)
    return false;
  input.frame = 1;
  input.poll.stats.dropped_events = 7;
  if (!test.observe(input).complete || test.sink.typed.lost != 2)
    return false;
  input.frame = 2;
  input.poll.stats.dropped_events = UINT32_MAX;
  if (!test.observe(input).complete ||
      test.sink.typed.reason != "loss_counter_saturated" ||
      test.sink.typed.lost)
    return false;
  input.frame = 3;
  input.poll.stats.dropped_events = 0;
  if (!test.observe(input).complete || test.sink.typed.lost)
    return false;
  input.frame = 4;
  if (!test.observe(input).complete || test.sink.typed.state != "active" ||
      test.sink.typed.lost != 0)
    return false;
  input.frame = 5;
  input.poll.stats.dropped_events = 9;
  if (!test.observe(input).complete || test.sink.typed.lost != 9)
    return false;
  input.frame = 6;
  input.poll.stats.dropped_events = 2;
  if (!test.observe(input).complete ||
      test.sink.typed.reason != "loss_counter_reset" || test.sink.typed.lost)
    return false;
  input.frame = 8;
  if (!test.observe(input).complete ||
      test.sink.typed.reason != "noncontiguous_interval" ||
      test.sink.typed.lost)
    return false;
  const auto previous_id = test.sink.typed.id;
  if (!test.observe(input).complete || test.sink.typed.lost ||
      test.sink.typed.id.sequence <= previous_id.sequence)
    return false;
  input.frame = 9;
  if (!test.observe(input).complete || test.sink.typed.lost != 0)
    return false;
  input.frame = 10;
  input.poll.stats_status = AYTHER_STATUS_BUSY;
  if (!test.observe(input).complete || test.sink.typed.lost)
    return false;
  input.frame = 11;
  input.poll.stats_status = AYTHER_STATUS_OK;
  return test.observe(input).complete && !test.sink.typed.lost &&
         test.sink.typed.reason == "loss_baseline_unavailable" &&
         test.sink.valid;
}

bool emission_limits() {
  Fixture test;
  auto input = ready(0);
  const auto off = test.tracker.observe({}, test.ids, input);
  if (off.raw || off.typed || !off.complete)
    return false;
  input.frame = 1;
  input.poll.stats.dropped_events = 1;
  auto result = test.observe(input);
  if (!result.raw || result.raw->sequence != 1 || test.sink.typed.lost)
    return false;
  input = ready(UINT64_MAX);
  result = test.observe(input);
  if (result.complete || !result.raw || !result.typed || test.sink.raw.end ||
      test.sink.typed.end)
    return false;
  const obs::Observer observer{&test.sink, Sink::receive, nullptr};
  const qa::DetectorInterval interval{
      UINT64_MAX, qa::DetectorPath::live, result, {}, false, false, UINT32_MAX,
      UINT32_MAX};
  if (qa::observe_detector_interval({}, test.ids, interval))
    return false;
  const auto detector =
      qa::observe_detector_interval(observer, test.ids, interval);
  return detector && test.sink.valid && !test.sink.detector_active &&
         test.sink.submitted == 0 &&
         test.sink.not_submitted == 2ULL * UINT32_MAX;
}
} // namespace

int main() try {
  if (!interval_facts()) {
    std::cerr << "source interval facts failed\n";
    return 1;
  }
  if (!raw_boundaries() || !metadata_boundaries() || !loss_transitions() ||
      !emission_limits()) {
    std::cerr << "source interval boundaries failed\n";
    return 1;
  }
  return 0;
} catch (...) {
  return 2;
}
