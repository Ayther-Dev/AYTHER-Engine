#pragma once

#include "audio_input_observation.h"

#include <algorithm>

namespace ayther::audio_qa {

enum class DetectorAction {
  created,
  reset,
  process_frame,
  set_pal,
  set_initial_active,
  finish,
  clear_closed
};
enum class DetectorUse {
  runtime_selection,
  live_learning,
  analysis_result,
  inspection
};

struct DetectorOperation {
  std::uint64_t frame = 0;
  DetectorAction action = DetectorAction::process_frame;
  std::optional<std::uint32_t> detector_frame{};
  std::optional<observation::FactId> input{};
  std::optional<std::uint64_t> parameter{};
};

struct DetectorQuery {
  std::uint64_t frame = 0;
  DetectorUse use = DetectorUse::inspection;
  std::uint32_t capacity = 0;
  std::uint32_t returned = 0;
  std::optional<std::uint32_t> total;
};

struct DetectorOperationObservation {
  std::optional<observation::FactId> id;
  bool complete = true;
};
struct DetectorOutputObservation {
  std::optional<observation::FactId> batch;
  bool complete = true;
};
struct DetectorActiveObservation {
  std::optional<observation::FactId> batch;
  bool complete = true;
  // The current Runtime query has capacity 64. Larger views are still emitted,
  // but callers cannot claim all row links were retained in this bounded
  // result.
  std::array<std::optional<observation::FactId>, 64> rows{};
};

namespace detector_detail {
using input_detail::known;
using observation::Unit;

inline std::string_view name(DetectorAction action) noexcept {
  switch (action) {
  case DetectorAction::created:
    return "created";
  case DetectorAction::reset:
    return "reset";
  case DetectorAction::process_frame:
    return "process_frame";
  case DetectorAction::set_pal:
    return "set_pal";
  case DetectorAction::set_initial_active:
    return "set_initial_active";
  case DetectorAction::finish:
    return "finish";
  case DetectorAction::clear_closed:
    return "clear_closed";
  }
  return "unknown";
}
inline std::string_view name(DetectorUse use) noexcept {
  switch (use) {
  case DetectorUse::runtime_selection:
    return "runtime_selection";
  case DetectorUse::live_learning:
    return "live_learning";
  case DetectorUse::analysis_result:
    return "analysis_result";
  case DetectorUse::inspection:
    return "inspection";
  }
  return "unknown";
}
inline observation::FieldView absent(std::string_view field,
                                     bool required) noexcept {
  return {field,
          required ? observation::Availability::unknown
                   : observation::Availability::not_applicable,
          Unit::none, std::monostate{},
          required ? "not_observed" : "operation_has_no_value"};
}
inline bool valid(std::optional<observation::FactId> id) noexcept {
  return id && id->producer != 0 && id->sequence != 0;
}
} // namespace detector_detail

// One tracker per detector, owned by the session thread. Observe completed
// operations and arrays already obtained by production code; never query or
// re-run detection here. A state chain preserves all prior inputs transitively,
// without guessing which individual write opened a channel.
class DetectorTracker final {
public:
  explicit DetectorTracker(DetectorPath path) noexcept : path_(path) {}

  [[nodiscard]] DetectorOperationObservation
  operation(observation::Observer observer, IdentitySource &ids,
            const DetectorOperation &operation) noexcept {
    using namespace detector_detail;
    if (observer.on_fact == nullptr) {
      previous_.reset();
      history_complete_ = false;
      return {};
    }
    const bool fresh = operation.action == DetectorAction::created ||
                       operation.action == DetectorAction::reset;
    const bool process = operation.action == DetectorAction::process_frame;
    const bool parameter =
        operation.action == DetectorAction::set_pal ||
        operation.action == DetectorAction::set_initial_active;
    const bool input_valid = valid(operation.input);
    const bool parameter_valid =
        operation.parameter &&
        *operation.parameter <=
            (operation.action == DetectorAction::set_pal ? 1ULL : 65535ULL);
    const bool complete =
        (fresh || (previous_ && history_complete_)) &&
        (!process || (input_valid && operation.detector_frame)) &&
        (!parameter || parameter_valid);
    std::array<observation::Cause, 2> causes{};
    std::size_t count = 0;
    if (previous_)
      causes[count++] = *previous_;
    if (process && input_valid)
      causes[count++] = *operation.input;
    const std::array fields{
        known("detector", path_name()),
        known("operation", name(operation.action)),
        known("state_history_complete", complete),
        known("fresh_state", fresh),
        operation.detector_frame
            ? known("detector_frame",
                    static_cast<std::uint64_t>(*operation.detector_frame),
                    Unit::emulation_frame)
            : absent("detector_frame", process),
        input_valid ? known("input_batch", *operation.input)
                    : absent("input_batch", process),
        operation.parameter ? known("parameter", *operation.parameter)
                            : absent("parameter", parameter)};
    const auto id =
        emit(observer, ids, operation.frame, "detector_state_operation", fields,
             std::span{causes}.first(count), true);
    previous_ = id;
    history_complete_ = complete && id.has_value();
    return {id, history_complete_};
  }

