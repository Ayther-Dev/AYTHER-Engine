#include "audio_detector_observation.h"

#include <array>
#include <iostream>

namespace qa = ayther::audio_qa;
namespace obs = ayther::engine::audio_observation;

namespace {
struct Record {
  obs::FactId id;
  std::string_view kind;
  std::array<obs::FactId, 2> causes{};
  std::size_t cause_count = 0;
  std::uint64_t signature = 0;
  std::uint64_t instrument = 0;
  std::uint64_t index = 0;
  std::uint64_t start = 0;
  std::uint64_t end = 0;
  std::uint64_t pitch = 0;
  std::uint64_t velocity = 0;
  std::uint64_t chip = 0;
  std::uint64_t channel = 0;
  std::uint64_t frame = 0;
  std::uint64_t state_sequence = 0;
  bool history_complete = false;
};
struct Sink {
  std::array<Record, 128> records{};
  std::size_t count = 0;
  bool valid = true;
  static void receive(void *context, const obs::FactView &fact) noexcept {
    auto &self = *static_cast<Sink *>(context);
    if (self.count >= self.records.size() || fact.causes.size() > 2) {
      self.valid = false;
      return;
    }
    auto &record = self.records[self.count++];
    record.id = fact.id;
    record.kind = fact.kind;
    record.frame = fact.frame.emulation_frame;
    if (fact.state_orders.size() == 1)
      record.state_sequence = fact.state_orders[0].sequence;
    for (const auto &cause : fact.causes)
      if (const auto *id = std::get_if<obs::FactId>(&cause))
        record.causes[record.cause_count++] = *id;
      else
        self.valid = false;
    for (const auto &item : fact.fields) {
      if (const auto *value = std::get_if<std::uint64_t>(&item.value)) {
        if (item.name == "signature")
          record.signature = *value;
        if (item.name == "instrument")
          record.instrument = *value;
        if (item.name == "source_index")
          record.index = *value;
        if (item.name == "start_frame")
          record.start = *value;
        if (item.name == "end_frame")
          record.end = *value;
        if (item.name == "pitch")
          record.pitch = *value;
        if (item.name == "velocity")
          record.velocity = *value;
        if (item.name == "chip")
          record.chip = *value;
        if (item.name == "channel")
          record.channel = *value;
      }
      if (item.name == "state_history_complete")
        if (const auto *value = std::get_if<bool>(&item.value))
          record.history_complete = *value;
    }
  }
};

bool output_values_and_causes() {
  qa::IdentitySource ids;
  qa::DetectorTracker tracker{qa::DetectorPath::live};
  Sink sink;
  const obs::Observer observer{&sink, Sink::receive, nullptr};
  const auto created =
      tracker.operation(observer, ids, {0, qa::DetectorAction::created});
  const obs::FactId input{3, 17};
  const auto processed = tracker.operation(
      observer, ids,
      {UINT64_MAX, qa::DetectorAction::process_frame, UINT32_MAX, input});
  if (!created.id || !created.complete || !processed.id ||
      !processed.complete || sink.records[1].cause_count != 2 ||
      sink.records[1].causes[0] != *created.id ||
      sink.records[1].causes[1] != input ||
      sink.records[1].frame != UINT64_MAX ||
      sink.records[1].state_sequence != processed.id->sequence)
    return false;
  const std::array<AytherAudioActive, 2> active{
      {{UINT64_MAX, 88, 0, 5, 255, {}}, {UINT64_MAX, 99, 3, 7, 45, {}}}};
  const qa::DetectorQuery query{
      UINT64_MAX, qa::DetectorUse::runtime_selection, 64, 2, {}};
  const auto result = tracker.active(observer, ids, query, active);
  if (!result.complete || !result.batch || !result.rows[0] || !result.rows[1] ||
      result.rows[0] == result.rows[1] || sink.count != 5 ||
      sink.records[2].causes[0] != *processed.id ||
      sink.records[3].causes[0] != *result.batch ||
      sink.records[4].index != 1 || sink.records[4].signature != UINT64_MAX ||
      sink.records[4].instrument != 99 || sink.records[3].pitch != 255 ||
      sink.records[4].chip != 3 || sink.records[4].channel != 7)
    return false;
  const auto finish = tracker.operation(
      observer, ids, {UINT64_MAX, qa::DetectorAction::finish});
  const std::array<AytherAudioEvent, 2> closed{
      {{18, 71, UINT32_MAX, UINT32_MAX, 0, 5, 255, 0},
       {18, 72, 0, 3, 3, 7, 36, 127}}};
  const auto events = tracker.closed(
      observer, ids, {UINT64_MAX, qa::DetectorUse::analysis_result, 2, 2, 2},
      closed);
  return events.complete && events.batch && finish.id && sink.valid &&
         sink.count == 9 && sink.records[5].causes[0] == *processed.id &&
         sink.records[6].causes[0] == *finish.id &&
         sink.records[7].start == UINT32_MAX &&
         sink.records[7].end == UINT32_MAX && sink.records[8].start == 0 &&
         sink.records[8].end == 3 && sink.records[8].velocity == 127;
}

bool absent_history_and_reset() {
  qa::IdentitySource ids;
  qa::DetectorTracker tracker{qa::DetectorPath::analysis};
  Sink sink;
  const obs::Observer observer{&sink, Sink::receive, nullptr};
  const auto unknown = tracker.operation(
      observer, ids,
      {1, qa::DetectorAction::process_frame, 0, obs::FactId{3, 1}});
  if (!unknown.id || unknown.complete || sink.records[0].cause_count != 1 ||
      sink.records[0].history_complete)
    return false;
  const auto reset =
      tracker.operation(observer, ids, {2, qa::DetectorAction::reset});
  if (!reset.complete || !reset.id)
    return false;
  const auto empty = tracker.closed(
      observer, ids, {2, qa::DetectorUse::live_learning, 0, 0, 0}, {});
  if (!empty.complete || !empty.batch)
    return false;
  const auto off =
      tracker.operation({}, ids, {3, qa::DetectorAction::process_frame});
  if (off.id)
    return false;
  const auto resumed = tracker.closed(
      observer, ids, {3, qa::DetectorUse::live_learning, 0, 0, 0}, {});
  if (resumed.complete || !resumed.batch || sink.records[3].cause_count != 0)
    return false;
  const auto missing = tracker.operation(
      observer, ids, {4, qa::DetectorAction::process_frame, 4});
  return !missing.complete && missing.id && sink.valid;
}

bool bounded_rows_and_read_mismatch() {
  qa::IdentitySource ids;
  qa::DetectorTracker tracker{qa::DetectorPath::live};
  Sink sink;
  const obs::Observer observer{&sink, Sink::receive, nullptr};
  if (!tracker.operation(observer, ids, {0, qa::DetectorAction::created})
           .complete)
    return false;
  const std::array<AytherAudioActive, 65> rows{};
  const auto limited = tracker.active(
      observer, ids, {0, qa::DetectorUse::inspection, 65, 65, {}}, rows);
  if (limited.complete || !limited.batch || !limited.rows.back() ||
      sink.count != 67)
    return false;
  sink = {};
  const auto boundary = tracker.active(
      observer, ids, {0, qa::DetectorUse::inspection, 64, 64, {}},
      std::span{rows}.first(64));
  if (!boundary.complete || !boundary.rows.back() || sink.count != 65)
    return false;
  sink = {};
  const auto mismatch =
      tracker.active(observer, ids, {0, qa::DetectorUse::inspection, 1, 2, {}},
                     std::span{rows}.first(1));
  if (mismatch.complete || !mismatch.batch || sink.count != 2)
    return false;
  const auto before = sink.count;
  const auto off =
      tracker.active({}, ids, {0, qa::DetectorUse::inspection, 0, 0, {}}, {});
  return !off.batch && sink.count == before && sink.valid;
}

bool configuration_and_large_closed_view() {
  qa::IdentitySource ids;
  qa::DetectorTracker tracker{qa::DetectorPath::analysis};
  std::size_t count = 0;
  const obs::Observer observer{
      &count,
      [](void *context, const obs::FactView &) noexcept {
        ++*static_cast<std::size_t *>(context);
      },
      nullptr};
  if (!tracker.operation(observer, ids, {0, qa::DetectorAction::created})
           .complete ||
      !tracker
           .operation(observer, ids,
                      {0, qa::DetectorAction::set_pal, {}, {}, 1})
           .complete ||
      !tracker
           .operation(
               observer, ids,
               {0, qa::DetectorAction::set_initial_active, {}, {}, 65535})
           .complete)
    return false;
  const std::array<AytherAudioEvent, 1025> rows{};
  const auto large = tracker.closed(
      observer, ids, {0, qa::DetectorUse::analysis_result, 1025, 1025, 1025},
      rows);
  if (!large.complete || !large.batch || count != 1029)
    return false;
  if (tracker
          .operation(observer, ids, {1, qa::DetectorAction::set_pal, {}, {}, 2})
          .complete)
    return false;
  if (!tracker.operation(observer, ids, {2, qa::DetectorAction::reset})
           .complete)
    return false;
  if (tracker
          .operation(observer, ids, {2, qa::DetectorAction::set_initial_active})
          .complete)
    return false;
  if (!tracker.operation(observer, ids, {3, qa::DetectorAction::reset})
           .complete)
    return false;
  if (tracker
          .operation(
              observer, ids,
              {3, qa::DetectorAction::process_frame, 3, obs::FactId{3, 0}})
          .complete)
    return false;
  return !tracker
              .closed(observer, ids,
                      {3, qa::DetectorUse::analysis_result, 1, 0, 1}, {})
              .complete;
}
} // namespace

int main() try {
  if (!output_values_and_causes() || !absent_history_and_reset() ||
      !bounded_rows_and_read_mismatch() ||
      !configuration_and_large_closed_view()) {
    std::cerr << "detector output observation failed\n";
    return 1;
  }
  return 0;
} catch (...) {
  return 2;
}
