#pragma once

#include "audio_input_observation.h"
#include "audio_source_read.h"

#include <limits>

namespace ayther::audio_qa {

struct SourceInterval {
  std::uint64_t frame = 0;
  SubscriptionObservation subscriptions;
  bool snapshot_available = false;
  ayther_frame_snapshot_v1 snapshot{};
  bool raw_abi = false;
  bool raw_data_available = true;
  std::optional<RetroRunner::AytherReadResult> raw_read;
  std::uint32_t raw_received = 0;
  std::uint32_t raw_prepared = 0;
  RetroRunner::AudioPollObservation poll;
  std::uint32_t poll_capacity = 0;
  std::uint32_t pcm_prepared = 0;
  std::uint32_t filtered_non_pcm = 0;
  std::uint32_t filtered_schema = 0;
  std::uint32_t filtered_type = 0;
};

struct SourceObservation {
  std::optional<observation::FactId> raw;
  std::optional<observation::FactId> typed;
  bool complete = true;
};

namespace source_detail {
using input_detail::known;
using observation::Unit;
inline observation::FieldView unknown(std::string_view name,
                                      std::string_view reason) noexcept {
  return {name, observation::Availability::unknown, Unit::none,
          std::monostate{}, reason};
}
inline bool subscriptions_valid(const SubscriptionObservation &value) noexcept {
  return value.attempted && value.status == AYTHER_STATUS_OK &&
         value.state.struct_size >= sizeof(value.state) &&
         value.state.state_version == 1;
}
inline bool snapshot_valid(const SourceInterval &value) noexcept {
  return value.snapshot_available &&
         value.snapshot.struct_size >= sizeof(value.snapshot) &&
         value.snapshot.snapshot_version == 1;
}
inline bool
stats_valid(const RetroRunner::AudioPollObservation &value) noexcept {
  return value.stats_attempted && value.stats_status == AYTHER_STATUS_OK &&
         value.stats.struct_size >= sizeof(value.stats) &&
         value.stats.transport_version == 1 &&
         value.stats.event_size == sizeof(ayther_audio_event_v1);
}
inline std::string_view
poll_outcome(RetroRunner::AudioPollObservation::Outcome value) noexcept {
  using Outcome = RetroRunner::AudioPollObservation::Outcome;
  switch (value) {
  case Outcome::no_api:
    return "no_api";
  case Outcome::invalid_output:
    return "invalid_output";
  case Outcome::unsupported:
    return "unsupported";
  case Outcome::layout_mismatch:
    return "layout_mismatch";
  case Outcome::poll_error:
    return "poll_error";
  case Outcome::polled:
    return "polled";
  }
  return "unknown";
}
struct Status {
  std::string_view state;
  std::string_view reason;
};
inline std::optional<Status>
subscription_status(const SubscriptionObservation &sub,
                    std::uint32_t mask) noexcept {
  if (!subscriptions_valid(sub))
    return Status{"unknown", "subscription_state_unavailable"};
  if (!(sub.state.supported_mask & mask))
    return Status{"unavailable", "subscription_unsupported"};
  if (!(sub.state.active_mask & mask))
    return Status{"disabled", "not_subscribed"};
  return {};
}
inline Status raw_status(const SourceInterval &input) noexcept {
  if (const auto state =
          subscription_status(input.subscriptions, AYTHER_SUB_AUDIO_WRITES))
    return *state;
  if (!snapshot_valid(input))
    return {"unknown", "snapshot_unavailable"};
  if (input.snapshot.overflow_flags & AYTHER_OVERFLOW_AUDIO_WRITES)
    return {"interrupted", "raw_write_overflow"};
  if (!input.raw_data_available)
    return {"interrupted", "raw_data_unavailable"};
  if (input.raw_read && !input.raw_read->ok())
    return {"interrupted", "abi_read_failed_with_fallback"};
  // Empty vectors can have a null data pointer and bypass read_region. The
  // current snapshot still attests zero with an active subscription and no
  // overflow.
  if (input.snapshot.audio_write_count == 0 && input.raw_received == 0)
    return input.raw_prepared == 0
               ? Status{"active", "empty_snapshot"}
               : Status{"interrupted", "raw_count_mismatch"};
  if (!input.raw_read)
    return {"unknown", "abi_read_unavailable"};
  if (!input.raw_abi)
    return {"interrupted", "abi_read_failed_with_fallback"};
  if (input.raw_received != input.snapshot.audio_write_count ||
      input.raw_prepared != input.raw_received)
    return {"interrupted", "raw_count_mismatch"};
  return {"active", "read_complete"};
}
inline observation::FieldView end_frame(std::uint64_t frame) noexcept {
  return frame == (std::numeric_limits<std::uint64_t>::max)()
             ? unknown("end_frame", "interval_overflow")
             : known("end_frame", frame + 1, Unit::emulation_frame);
}
inline std::optional<observation::FactId>
emit(observation::Observer observer, IdentitySource &ids, std::uint64_t frame,
     std::string_view kind, std::span<const observation::FieldView> fields,
     std::span<const observation::Cause> causes = {}) noexcept {
  const auto id = ids.next_fact(Producer::detector_input);
  if (id)
    observer.observe(
        observation::FactView{*id,
                              kind,
                              {observation::Availability::known, frame, {}},
                              causes,
                              {},
                              fields});
  return id;
}
} // namespace source_detail

class SourceTracker final {
public:
  [[nodiscard]] SourceObservation
  observe(observation::Observer observer, IdentitySource &ids,
          const SourceInterval &input) noexcept {
    using namespace source_detail;
    SourceObservation result;
    if (observer.on_fact == nullptr)
      return result;
    const auto raw = raw_status(input);
    const bool snapshot = snapshot_valid(input);
    const bool subscriptions = subscriptions_valid(input.subscriptions);
    const std::array raw_fields{
        known("source", std::string_view{"chip_writes"}),
        known("state", raw.state),
        known("reason", raw.reason),
        known("begin_frame", input.frame, Unit::emulation_frame),
        end_frame(input.frame),
        known("origin", input.raw_abi ? std::string_view{"abi_snapshot"}
                                      : std::string_view{"legacy_core"}),
        known("reported_count", static_cast<std::uint64_t>(input.raw_received),
              Unit::count),
        known("subscription_query_attempted", input.subscriptions.attempted),
        input.subscriptions.attempted
            ? known("subscription_status",
                    static_cast<std::int64_t>(input.subscriptions.status))
            : unknown("subscription_status",
                      "subscription_query_not_attempted"),
        known("subscription_schema_valid", subscriptions),
        known("snapshot_schema_valid", snapshot),
        input.raw_data_available
            ? known("received", static_cast<std::uint64_t>(input.raw_received),
                    Unit::count)
            : unknown("received", "raw_data_unavailable"),
        known("prepared_inputs", static_cast<std::uint64_t>(input.raw_prepared),
              Unit::count),
        input.raw_read ? known("read_status", static_cast<std::int64_t>(
                                                  input.raw_read->status))
                       : unknown("read_status", "abi_read_not_attempted"),
        snapshot ? known("overflow", (input.snapshot.overflow_flags &
                                      AYTHER_OVERFLOW_AUDIO_WRITES) != 0)
                 : unknown("overflow", "snapshot_unavailable"),
        snapshot &&
                !(input.snapshot.overflow_flags & AYTHER_OVERFLOW_AUDIO_WRITES)
            ? known("lost_in_interval", std::uint64_t{0}, Unit::count)
            : unknown("lost_in_interval", snapshot
                                              ? "overflow_count_not_exposed"
                                              : "snapshot_unavailable"),
        subscriptions ? known("active_subscription_mask",
                              static_cast<std::uint64_t>(
                                  input.subscriptions.state.active_mask))
                      : unknown("active_subscription_mask",
                                "subscription_state_unavailable"),
        snapshot ? known("frame_generation", input.snapshot.frame_generation)
                 : unknown("frame_generation", "snapshot_unavailable")};
    result.raw =
        emit(observer, ids, input.frame, "audio_source_interval", raw_fields);

    const bool stats = stats_valid(input.poll);
    const auto loss = loss_since_previous(input);
    const auto typed = typed_status(input, loss);
    const bool polled = input.poll.outcome ==
                        RetroRunner::AudioPollObservation::Outcome::polled;
    const std::array typed_fields{
        known("source", std::string_view{"typed_audio_events"}),
        known("state", typed.state),
        known("reason", typed.reason),
        known("begin_frame", input.frame, Unit::emulation_frame),
        end_frame(input.frame),
        known("origin", std::string_view{"core_event_queue"}),
        known("poll_attempted", input.poll.poll_attempted),
        known("poll_outcome", poll_outcome(input.poll.outcome)),
        known("subscription_schema_valid", subscriptions),
        known("transport_stats_schema_valid", stats),
        input.subscriptions.attempted
            ? known("subscription_status",
                    static_cast<std::int64_t>(input.subscriptions.status))
            : unknown("subscription_status",
                      "subscription_query_not_attempted"),
        input.poll.stats_attempted &&
                input.poll.stats_status == AYTHER_STATUS_OK
            ? known("reported_transport_version",
                    static_cast<std::uint64_t>(
                        input.poll.stats.transport_version))
            : unknown("reported_transport_version",
                      "transport_stats_unavailable"),
        input.poll.stats_attempted &&
                input.poll.stats_status == AYTHER_STATUS_OK
            ? known("reported_event_size",
                    static_cast<std::uint64_t>(input.poll.stats.event_size),
                    Unit::bytes)
            : unknown("reported_event_size", "transport_stats_unavailable"),
        input.poll.poll_attempted
            ? known("poll_status",
                    static_cast<std::int64_t>(input.poll.poll_status))
            : unknown("poll_status", "poll_not_attempted"),
        input.poll.stats_attempted
            ? known("stats_status",
                    static_cast<std::int64_t>(input.poll.stats_status))
            : unknown("stats_status", "stats_not_attempted"),
        polled ? known("received", static_cast<std::uint64_t>(input.poll.count),
                       Unit::count)
               : unknown("received", "poll_not_successful"),
        known("prepared_inputs", static_cast<std::uint64_t>(input.pcm_prepared),
              Unit::count),
        known("filtered_non_pcm",
              static_cast<std::uint64_t>(input.filtered_non_pcm), Unit::count),
        known("filtered_schema",
              static_cast<std::uint64_t>(input.filtered_schema), Unit::count),
        known("filtered_type", static_cast<std::uint64_t>(input.filtered_type),
              Unit::count),
        stats ? known("transport_pending_before_poll",
                      static_cast<std::uint64_t>(input.poll.stats.pending),
                      Unit::count)
              : unknown("transport_pending_before_poll",
                        "transport_stats_unavailable"),
        stats
            ? known("transport_lost_total_before_poll",
                    static_cast<std::uint64_t>(input.poll.stats.dropped_events),
                    Unit::count)
            : unknown("transport_lost_total_before_poll",
                      "transport_stats_unavailable"),
        loss.count ? known("lost_in_interval",
                           static_cast<std::uint64_t>(*loss.count), Unit::count)
                   : unknown("lost_in_interval", loss.reason),
        subscriptions ? known("active_subscription_mask",
                              static_cast<std::uint64_t>(
                                  input.subscriptions.state.active_mask))
                      : unknown("active_subscription_mask",
                                "subscription_state_unavailable")};
    result.typed =
        emit(observer, ids, input.frame, "audio_source_interval", typed_fields);
    result.complete =
        result.raw && result.typed &&
        input.frame != (std::numeric_limits<std::uint64_t>::max)();
    return result;
  }

private:
  struct Baseline {
    std::uint64_t frame;
    std::uint32_t lost;
  };
  struct Loss {
    std::optional<std::uint32_t> count;
    std::string_view reason;
  };
  std::optional<Baseline> previous_;