  [[nodiscard]] DetectorActiveObservation
  active(observation::Observer observer, IdentitySource &ids,
         const DetectorQuery &query,
         std::span<const AytherAudioActive> rows) const noexcept {
    using namespace detector_detail;
    DetectorActiveObservation result;
    if (observer.on_fact == nullptr)
      return result;
    const auto output = batch(observer, ids, query, rows.size(), "active");
    result.batch = output.batch;
    result.complete = output.complete && rows.size() <= result.rows.size();
    if (!result.batch)
      return result;
    const std::array<observation::Cause, 1> causes{*result.batch};
    for (std::size_t index = 0; index < rows.size(); ++index) {
      const auto &row = rows[index];
      const auto fields = common_fields(row, index);
      const auto id = emit(observer, ids, query.frame,
                           "detector_active_channel", fields, causes);
      result.complete = result.complete && id.has_value();
      if (index < result.rows.size())
        result.rows[index] = id;
    }
    return result;
  }

  [[nodiscard]] DetectorOutputObservation
  closed(observation::Observer observer, IdentitySource &ids,
         const DetectorQuery &query,
         std::span<const AytherAudioEvent> rows) const noexcept {
    using namespace detector_detail;
    if (observer.on_fact == nullptr)
      return {};
    auto result = batch(observer, ids, query, rows.size(), "closed");
    if (!result.batch)
      return result;
    const std::array<observation::Cause, 1> causes{*result.batch};
    for (std::size_t index = 0; index < rows.size(); ++index) {
      const auto &row = rows[index];
      const auto common = common_fields(row, index);
      const std::array fields{
          common[0],
          common[1],
          common[2],
          common[3],
          common[4],
          common[5],
          known("start_frame", static_cast<std::uint64_t>(row.start_frame),
                Unit::emulation_frame),
          known("end_frame", static_cast<std::uint64_t>(row.end_frame),
                Unit::emulation_frame),
          known("velocity", static_cast<std::uint64_t>(row.velocity))};
      const auto id = emit(observer, ids, query.frame, "detector_closed_event",
                           fields, causes);
      result.complete = result.complete && id.has_value();
    }
    return result;
  }

private:
  DetectorPath path_;
  std::optional<observation::FactId> previous_;
  bool history_complete_ = false;

  std::string_view path_name() const noexcept {
    return path_ == DetectorPath::live ? "live" : "analysis";
  }
  template <typename Row>
  static std::array<observation::FieldView, 6>
  common_fields(const Row &row, std::size_t index) noexcept {
    using namespace detector_detail;
    return std::array{
        known("source_index", static_cast<std::uint64_t>(index), Unit::count),
        known("signature", row.signature),
        known("instrument", row.instrument),
        known("chip", static_cast<std::uint64_t>(row.chip)),
        known("channel", static_cast<std::uint64_t>(row.channel)),
        known("pitch", static_cast<std::uint64_t>(row.pitch))};
  }
  std::optional<observation::FactId>
  emit(observation::Observer observer, IdentitySource &ids, std::uint64_t frame,
       std::string_view kind, std::span<const observation::FieldView> fields,
       std::span<const observation::Cause> causes,
       bool state_change = false) const noexcept {
    const auto id = ids.next_fact(Producer::detector);
    if (!id)
      return {};
    const std::array order{observation::StateOrder{
        path_ == DetectorPath::live ? "audio_detector_live"
                                    : "audio_detector_analysis",
        id->sequence}};
    observer.observe(observation::FactView{
        *id,
        kind,
        {observation::Availability::known, frame, {}},
        causes,
        state_change ? std::span<const observation::StateOrder>{order}
                     : std::span<const observation::StateOrder>{},
        fields});
    return id;
  }
  DetectorOutputObservation batch(observation::Observer observer,
                                  IdentitySource &ids,
                                  const DetectorQuery &query, std::size_t size,
                                  std::string_view output) const noexcept {
    using namespace detector_detail;
    const bool read_complete =
        size == query.returned && query.returned <= query.capacity &&
        (!query.total ||
         query.returned == (std::min)(*query.total, query.capacity));
    const std::array fields{
        known("detector", path_name()),
        known("output", output),
        known("use", name(query.use)),
        known("returned_count", static_cast<std::uint64_t>(query.returned),
              Unit::count),
        known("observed_count", static_cast<std::uint64_t>(size), Unit::count),
        known("query_capacity", static_cast<std::uint64_t>(query.capacity),
              Unit::count),
        query.total
            ? known("total_count", static_cast<std::uint64_t>(*query.total),
                    Unit::count)
            : absent("total_count", true),
        known("read_complete", read_complete),
        known("state_history_complete", history_complete_)};
    std::array<observation::Cause, 1> causes{};
    if (previous_)
      causes[0] = *previous_;
    const auto id = emit(observer, ids, query.frame, "detector_output_batch",
                         fields, std::span{causes}.first(previous_ ? 1 : 0));
    return {id, id && read_complete && previous_ && history_complete_};
  }
};

static_assert(sizeof(DetectorTracker) <= 64);

} // namespace ayther::audio_qa
