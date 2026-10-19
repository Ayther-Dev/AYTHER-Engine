#define NOMINMAX
#include "../support/tessera_fx01_oracle.hpp"
#include <ayther/ayther_session.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
namespace native = ayther::engine;
namespace fx = ayther::test::tessera_fx01;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
void require(bool condition, std::string_view code) {
  if (!condition)
    throw std::runtime_error(std::string(code));
}
void ok(native::NativeResult result) {
  require(result.code == 0,
          "NATIVE_OPERATION_FAILED:" + std::to_string(result.code) + ":" +
              std::to_string(result.phase));
}
fx::Bytes read(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  require(stream.good(), "FIXTURE_OPEN_FAILED");
  fx::Bytes result{std::istreambuf_iterator<char>{stream},
                   std::istreambuf_iterator<char>{}};
  require(result.size() <= 64ULL * 1024 * 1024, "FIXTURE_LIMIT_EXCEEDED");
  return result;
}
void publish(const std::filesystem::path &path, const Json &value) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  require(stream.good(), "REPORT_OPEN_FAILED");
  stream << value.dump(2) << '\n';
  stream.close();
  require(!stream.fail(), "REPORT_WRITE_FAILED");
}
native::NativeBackendInfo backend(std::string_view id) {
  std::vector<native::NativeBackendInfo> catalogue;
  ok(native::NativeEmulation::backends(catalogue));
  const auto found =
      std::ranges::find(catalogue, id, &native::NativeBackendInfo::id);
  require(found != catalogue.end(), "PUBLIC_BACKEND_MISSING");
  require(found->kind == 2 && found->system == 0,
          "PUBLIC_BACKEND_IDENTITY_INVALID");
  return *found;
}
ayther::AytherSession::Config
session_config(const native::NativeConfig &configuration) {
  ayther::AytherSession::Config result;
  result.provider = ayther::EmulationProvider::tessera_native;
  result.native_config = configuration;
  result.core_path = "this-core-must-never-be-opened.dll";
  result.enable_audio = false;
  result.derive_core_pack = false;
  return result;
}
std::unique_ptr<ayther::AytherSession>
create(const native::NativeConfig &configuration) {
  native::NativeResult result;
  auto session =
      ayther::AytherSession::create(session_config(configuration), result);
  require(static_cast<bool>(session),
          "REAL_AYTHER_SESSION_CREATE_FAILED:" + std::to_string(result.code));
  ok(result);
  auto owner = std::move(*session);
  require(owner && owner->native_emulation(), "NATIVE_FACADE_NOT_SELECTED");
  return owner;
}
native::NativeSessionInfo info(native::NativeEmulation &emulation) {
  native::NativeSessionInfo result;
  ok(emulation.info(result));
  return result;
}
fx::Bytes state(native::NativeEmulation &emulation) {
  fx::Bytes result;
  ok(emulation.state(result));
  return result;
}
std::uint64_t read_le(fx::View data, std::size_t offset, unsigned count) {
  require(offset <= data.size() && count <= data.size() - offset,
          "PUBLIC_TLV_TRUNCATED");
  std::uint64_t value{};
  for (unsigned index = 0; index < count; ++index)
    value |= std::uint64_t{data[offset + index]} << (8 * index);
  return value;
}
fx::View tlv_field(fx::View record, std::uint32_t wanted) {
  std::size_t offset{};
  while (offset < record.size()) {
    require(record.size() - offset >= 16, "PUBLIC_TLV_HEADER_TRUNCATED");
    const auto tag = read_le(record, offset, 4),
               bytes = read_le(record, offset + 8, 8);
    require(bytes <= record.size() - offset - 16,
            "PUBLIC_TLV_PAYLOAD_TRUNCATED");
    if (tag == wanted)
      return record.subspan(offset + 16, static_cast<std::size_t>(bytes));
    offset += 16 + static_cast<std::size_t>(bytes);
  }
  throw std::runtime_error("PUBLIC_TLV_TAG_MISSING");
}
fx::Bytes loss_bytes(native::NativeEmulation &emulation) {
  std::uint64_t required{};
  require(emulation.losses({}, required).code == -3 && required <= 4096,
          "PUBLIC_LOSS_SIZE_QUERY_FAILED");
  fx::Bytes value(static_cast<std::size_t>(required));
  ok(emulation.losses(value, required));
  return value;
}
struct EventBatch {
  fx::Bytes bytes;
  std::uint64_t next{}, count{};
};
EventBatch read_events(native::NativeEmulation &emulation,
                       std::uint64_t cursor) {
  std::uint64_t required{}, next = cursor;
  require(emulation.events(cursor, {}, required, next).code == -3 &&
              required <= 4 * 1024 * 1024,
          "PUBLIC_EVENTS_SIZE_QUERY_FAILED");
  EventBatch out;
  out.bytes.resize(static_cast<std::size_t>(required));
  ok(emulation.events(cursor, out.bytes, required, out.next));
  out.count = read_le(tlv_field(out.bytes, 1), 0, 8);
  return out;
}
std::array<native::NativeInputAssignment, 8> assignments(unsigned mask = 0) {
  std::array<native::NativeInputAssignment, 8> result{};
  for (std::uint16_t control = 0; control < 8; ++control)
    result[control] = {0, control,
                       static_cast<std::int32_t>((mask >> control) & 1)};
  return result;
}
native::NativeResult step(native::NativeEmulation &emulation,
                          unsigned mask = 0) {
  const auto current = info(emulation);
  const auto input = assignments(mask);
  return emulation.step(
      {current.frame_id.epoch, current.frame_id.frame + 1, input});
}
fx::Bytes snapshot(native::NativeSnapshot &reference,
                   const native::NativeSessionInfo &current) {
  native::NativeSnapshotInfo capture;
  ok(reference.info(capture));
  require(capture.frame_id.session == current.frame_id.session &&
              capture.frame_id.epoch == current.frame_id.epoch &&
              capture.frame_id.frame == current.frame_id.frame &&
              capture.generation == current.generation &&
              capture.complete_mask == 31 && capture.region_mask == 31,
          "SNAPSHOT_COHERENCE_FAILED");
  fx::Bytes bytes;
  for (std::uint32_t index = 0; index < capture.region_count; ++index) {
    native::NativeRegion region;
    ok(reference.region(index, region));
    require(region.data.size() == region.byte_count,
            "SNAPSHOT_REGION_SIZE_INVALID");
    bytes.push_back(static_cast<std::uint8_t>(region.kind));
    bytes.insert(bytes.end(), region.data.begin(), region.data.end());
  }
  return bytes;
}
struct Observer {
  bool enabled{};
  std::array<std::uint64_t, 2> cursors{};
  std::uint64_t snapshot_reads{}, event_batches{};
  void prepare(native::NativeEmulation &emulation) const {
    if (!enabled)
      return;
    const auto current = info(emulation);
    const std::array<native::NativeControlItem, 2> controls{
        {{1, 1, 31}, {2, 1, 31}}};
    ok(emulation.submit_controls({current.frame_id.epoch, 1, 1, controls}));
  }
  fx::Bytes events(native::NativeEmulation &emulation, unsigned reader) {
    std::uint64_t required{}, next = cursors[reader];
    const auto query = emulation.events(cursors[reader], {}, required, next);
    require(query.code == 0 || query.code == -3, "EVENT_SIZE_QUERY_FAILED");
    require(required <= 64ULL * 1024 * 1024, "EVENT_BATCH_LIMIT_EXCEEDED");
    fx::Bytes bytes(static_cast<std::size_t>(required));
    ok(emulation.events(cursors[reader], bytes, required, next));
    cursors[reader] = next;
    ++event_batches;
    return bytes;
  }
  void consume(native::NativeEmulation &emulation,
               native::NativeResult advanced) {
    if (!enabled)
      return;
    const auto current = info(emulation);
    if (current.frame_id.frame == 1)
      require(advanced.flags == native::native_controls_applied &&
                  advanced.transaction == 1 &&
                  advanced.frame_id.epoch == current.frame_id.epoch &&
                  advanced.frame_id.frame == 1,
              "CONTROL_CONFIRMATION_MISSING");
    native::NativeSnapshot first, second;
    ok(emulation.snapshot_acquire(first));
    ok(first.retain(second));
    const auto first_data = snapshot(first, current);
    ++snapshot_reads;
    require(first_data == snapshot(second, current),
            "INDEPENDENT_SNAPSHOT_READERS_DIVERGE");
    ++snapshot_reads;
    const auto first_events = events(emulation, 0);
    require(first_events == events(emulation, 1),
            "INDEPENDENT_EVENT_CURSORS_DIVERGE");
  }
  Json json() const {
    return {{"mode", enabled ? "two-readers" : "off"},
            {"readers", enabled ? 2 : 0},
            {"concurrent", false},
            {"snapshot_reads", snapshot_reads},
            {"event_batches", event_batches}};
  }
};
struct Samples {
  std::vector<double> step, hashes, observation, oracle;
  static Json series(const std::vector<double> &values) {
    auto ordered = values;
    std::ranges::sort(ordered);
    const auto percentile = [&](std::size_t percent) {
      return ordered.empty()
                 ? 0.0
                 : ordered[(ordered.size() * percent + 99) / 100 - 1];
    };
    return {{"unit", "microseconds"}, {"count", values.size()},
            {"samples", values},      {"p50", percentile(50)},
            {"p95", percentile(95)},  {"p99", percentile(99)}};
  }
  Json json() const {
    return {{"step", series(step)},
            {"canonical_hashes", series(hashes)},
            {"observation", series(observation)},
            {"functional_oracle", series(oracle)}};
  }
};
double microseconds(Clock::time_point begin, Clock::time_point end) {
  return std::chrono::duration<double, std::micro>(end - begin).count();
}
Json identity() {
  FILETIME created{}, ended{}, kernel{}, user{};
  require(GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel,
                          &user) != FALSE,
          "PROCESS_IDENTITY_FAILED");
  const auto birth =
      (std::uint64_t{created.dwHighDateTime} << 32) | created.dwLowDateTime;
  return {{"pid", GetCurrentProcessId()},
          {"process_creation_time_utc_100ns", birth},
          {"process_instance", std::to_string(GetCurrentProcessId()) + "/" +
                                   std::to_string(birth)}};
}