  Loss loss_since_previous(const SourceInterval &input) noexcept {
    if (!source_detail::stats_valid(input.poll)) {
      previous_.reset();
      return {{}, "transport_stats_unavailable"};
    }
    const auto current = input.poll.stats.dropped_events;
    const auto previous = previous_;
    previous_ = Baseline{input.frame, current};
    if (current == (std::numeric_limits<std::uint32_t>::max)() ||
        (previous &&
         previous->lost == (std::numeric_limits<std::uint32_t>::max)()))
      return {{}, "loss_counter_saturated"};
    if (previous && current < previous->lost)
      return {{}, "loss_counter_reset"};
    if (!previous)
      return current == 0 ? Loss{0, {}} : Loss{{}, "loss_baseline_unavailable"};
    if (previous->frame == (std::numeric_limits<std::uint64_t>::max)() ||
        input.frame != previous->frame + 1)
      return {{}, "noncontiguous_interval"};
    return {current - previous->lost, {}};
  }

  static source_detail::Status typed_status(const SourceInterval &input,
                                            const Loss &loss) noexcept {
    using Outcome = RetroRunner::AudioPollObservation::Outcome;
    if (input.poll.outcome == Outcome::no_api ||
        input.poll.outcome == Outcome::unsupported)
      return {"unavailable", "typed_source_unavailable"};
    if (input.poll.poll_attempted &&
        input.poll.poll_status == AYTHER_STATUS_NOT_SUBSCRIBED)
      return {"disabled", "not_subscribed"};
    if (const auto state = source_detail::subscription_status(
            input.subscriptions, AYTHER_SUB_AUDIO_EVENTS))
      return *state;
    if (input.poll.outcome != Outcome::polled)
      return {"interrupted", "poll_not_successful"};
    if (!source_detail::stats_valid(input.poll))
      return {"unknown", "transport_stats_unavailable"};
    if (!(input.poll.stats.flags & AYTHER_AUDIO_TRANSPORT_OBSERVATION_ACTIVE))
      return {"disabled", "transport_observation_inactive"};
    const auto accounted = static_cast<std::uint64_t>(input.pcm_prepared) +
                           input.filtered_non_pcm + input.filtered_schema +
                           input.filtered_type;
    if (input.poll.count > input.poll_capacity || accounted != input.poll.count)
      return {"interrupted", "typed_count_mismatch"};
    if (!loss.count)
      return {"unknown", loss.reason};
    if (*loss.count != 0)
      return {"interrupted", "transport_event_loss"};
    return {"active", "poll_complete"};
  }
};

struct DetectorInterval {
  std::uint64_t frame = 0;
  DetectorPath path = DetectorPath::live;
  SourceObservation sources;
  std::optional<observation::FactId> batch;
  bool call_completed = false;
  bool substitution_enabled = false;
  std::uint32_t writes = 0;
  std::uint32_t pcm = 0;
};

[[nodiscard]] inline std::optional<observation::FactId>
observe_detector_interval(observation::Observer observer, IdentitySource &ids,
                          const DetectorInterval &interval) noexcept {
  if (observer.on_fact == nullptr)
    return {};
  using observation::Unit;
  using source_detail::known;
  const std::array fields{
      known("detector", interval.path == DetectorPath::live
                            ? std::string_view{"live"}
                            : std::string_view{"analysis"}),
      known("begin_frame", interval.frame, Unit::emulation_frame),
      source_detail::end_frame(interval.frame),
      known("state", interval.call_completed ? std::string_view{"active"}
                                             : std::string_view{"unavailable"}),
      known("call_completed", interval.call_completed),
      known("substitution_enabled", interval.substitution_enabled),
      known("prepared_inputs",
            static_cast<std::uint64_t>(interval.writes) + interval.pcm,
            Unit::count),
      known("submitted_inputs",
            interval.call_completed
                ? static_cast<std::uint64_t>(interval.writes) + interval.pcm
                : 0,
            Unit::count),
      known("not_submitted_inputs",
            interval.call_completed
                ? 0
                : static_cast<std::uint64_t>(interval.writes) + interval.pcm,
            Unit::count)};
  std::array<observation::Cause, 3> causes{};
  std::size_t count = 0;
  for (const auto &id :
       {interval.sources.raw, interval.sources.typed, interval.batch})
    if (id)
      causes[count++] = *id;
  return source_detail::emit(observer, ids, interval.frame,
                             "audio_detector_interval", fields,
                             std::span{causes}.first(count));
}

} // namespace ayther::audio_qa
