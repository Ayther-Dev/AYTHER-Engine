#include "audio_input_observation.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

#if defined(QA_INGRESS_SESSION)
#include "../../../tools/common/synth_rom.h"
#include <ayther/ayther_recording.h>
#include <ayther/ayther_session.h>
#endif

namespace qa = ayther::audio_qa;
namespace obs = ayther::engine::audio_observation;

namespace {
template <typename T>
const T *field(const obs::FactView &fact, std::string_view name) noexcept {
  for (const auto &value : fact.fields)
    if (value.name == name)
      return std::get_if<T>(&value.value);
  return nullptr;
}
std::uint64_t number(const obs::FactView &fact,
                     std::string_view name) noexcept {
  const auto *value = field<std::uint64_t>(fact, name);
  return value ? *value : UINT64_MAX;
}
std::string_view text(const obs::FactView &fact,
                      std::string_view name) noexcept {
  const auto *value = field<std::string_view>(fact, name);
  return value ? *value : std::string_view{};
}
struct Row {
  obs::FactId id;
  obs::FactId batch;
  std::uint64_t frame = 0;
  std::uint64_t index = 0;
  std::array<std::uint64_t, 8> payload{};
  bool raw = false;
  bool pcm = false;
  bool abi = false;
  bool legacy = false;
  bool queue = false;
};
struct Sink {
  std::array<Row, 64> rows{};
  std::size_t count = 0;
  std::uint64_t batches = 0;
  std::uint64_t writes = 0;
  std::uint64_t pcm = 0;
  std::uint64_t detector_frame = 0;
  std::uint64_t analysis_batches = 0;
  std::uint64_t analysis_inputs = 0;
  bool analysis = false;
  std::uint64_t source_intervals = 0;
  std::uint64_t detector_intervals = 0;
  std::uint64_t analysis_intervals = 0;
  std::uint64_t raw_received = 0;
  bool raw_active = false;
  obs::FactId raw_source;
  obs::FactId typed_source;
  obs::FactId batch;
  bool valid = true;
  std::uint64_t detector_created = 0;
  std::uint64_t detector_processed = 0;
  std::uint64_t detector_resets = 0;
  std::uint64_t detector_pal = 0;
  std::uint64_t detector_initial = 0;
  std::uint64_t detector_finished = 0;
  std::uint64_t detector_cleared = 0;
  std::uint64_t runtime_queries = 0;
  std::uint64_t inspection_queries = 0;
  std::uint64_t analysis_queries = 0;
  std::uint64_t learning_queries = 0;
  static void receive(void *context, const obs::FactView &fact) noexcept {
    auto &self = *static_cast<Sink *>(context);
    if (fact.kind == "detector_state_operation") {
      const auto *complete = field<bool>(fact, "state_history_complete");
      self.valid =
          self.valid && complete && *complete && fact.state_orders.size() == 1;
      const auto action = text(fact, "operation");
      if (action == "created")
        ++self.detector_created;
      if (action == "reset")
        ++self.detector_resets;
      if (action == "set_pal")
        ++self.detector_pal;
      if (action == "set_initial_active")
        ++self.detector_initial;
      if (action == "finish")
        ++self.detector_finished;
      if (action == "clear_closed")
        ++self.detector_cleared;
      if (action == "process_frame") {
        ++self.detector_processed;
        const auto *input = field<obs::FactId>(fact, "input_batch");
        self.valid = self.valid && input && *input == self.batch;
      }
      return;
    }
    if (fact.kind == "detector_output_batch") {
      const auto *complete = field<bool>(fact, "state_history_complete");
      const auto *read = field<bool>(fact, "read_complete");
      self.valid = self.valid && complete && *complete && read && *read &&
                   fact.causes.size() == 1;
      const auto use = text(fact, "use");
      if (use == "runtime_selection")
        ++self.runtime_queries;
      if (use == "inspection")
        ++self.inspection_queries;
      if (use == "analysis_result")
        ++self.analysis_queries;
      if (use == "live_learning")
        ++self.learning_queries;
      return;
    }
    if (fact.kind == "audio_source_interval") {
      ++self.source_intervals;
      if (text(fact, "source") == "chip_writes") {
        self.raw_source = fact.id;
        self.raw_received = number(fact, "received");
        self.raw_active = text(fact, "state") == "active";
      } else {
        self.typed_source = fact.id;
      }
      return;
    }
    if (fact.kind == "audio_detector_interval") {
      ++self.detector_intervals;
      if (text(fact, "detector") == "analysis")
        ++self.analysis_intervals;
      const auto *completed = field<bool>(fact, "call_completed");
      self.valid = self.valid && completed && *completed &&
                   fact.causes.size() == 3 &&
                   number(fact, "submitted_inputs") == self.writes + self.pcm;
      if (fact.causes.size() == 3) {
        const auto *raw = std::get_if<obs::FactId>(&fact.causes[0]);
        const auto *typed = std::get_if<obs::FactId>(&fact.causes[1]);
        const auto *batch = std::get_if<obs::FactId>(&fact.causes[2]);
        self.valid = self.valid && raw && typed && batch &&
                     *raw == self.raw_source && *typed == self.typed_source &&
                     *batch == self.batch;
      }
      return;
    }
    if (fact.kind == "detector_input_batch") {
      ++self.batches;
      self.batch = fact.id;
      self.writes = number(fact, "raw_write_count");
      self.pcm = number(fact, "pcm_event_count");
      self.detector_frame = number(fact, "detector_frame");
      self.analysis = text(fact, "detector") == "analysis";
      if (self.analysis)
        ++self.analysis_batches;
      self.valid =
          self.valid && text(fact, "cross_stream_order") == "independent";
      return;
    }
    if (fact.kind != "detector_input")
      return;
    if (self.analysis)
      ++self.analysis_inputs;
    self.valid = self.valid &&
                 fact.frame.availability == obs::Availability::known &&
                 fact.id.producer ==
                     static_cast<std::uint32_t>(qa::Producer::detector_input);
    Row row;
    row.id = fact.id;
    row.frame = fact.frame.emulation_frame;
    row.index = number(fact, "source_index");
    row.raw = text(fact, "input_type") == "chip_write";
    row.pcm = text(fact, "input_type") == "pcm_event";
    row.abi = text(fact, "origin") == "abi_snapshot";
    row.legacy = text(fact, "origin") == "legacy_core";
    row.queue = text(fact, "origin") == "core_event_queue";
    if (fact.causes.size() == 1) {
      if (const auto *id = std::get_if<obs::FactId>(&fact.causes[0]))
        row.batch = *id;
    }
    self.valid = self.valid && row.batch == self.batch;
    constexpr std::array raw_names{"cycle", "address", "data", "chip"};
    constexpr std::array pcm_names{"event_kind", "channel", "envelope", "pan",
                                   "start",      "loop",    "rate"};
    if (row.raw) {
      for (std::size_t i = 0; i < raw_names.size(); ++i)
        row.payload[i] = number(fact, raw_names[i]);
    } else if (row.pcm) {
      for (std::size_t i = 0; i < pcm_names.size(); ++i)
        row.payload[i] = number(fact, pcm_names[i]);
    } else
      self.valid = false;
    if (self.count < self.rows.size())
      self.rows[self.count] = row;
    ++self.count;
  }
};

bool ingress_values() {
  qa::IdentitySource ids;
  Sink sink;
  std::array<AytherAudioWrite, 2> writes{
      {{UINT32_MAX, 0x1ff, 0x80, 0}, {0, 0, 0x90, 1}}};
  std::array<AytherPcmEvent, 4> pcm{
      {{AYTHER_PCM_KEY_ON, 7, 255, 128, 254, 0, 65535, 65534},
       {AYTHER_PCM_PITCH, 7, 3, 4, 5, 0, 6, 7},
       {AYTHER_PCM_VOLUME, 7, 8, 9, 0, 0, 0, 0},
       {AYTHER_PCM_KEY_OFF, 7, 0, 0, 0, 0, 0, 0}}};
  const qa::DetectorInput input{
      UINT64_C(0x100000005),         5,      qa::DetectorPath::live,
      qa::WriteOrigin::abi_snapshot, writes, pcm};
  const auto result =
      qa::observe_detector_input({&sink, Sink::receive, nullptr}, ids, input);
  if (!result.complete || !result.batch || !sink.valid || sink.batches != 1 ||
      sink.count != 6 || sink.writes != 2 || sink.pcm != 4 ||
      sink.detector_frame != 5 || sink.analysis)
    return false;
  for (std::size_t i = 0; i < sink.count; ++i) {
    const auto &row = sink.rows[i];
    if (row.frame != input.session_frame ||
        row.id.sequence != result.batch->sequence + i + 1 ||
        row.index != (i < 2 ? i : i - 2))
      return false;
  }
  writes[0] = {};
  pcm[0] = {};
  if (!sink.rows[0].abi || sink.rows[0].payload[0] != UINT32_MAX ||
      sink.rows[0].payload[1] != 0x1ff || !sink.rows[2].queue ||
      sink.rows[2].payload !=
          std::array<std::uint64_t, 8>{1, 7, 255, 128, 254, 65535, 65534, 0})
    return false;
  const auto last = sink.rows[5].id;
  sink = {};
  const auto empty = qa::observe_detector_input(
      {&sink, Sink::receive, nullptr}, ids,
      {9, 9, qa::DetectorPath::analysis, qa::WriteOrigin::legacy_core, {}, {}});
  if (!empty.complete || !empty.batch ||
      empty.batch->sequence <= last.sequence || sink.batches != 1 ||
      sink.count != 0 || sink.writes != 0 || sink.pcm != 0)
    return false;
  const auto disabled = qa::observe_detector_input({}, ids, input);
  return disabled.complete && !disabled.batch;
}

bool bounded_storage() {
  qa::IdentitySource ids;
  Sink sink;
  const std::vector<AytherAudioWrite> writes(4097,
                                             AytherAudioWrite{0, 0, 0, 255});
  const auto result = qa::observe_detector_input(
      {&sink, Sink::receive, nullptr}, ids,
      {1, 1, qa::DetectorPath::live, qa::WriteOrigin::legacy_core, writes, {}});
  return result.complete && sink.valid && sink.count == writes.size() &&
         sink.writes == writes.size() && sink.rows[0].legacy &&
         sink.rows[0].payload[3] == 255;
}

struct DetectorDelete {
  void operator()(AytherAudioEventDetector *detector) const noexcept {
    ayther_audio_event_free(detector);
  }
};
using Detector = std::unique_ptr<AytherAudioEventDetector, DetectorDelete>;
bool detector_unchanged() {
  Detector plain{ayther_audio_event_new()};
  Detector observed{ayther_audio_event_new()};
  if (!plain || !observed)
    return false;
  const std::array<AytherPcmEvent, 1> on{
      {{AYTHER_PCM_KEY_ON, 2, 192, 255, 42, 0, 4096, 2048}}};
  const std::array<AytherPcmEvent, 1> off{
      {{AYTHER_PCM_KEY_OFF, 2, 0, 0, 0, 0, 0, 0}}};
  qa::IdentitySource ids;
  Sink sink;
  for (std::uint32_t frame = 0; frame < 2; ++frame) {
    const auto &pcm = frame == 0 ? on : off;
    const auto result =
        qa::observe_detector_input({&sink, Sink::receive, nullptr}, ids,
                                   {frame,
                                    frame,
                                    qa::DetectorPath::live,
                                    qa::WriteOrigin::abi_snapshot,
                                    {},
                                    pcm});
    if (!result.complete)
      return false;
    ayther_audio_event_process_frame_ex(observed.get(), frame, nullptr, 0,
                                        pcm.data(), 1);
    ayther_audio_event_process_frame_ex(plain.get(), frame, nullptr, 0,
                                        pcm.data(), 1);
  }
  AytherAudioEvent left{}, right{};
  if (ayther_audio_event_get(plain.get(), &left, 1) != 1 ||
      ayther_audio_event_get(observed.get(), &right, 1) != 1)
    return false;
  return left.signature == right.signature &&
         left.instrument == right.instrument &&
         left.start_frame == right.start_frame &&
         left.end_frame == right.end_frame && left.chip == right.chip &&
         left.channel == right.channel && left.pitch == right.pitch &&
         left.velocity == right.velocity;
}

#if defined(QA_INGRESS_SESSION)
bool session_ingress() {
  Sink sink;
  ayther::AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = ayther::synth::canonical_rom_path();
  config.enable_audio = false;
  config.derive_core_pack = false;
  config.audio_observer = {&sink, Sink::receive, nullptr};
  auto session = ayther::AytherSession::create(config);
  if (!session)
    return false;
  if (sink.detector_created != 2)
    return false;
  (*session)->set_audio_runtime_substitution(true);
  (*session)->record_start();
  const auto &frame = (*session)->step();
  if (!sink.valid || sink.batches != 1 ||
      sink.count != frame.chip_write_count || sink.count == 0 ||
      sink.count > sink.rows.size() || sink.source_intervals != 2 ||
      sink.detector_intervals != 1 || !sink.raw_active ||
      sink.raw_received != frame.chip_write_count ||
      sink.detector_processed != 1 || sink.detector_cleared != 1 ||
      sink.runtime_queries != 1 || sink.learning_queries != 1)
    return false;
  for (std::size_t i = 0; i < sink.count; ++i) {
    const auto &row = sink.rows[i];
    const auto &write = frame.chip_writes[i];
    if (!row.raw || !row.abi || row.index != i ||
        row.frame != frame.frame_index || row.payload[0] != write.cycle ||
        row.payload[1] != write.addr || row.payload[2] != write.data ||
        row.payload[3] != write.chip)
      return false;
  }
  const auto writes_per_frame = frame.chip_write_count;
  std::array<AytherAudioActive, 64> active{};
  (void)(*session)->audio_live_active(
      active.data(), static_cast<std::uint32_t>(active.size()));
  if (sink.inspection_queries != 1)
    return false;
  (*session)->record_stop();
  const auto recording = (*session)->take_recording();
  if (recording.frame_count() != 1)
    return false;
  sink = {};
  (void)(*session)->analyze_audio_events(recording);
  if (!sink.valid || sink.analysis_batches != 1 ||
      sink.analysis_intervals != 1 ||
      sink.analysis_inputs != writes_per_frame || sink.detector_frame != 0 ||
      sink.detector_resets < 1 || sink.detector_pal != 2 ||
      sink.detector_initial != 1 || sink.detector_finished != 1 ||
      sink.analysis_queries != 1)
    return false;
  const auto resets = sink.detector_resets;
  (*session)->reset();
  return sink.valid && sink.detector_resets == resets + 1;
}
#endif
} // namespace

int main() {
  try {
    if (!ingress_values()) {
      std::cerr << "ingress values failed\n";
      return 1;
    }
    if (!bounded_storage()) {
      std::cerr << "bounded storage failed\n";
      return 1;
    }
    if (!detector_unchanged()) {
      std::cerr << "detector equivalence failed\n";
      return 1;
    }
#if defined(QA_INGRESS_SESSION)
    if (!session_ingress()) {
      std::cerr << "session ingress failed\n";
      return 1;
    }
#endif
    return 0;
  } catch (...) {
    return 2;
  }
}