Json self_tests() {
  std::uint64_t checks{};
  const auto check = [&](bool condition, std::string_view code) {
    require(condition, code);
    ++checks;
  };
  const auto selected = backend("synthetic.complete");
  native::NativeConfig configuration;
  configuration.backend_id = selected.id;
  configuration.content = fx::content(true);
  configuration.config = fx::configuration(selected);
  configuration.initial_persistent_memory = fx::initial_memory();
  auto owner = create(configuration);
  auto &emulation = *owner->native_emulation();
  const auto initial = state(emulation);
  check(initial == fx::expected(selected, false, 0).state,
        "FACADE_INITIAL_ORACLE_FAILED");
  ok(step(emulation));
  const auto before = info(emulation);
  const auto bytes = state(emulation);
  const auto &frame_view = owner->native_frame();
  check(frame_view.pixel_encoding == ayther::FramePixelEncoding::rgba8 &&
            frame_view.native_result.code == 0,
        "FACADE_ORIGINAL_FORMAT_FAILED");
  check(frame_view.native_original_reason ==
            ayther::NativeOriginalReason::pack_not_integrated,
        "FACADE_ORIGINAL_FALLBACK_NOT_EXPLICIT");
  const std::array<native::NativeControlItem, 2> controls{
      {{1, 1, 31}, {2, 1, 31}}};
  ok(emulation.submit_controls({before.frame_id.epoch, 2, 77, controls}));
  owner->set_input(0, std::uint16_t{32});
  check(frame_view.native_result.code == -6,
        "LEGACY_INPUT_REJECTION_NOT_OBSERVABLE");
  check(owner->step().native_result.code == -6, "LEGACY_STEP_NOT_REJECTED");
  check(state(emulation) == bytes &&
            info(emulation).frame_id.frame == before.frame_id.frame,
        "LEGACY_CALL_ADVANCED_NATIVE_MACHINE");
  check(info(emulation).pending_transaction == 77,
        "LEGACY_CALL_APPLIED_PENDING_CONTROLS");
  const auto reset_legacy = emulation.reset();
  check(reset_legacy.code == 0 && reset_legacy.transaction == 77 &&
            reset_legacy.cancel_reason == 1 && reset_legacy.flags == 2 &&
            reset_legacy.frame_id.epoch == before.frame_id.epoch &&
            reset_legacy.frame_id.frame == 2,
        "RESET_DROPPED_LEGACY_PRESERVED_PENDING_RESULT");
  check(state(emulation) == initial && info(emulation).frame_id.frame == 0,
        "FACADE_RESET_INITIAL_STATE_FAILED");
  const auto &after_reset_view = owner->native_frame();
  check(after_reset_view.native_result.code == -5 &&
            after_reset_view.fb_pixels == nullptr &&
            after_reset_view.native_original.pixels.empty() &&
            after_reset_view.native_original.pcm.empty(),
        "DRIVER_RESET_LEFT_DANGLING_FRAME_VIEW");
  struct RejectRender final : native::render_observation::RenderObserver {
    void on_render_frame(
        const native::render_observation::RenderFrameView &) noexcept override {
    }
  } rejected_render;
  for (unsigned negative = 0; negative < 10; ++negative) {
    auto rejected = session_config(configuration);
    switch (negative) {
    case 0:
      rejected.enable_audio = true;
      break;
    case 1:
      rejected.pack_path = "unavailable-native-pack.ay";
      break;
    case 2:
      rejected.patch_path = "unsupported-native-patch.ips";
      break;
    case 3:
      rejected.core_options.emplace_back("legacy", "must-reject");
      break;
    case 4:
      rejected.rom_path = "legacy-content-must-not-be-opened.md";
      break;
    case 5:
      rejected.native_config.backend_id = "unavailable.native.backend";
      break;
    case 6:
      rejected.provider = static_cast<ayther::EmulationProvider>(99);
      break;
    case 7:
      rejected.audio_observer.on_fact =
          +[](void *, const native::audio_observation::FactView &) noexcept {};
      break;
    case 8:
      rejected.audio_observer.on_pcm =
          +[](void *, const native::audio_observation::PcmView &) noexcept {};
      break;
    default:
      rejected.render_observer = &rejected_render;
      break;
    }
    native::NativeResult result;
    auto created = ayther::AytherSession::create(rejected, result);
    check(!static_cast<bool>(created) && result.code < 0 &&
              !created.value.has_value(),
          "INCOMPATIBLE_FACADE_CONFIGURATION_ACCEPTED");
    if (negative < 5 || negative >= 7)
      check(result.code == -6, "FACADE_CONFIGURATION_REJECTION_NOT_EXPLICIT");
  }
  auto incompatible = session_config(configuration);
  incompatible.native_config.config.back() ^= 1;
  native::NativeResult creation;
  check(!static_cast<bool>(
            ayther::AytherSession::create(incompatible, creation)) &&
            creation.code == -1,
        "INCOMPATIBLE_NATIVE_CONFIGURATION_ACCEPTED");
  {
    const auto base = info(emulation);
    const auto saved = state(emulation);
    ok(emulation.submit_controls({base.frame_id.epoch, 1, 88, controls}));
    check(emulation.submit_controls({base.frame_id.epoch, 1, 89, controls})
                  .code == -19,
          "SECOND_PENDING_REPLACED_FIRST");
    ok(emulation.pause());
    check(state(emulation) == saved &&
              info(emulation).pending_transaction == 88,
          "PAUSE_APPLIED_PENDING_OR_ADVANCED");
    const auto reject =
        [&](std::span<const native::NativeInputAssignment> values,
            std::uint64_t epoch, std::uint64_t frame, std::int32_t code) {
          check(emulation.step({epoch, frame, values}).code == code,
                "COMPLETE_INPUT_REJECTION_CODE_WRONG");
          const auto unchanged = info(emulation);
          check(state(emulation) == saved &&
                    unchanged.frame_id.epoch == base.frame_id.epoch &&
                    unchanged.frame_id.frame == 0 &&
                    unchanged.generation == base.generation &&
                    unchanged.logical_tick == 0 &&
                    unchanged.pending_transaction == 88,
                "REJECTED_INPUT_CHANGED_STATE_OR_PENDING");
        };
    const auto neutral = assignments();
    std::vector<native::NativeInputAssignment> duplicate(neutral.begin(),
                                                         neutral.end());
    duplicate.push_back(neutral.front());
    reject(duplicate, base.frame_id.epoch, 1, -14);
    duplicate.back().value = 1;
    reject(duplicate, base.frame_id.epoch, 1, -16);
    reject(std::span(neutral).first(7), base.frame_id.epoch, 1, -15);
    reject(neutral, base.frame_id.epoch, 0, -12);
    reject(neutral, base.frame_id.epoch, 2, -13);
    reject(neutral, base.frame_id.epoch + 1, 1, -11);
    auto invalid = neutral;
    invalid[7].value = 2;
    reject(invalid, base.frame_id.epoch, 1, -1);
    const auto applied = step(emulation);
    check(applied.code == 0 && applied.transaction == 88 &&
              applied.flags == 1 &&
              applied.frame_id.epoch == base.frame_id.epoch &&
              applied.frame_id.frame == 1,
          "NEXT_VALID_FRAME_DID_NOT_CONFIRM_ORIGINAL_PENDING");
    ok(emulation.reset());
    check(state(emulation) == initial,
          "RESET_AFTER_INPUT_NEGATIVES_NOT_INITIAL");
  }
  {
    auto fault_config = configuration;
    fault_config.content = fx::content(true, 1);
    auto fault_owner = create(fault_config);
    auto &fault = *fault_owner->native_emulation();
    const auto initial_fault = state(fault);
    ok(step(fault));
    const auto base = info(fault);
    ok(fault.submit_controls({base.frame_id.epoch, 2, 101, controls}));
    const auto before_fault = state(fault);
    check(fault.reset().code == -27, "RESET_FAULT_NOT_PROPAGATED_BY_FACADE");
    check(state(fault) == before_fault &&
              info(fault).pending_transaction == 101 &&
              info(fault).frame_id.epoch == base.frame_id.epoch,
          "FAILED_RESET_MUTATED_FACADE_SESSION");
    const auto success = fault.reset();
    check(success.code == 0 && success.transaction == 101 &&
              success.flags == 2 && success.cancel_reason == 1 &&
              state(fault) == initial_fault,
          "RESET_RETRY_DID_NOT_RESTORE_INITIAL_AND_CANCEL");
  }
  {
    auto replay_owner = create(configuration);
    auto &replay = *replay_owner->native_emulation();
    auto trace = fx::neutral_trace(configuration, 2);
    ok(replay.bind_trace(trace));
    ok(replay.replay_next());
    ok(replay.replay_next());
    const auto base = info(replay);
    const auto saved = state(replay);
    ok(replay.submit_controls({base.frame_id.epoch, 3, 201, controls}));
    check(replay.replay_next().code == 1 && state(replay) == saved &&
              info(replay).pending_transaction == 201 &&
              info(replay).next_replay_frame == 3,
          "EOF_CHANGED_STATE_OR_APPLIED_PENDING");
    trace.pop_back();
    check(replay.bind_trace(trace).code == -17 && state(replay) == saved &&
              info(replay).pending_transaction == 201,
          "INVALID_TRACE_MUTATED_FACADE_SESSION");
  }
  {
    auto budget_owner = create(configuration);
    auto &budget = *budget_owner->native_emulation();
    auto base = info(budget);
    ok(budget.submit_controls({base.frame_id.epoch, 1, 301, controls}));
    std::array<native::NativeSnapshot, 8> held;
    for (unsigned index = 0; index < held.size(); ++index) {
      ok(step(budget));
      ok(budget.snapshot_acquire(held[index]));
    }
    native::NativeSnapshot duplicate;
    ok(budget.snapshot_acquire(duplicate));
    ok(duplicate.release());
    const auto historic = info(budget);
    const auto saved = snapshot(held.back(), historic);
    ok(step(budget));
    native::NativeSnapshot absent;
    check(budget.snapshot_acquire(absent).code == -18,
          "SNAPSHOT_OBJECT_BUDGET_NOT_ENFORCED");
    native::NativeFrame original;
    ok(budget.original(original));
    fx::verify(selected, false, 9, state(budget), original);
    check(snapshot(held.back(), historic) == saved,
          "BUDGET_FAILURE_OVERWROTE_RETAINED_CAPTURE");
    check((read_le(tlv_field(loss_bytes(budget), 4), 0, 4) & 2) != 0,
          "SNAPSHOT_BUDGET_LOSS_NOT_REPORTED");
    ok(held.front().release());
    ok(step(budget));
    ok(budget.snapshot_acquire(absent));
    check(info(budget).frame_id.frame == 10, "SNAPSHOT_BUDGET_DID_NOT_RECOVER");
  }
  {
    auto event_owner = create(configuration);
    auto &events = *event_owner->native_emulation();
    const auto base = info(events);
    ok(events.submit_controls({base.frame_id.epoch, 1, 401, controls}));
    ok(step(events));
    const auto early = read_events(events, 0);
    for (unsigned frame = 2; frame <= 2200; ++frame)
      ok(step(events));
    const auto losses = loss_bytes(events);
    check(read_le(tlv_field(losses, 1), 0, 8) > 0 &&
              (read_le(tlv_field(losses, 4), 0, 4) & 1),
          "SATURATED_QUEUE_LOSS_NOT_REPORTED");
    std::uint64_t required{}, next = 991;
    check(events.events(0, {}, required, next).code == -3 && next == 991,
          "EVENT_SIZE_QUERY_ADVANCED_CURSOR");
    fx::Bytes bytes_out(static_cast<std::size_t>(required), 0xCC);
    check(events.events(0, std::span(bytes_out).first(bytes_out.size() - 1),
                        required, next)
                      .code == -3 &&
              next == 991 &&
              std::ranges::all_of(bytes_out,
                                  [](auto byte) { return byte == 0xCC; }),
          "SMALL_EVENT_BUFFER_PARTIAL_WRITE_OR_CURSOR_ADVANCE");
    const auto reader_a = read_events(events, early.next),
               reader_b = read_events(events, 0);
    check(reader_a.count == 4096 && reader_b.count == 4096 &&
              reader_a.bytes == reader_b.bytes,
          "SLOW_READER_GAP_OR_INDEPENDENT_CURSOR_WRONG");
    check(read_events(events, reader_a.next).count == 0 &&
              read_events(events, 0).count == 4096,
          "READING_ONE_CURSOR_CONSUMED_OTHER_READER");
    const auto saturated = info(events);
    ok(events.submit_controls({saturated.frame_id.epoch, 2201, 402, controls}));
    const auto reset_result = events.reset();
    check(reset_result.code == 0 && reset_result.transaction == 402 &&
              reset_result.flags == 2 && reset_result.cancel_reason == 1 &&
              reset_result.frame_id.epoch == saturated.frame_id.epoch &&
              reset_result.frame_id.frame == 2201,
          "SATURATED_RESET_CANCELLATION_LOST");
    check(read_events(events, 0).count == 4096,
          "RESET_UNEXPECTEDLY_REMOVED_QUEUE_PROOF");
    const auto fresh = info(events);
    ok(events.submit_controls({fresh.frame_id.epoch, 1, 403, controls}));
    const auto closed = events.close();
    check(closed.code == 0 && closed.transaction == 403 && closed.flags == 2 &&
              closed.cancel_reason == 3 &&
              closed.frame_id.epoch == fresh.frame_id.epoch &&
              closed.frame_id.frame == 1,
          "SATURATED_CLOSE_CANCELLATION_LOST");
  }
  const auto current = info(emulation);
  ok(emulation.submit_controls({current.frame_id.epoch, 1, 81, controls}));
  ok(step(emulation));
  const auto captured = info(emulation);
  native::NativeSnapshot first, second;
  ok(emulation.snapshot_acquire(first));
  ok(first.retain(second));
  const auto saved = snapshot(first, captured);
  ok(emulation.submit_controls({captured.frame_id.epoch, 2, 82, controls}));
  const auto closed = emulation.close();
  check(closed.code == 0 && closed.transaction == 82 && closed.flags == 2 &&
            closed.cancel_reason == 3,
        "FACADE_CLOSE_CANCEL_RESULT_LOST");
  const auto &after_close_view = owner->native_frame();
  check(after_close_view.native_result.code == -4 &&
            after_close_view.fb_pixels == nullptr &&
            after_close_view.native_original.pixels.empty() &&
            after_close_view.native_original.pcm.empty(),
        "DRIVER_CLOSE_LEFT_DANGLING_FRAME_VIEW");
  owner.reset();
  auto reader = std::async(std::launch::async,
                           [&] { return snapshot(second, captured); });
  check(snapshot(first, captured) == saved && reader.get() == saved,
        "RETAINED_CAPTURE_INVALID_AFTER_FACADE_DESTRUCTION");
  ok(first.release());
  ok(second.release());
  return {{"passed", true},
          {"checks", checks},
          {"real_facade", true},
          {"retained_after_owner_destruction", true},
          {"legacy_calls_rejected_without_advance", true},
          {"device_and_pack_rejected", true},
          {"complete_input_negatives", true},
          {"driver_reset_close_views_cleared", true},
          {"snapshot_budget_and_recovery", true},
          {"queue_4096_independent_cursors_and_losses", true},
          {"saturated_reset_close_cancellation", true},
          {"scope", "T018 facade; T016 detailed contracts remain separate"}};
}

Json worker(const std::map<std::string, std::string> &options) {
  const auto id = options.at("--backend"), scenario = options.at("--scenario"),
             observer = options.at("--observer");
  require(scenario == "neutral" || scenario == "action", "SCENARIO_INVALID");
  require(observer == "off" || observer == "two-readers", "OBSERVER_INVALID");
  const bool action = scenario == "action",
             complete = id == "synthetic.complete";
  require(complete || id == "synthetic.minimal", "BACKEND_INVALID");
  require(complete || observer == "off", "MINIMAL_OBSERVATION_UNSUPPORTED");
  const auto selected = backend(id);
  const std::filesystem::path fixture = options.at("--fixture-dir");
  native::NativeConfig configuration;
  configuration.backend_id = id;
  configuration.content = read(fixture / "content.bin");
  configuration.config = read(fixture / "config.bin");
  configuration.initial_persistent_memory = read(fixture / "memory.bin");
  const auto trace = read(fixture / "trace.bin");
  require(configuration.content == fx::content(complete) &&
              configuration.config == fx::configuration(selected) &&
              configuration.initial_persistent_memory == fx::initial_memory(),
          "FIXTURE_DOES_NOT_MATCH_INDEPENDENT_MODEL");
  auto owner = create(configuration);
  auto &emulation = *owner->native_emulation();
  const auto initial_info = info(emulation);
  const auto initial_state = state(emulation);
  require(initial_state == fx::expected(selected, false, 0).state,
          "INITIAL_ORACLE_FAILED");
  ok(emulation.pause());
  require(info(emulation).frame_id.frame == 0, "PAUSE_ADVANCED_MACHINE");
  ok(step(emulation));
  require(info(emulation).frame_id.frame == 1, "STEP_NOT_EXACTLY_ONE_FRAME");
  ok(emulation.reset());
  const auto reset = info(emulation);
  require(reset.frame_id.frame == 0 && reset.logical_tick == 0 &&
              reset.state == 1 && reset.next_replay_frame == 1 &&
              reset.frame_id.epoch != initial_info.frame_id.epoch &&
              state(emulation) == initial_state,
          "RESET_NOT_RECORDED_INITIAL_STATE");
  ok(emulation.bind_trace(trace));
  Observer observation{observer == "two-readers"};
  observation.prepare(emulation);
  Samples costs;
  Json frames = Json::array();
  frames.get_ref<Json::array_t &>().reserve(3000);
  for (std::uint64_t frame = 1; frame <= 3000; ++frame) {
    const auto started = Clock::now();
    const auto advanced = emulation.replay_next();
    const auto stepped = Clock::now();
    ok(advanced);
    const auto hash_started = Clock::now();
    const auto logical = state(emulation);
    native::NativeFrame original;
    ok(emulation.original(original));
    const auto &view = owner->native_frame();
    require(view.native_result.code == 0 &&
                view.pixel_encoding == ayther::FramePixelEncoding::rgba8 &&
                view.fb_pixels == original.pixels.data() &&
                view.native_original.pcm.data() == original.pcm.data() &&
                view.fb_width == 16 && view.fb_height == 16 &&
                view.fb_pitch == 64 &&
                view.native_original_reason ==
                    ayther::NativeOriginalReason::pack_not_integrated,
            "REAL_FRAME_VIEW_NOT_ORIGINAL_ZERO_COPY_FALLBACK");
    const auto hashes = fx::canonical(logical, original);
    const auto hashed = Clock::now();
    try {
      fx::verify(selected, action, frame, logical, original);
    } catch (const std::exception &failure) {
      throw std::runtime_error("FUNCTIONAL_ORACLE_DIVERGENCE:frame=" +
                               std::to_string(frame) + ":" + failure.what());
    }
    const auto verified = Clock::now();
    const auto current = info(emulation);
    require(current.frame_id.frame == frame && current.state == 1 &&
                current.logical_tick == frame * 1000 &&
                current.backend_build_id == selected.build_id,
            "NATIVE_FRAME_IDENTITY_DIVERGENCE");
    const auto observed_begin = Clock::now();
    observation.consume(emulation, advanced);
    const auto observed = Clock::now();
    frames.push_back({{"frame", frame},
                      {"state", hashes.state},
                      {"video", hashes.video},
                      {"audio", hashes.audio}});
    costs.step.push_back(microseconds(started, stepped));
    costs.hashes.push_back(microseconds(hash_started, hashed));
    costs.oracle.push_back(microseconds(hashed, verified));
    costs.observation.push_back(microseconds(observed_begin, observed));
  }
  const auto before_eof = info(emulation);
  const auto before_eof_state = state(emulation);
  const auto eof = emulation.replay_next();
  const auto after_eof = info(emulation);
  require(eof.code == 1 &&
              after_eof.frame_id.frame == before_eof.frame_id.frame &&
              after_eof.frame_id.epoch == before_eof.frame_id.epoch &&
              after_eof.next_replay_frame == before_eof.next_replay_frame &&
              state(emulation) == before_eof_state,
          "EOF_ADVANCED_OR_CHANGED_MACHINE");
  ok(emulation.close());
  auto report = identity();
  report.update(
      {{"schema_version", 1},
       {"code", 0},
       {"phase", 0},
       {"text", "Validación nativa completada."},
       {"route", "AytherSession->NativeEmulation->Tessera"},
       {"real_facade", true},
       {"backend",
        {{"id", id},
         {"kind", selected.kind},
         {"system", selected.system},
         {"version", selected.version},
         {"capabilities", selected.capabilities},
         {"build_id", fx::hex_bytes(selected.build_id)}}},
       {"build_id", fx::hex_bytes(selected.build_id)},
       {"scenario", scenario},
       {"content", fx::digest(configuration.content)},
       {"config", fx::digest(configuration.config)},
       {"trace", fx::digest(trace)},
       {"initial_state", fx::digest(initial_state)},
       {"clock",
        {{"tick_hz_num", reset.tick_hz_num},
         {"tick_hz_den", reset.tick_hz_den}}},
       {"requested_frames", 3000},
       {"completed_frames", frames.size()},
       {"frames", std::move(frames)},
       {"eof_code", eof.code},
       {"closed", true},
       {"observer", observation.json()},
       {"costs", costs.json()},
       {"oracle_provenance", fx::oracle_provenance},
       {"functional_oracles",
        {{"state", 3000},
         {"video", 3000},
         {"audio", 3000},
         {"action_frames", action ? 300 : 0},
         {"neutral_frames", action ? 2700 : 3000}}},
       {"lifecycle",
        {{"step_completed", 1}, {"reset_completed", 1}, {"preflight", true}}},
       {"cost_scope",
        "Wall-clock microseconds; step=real facade native replay; "
        "canonical_hashes=state/original/frame facade queries and canonical "
        "encoding/SHA; observation=two logical readers; "
        "functional_oracle=independent closed forms separately. Setup, "
        "metadata, JSON/file output excluded. No T032 acceptance."},
       {"scope", "T018 synthetic integration; no real pack, checkpoints, "
                 "Runtime or MVP acceptance"}});
  return report;
}
} // namespace

int main(int argc, char **argv) {
  std::filesystem::path report_path;
  try {
    const std::string mode = argc > 1 ? argv[1] : "--self-test";
    std::map<std::string, std::string> options;
    for (int index = 2; index < argc; index += 2) {
      require(index + 1 < argc &&
                  options.emplace(argv[index], argv[index + 1]).second,
              "WORKER_ARGUMENT_INVALID");
    }
    if (options.contains("--report"))
      report_path = options.at("--report");
    Json report;
    if (mode == "--self-test") {
      report = identity();
      report.update(
          {{"schema_version", 1}, {"code", 0}, {"self_tests", self_tests()}});
    } else {
      require(mode == "--worker", "WORKER_MODE_INVALID");
      report = worker(options);
    }
    if (!report_path.empty())
      publish(report_path, report);
    std::cout << report.dump() << '\n';
    return 0;
  } catch (const std::exception &error) {
    Json report{{"schema_version", 1},
                {"code", -27},
                {"text", "La validación nativa ha fallado."},
                {"diagnostic_code", error.what()}};
    if (!report_path.empty()) {
      try {
        publish(report_path, report);
      } catch (...) {
      }
    }
    std::cerr << report.dump() << '\n';
    return 1;
  }
}
