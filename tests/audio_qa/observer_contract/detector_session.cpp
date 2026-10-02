#include "../../../tools/common/synth_rom.h"
#include "audio_detector_observation.h"
#include <ayther/ayther_recording.h>
#include <ayther/ayther_session.h>

#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>

namespace qa = ayther::audio_qa;
namespace obs = ayther::engine::audio_observation;

namespace {
using Row = std::array<std::uint64_t, 9>;
Row values(const AytherAudioActive &row) {
  return {row.signature,
          row.instrument,
          row.chip,
          row.channel,
          row.pitch,
          0,
          0,
          0,
          0};
}
Row values(const AytherAudioEvent &row) {
  return {row.signature, row.instrument, row.chip,
          row.channel,   row.pitch,      row.start_frame,
          row.end_frame, row.velocity,   1};
}
template <typename T>
const T *field(const obs::FactView &fact, std::string_view name) noexcept {
  for (const auto &item : fact.fields)
    if (item.name == name)
      return std::get_if<T>(&item.value);
  return nullptr;
}
std::string_view text(const obs::FactView &fact,
                      std::string_view name) noexcept {
  const auto *value = field<std::string_view>(fact, name);
  return value ? *value : std::string_view{};
}
std::uint64_t number(const obs::FactView &fact,
                     std::string_view name) noexcept {
  const auto *value = field<std::uint64_t>(fact, name);
  return value ? *value : UINT64_MAX;
}
bool truth(const obs::FactView &fact, std::string_view name) noexcept {
  const auto *value = field<bool>(fact, name);
  return value && *value;
}
bool cause_is(const obs::FactView &fact, std::size_t index,
              obs::FactId expected) noexcept {
  if (index >= fact.causes.size())
    return false;
  const auto *actual = std::get_if<obs::FactId>(&fact.causes[index]);
  return actual && *actual == expected;
}
enum class Use : std::size_t { selection, learning, analysis, inspection };
struct Snapshot {
  obs::FactId id;
  std::array<Row, 256> rows{};
  std::size_t expected = 0;
  std::size_t count = 0;
};
struct Sink {
  std::array<std::optional<obs::FactId>, 2> previous{};
  std::array<std::optional<obs::FactId>, 2> input{};
  std::array<Snapshot, 4> snapshots{};
  Use current = Use::selection;
  std::uint64_t sequence = 0;
  std::size_t active_rows = 0;
  std::size_t closed_rows = 0;
  std::size_t learning_rows = 0;
  std::size_t calls = 0;
  bool drop = false;
  bool lost = false;
  bool valid = true;
  const Snapshot &snapshot(Use use) const {
    return snapshots[static_cast<std::size_t>(use)];
  }
  static void receive(void *context, const obs::FactView &fact) noexcept {
    auto &self = *static_cast<Sink *>(context);
    ++self.calls;
    if (self.drop && self.calls > 32) {
      self.lost = true;
      return;
    }
    const auto detector = text(fact, "detector");
    const std::size_t lane = detector == "analysis" ? 1 : 0;
    if (fact.kind == "detector_input_batch") {
      self.input[lane] = fact.id;
      return;
    }
    if (fact.id.producer != static_cast<std::uint32_t>(qa::Producer::detector))
      return;
    self.valid = self.valid && fact.id.sequence > self.sequence;
    self.sequence = fact.id.sequence;
    if (fact.kind == "detector_state_operation") {
      self.valid = self.valid && truth(fact, "state_history_complete") &&
                   fact.state_orders.size() == 1 &&
                   fact.state_orders[0].sequence == fact.id.sequence;
      std::size_t causes = 0;
      if (self.previous[lane])
        self.valid =
            self.valid && cause_is(fact, causes++, *self.previous[lane]);
      if (text(fact, "operation") == "process_frame") {
        self.valid = self.valid && self.input[lane].has_value();
        if (self.input[lane])
          self.valid =
              self.valid && cause_is(fact, causes++, *self.input[lane]);
      }
      self.valid = self.valid && fact.causes.size() == causes;
      self.previous[lane] = fact.id;
      return;
    }
    if (fact.kind == "detector_output_batch") {
      const auto &last = self.snapshot(self.current);
      self.valid = self.valid && last.count == last.expected &&
                   truth(fact, "read_complete") &&
                   truth(fact, "state_history_complete") &&
                   self.previous[lane].has_value();
      if (self.previous[lane])
        self.valid = self.valid && fact.causes.size() == 1 &&
                     cause_is(fact, 0, *self.previous[lane]);
      const auto use = text(fact, "use");
      if (use == "runtime_selection")
        self.current = Use::selection;
      else if (use == "live_learning")
        self.current = Use::learning;
      else if (use == "analysis_result")
        self.current = Use::analysis;
      else if (use == "inspection")
        self.current = Use::inspection;
      else
        self.valid = false;
      auto &target = self.snapshots[static_cast<std::size_t>(self.current)];
      target.count = 0;
      target.id = fact.id;
      const auto count = number(fact, "observed_count");
      if (count > target.rows.size()) {
        self.valid = false;
        return;
      }
      target.expected = static_cast<std::size_t>(count);
      return;
    }
    const bool closed = fact.kind == "detector_closed_event";
    if (!closed && fact.kind != "detector_active_channel") {
      self.valid = false;
      return;
    }
    auto &target = self.snapshots[static_cast<std::size_t>(self.current)];
    self.valid = self.valid && fact.causes.size() == 1 &&
                 cause_is(fact, 0, target.id) &&
                 number(fact, "source_index") == target.count;
    if (target.count >= target.rows.size()) {
      self.valid = false;
      return;
    }
    Row row{};
    constexpr std::array names{"signature", "instrument", "chip",
                               "channel",   "pitch",      "start_frame",
                               "end_frame", "velocity"};
    for (std::size_t i = 0; i < (closed ? 8U : 5U); ++i)
      row[i] = number(fact, names[i]);
    row[8] = closed ? 1 : 0;
    target.rows[target.count++] = row;
    if (closed)
      ++self.closed_rows;
    else
      ++self.active_rows;
    if (closed && self.current == Use::learning)
      ++self.learning_rows;
  }
};
struct Result {
  std::vector<std::vector<Row>> active;
  std::vector<Row> closed;
  std::vector<std::uint8_t> state;
  friend bool operator==(const Result &, const Result &) = default;
};
bool matches(const Snapshot &snapshot, const std::vector<Row> &rows) {
  return snapshot.count == rows.size() && snapshot.expected == rows.size() &&
         std::equal(rows.begin(), rows.end(), snapshot.rows.begin());
}
std::optional<Result> run(unsigned mode) {
  Sink sink;
  sink.drop = mode == 2;
  ayther::AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  // The test core derives writes from the ROM CRC. This title makes the
  // canonical program's CRC 0x6ecdfdd0: PSG ch1 opens (0xba) and closes
  // (0xbf) within each frame while ch0/ch3 remain active. No core changes.
  ayther::synth::Rom rom("AYTHER QA DETECTOR 128");
  ayther::synth::program_canonical(rom, true);
  const auto rom_path =
      std::filesystem::current_path() / "detector-sequence.md";
  if (!rom.save(rom_path))
    return {};
  config.rom_path = rom_path.string();
  config.enable_audio = false;
  config.derive_core_pack = false;
  if (mode != 0)
    config.audio_observer = {&sink, Sink::receive, nullptr};
  auto created = ayther::AytherSession::create(config);
  if (!created)
    return {};
  auto &session = **created;
  session.set_audio_runtime_substitution(true);
  session.record_start();
  const std::unique_ptr<AytherAudioEventDetector,
                        decltype(&ayther_audio_event_free)>
      reference{ayther_audio_event_new(), ayther_audio_event_free};
  if (!reference)
    return {};
  Result result;
  for (unsigned frame = 0; frame < 8; ++frame) {
    session.set_input(0, static_cast<std::uint16_t>(frame & 1U));
    const auto &view = session.step();
    // The synthetic core has no typed PCM transport. Feed its real write
    // array to an independent detector to check the live learning values.
    ayther_audio_event_process_frame_ex(
        reference.get(), static_cast<std::uint32_t>(view.frame_index),
        view.chip_writes, view.chip_write_count, nullptr, 0);
    std::array<AytherAudioEvent, 64> reference_events{};
    const auto reference_count = ayther_audio_event_count(reference.get());
    if (reference_count == 0 || reference_count > reference_events.size() ||
        ayther_audio_event_get(reference.get(), reference_events.data(),
                               static_cast<std::uint32_t>(
                                   reference_events.size())) != reference_count)
      return {};
    std::vector<Row> learning;
    for (std::uint32_t i = 0; i < reference_count; ++i)
      learning.push_back(values(reference_events[i]));
    if (mode == 1 && !matches(sink.snapshot(Use::learning), learning))
      return {};
    ayther_audio_event_clear_events(reference.get());
    std::array<AytherAudioActive, 64> active{};
    const auto count = session.audio_live_active(
        active.data(), static_cast<std::uint32_t>(active.size()));
    if (count > active.size())
      return {};
    std::vector<Row> rows;
    for (std::uint32_t i = 0; i < count; ++i)
      rows.push_back(values(active[i]));
    if (mode == 1 &&
        (!sink.valid || !matches(sink.snapshot(Use::selection), rows) ||
         !matches(sink.snapshot(Use::inspection), rows)))
      return {};
    result.active.push_back(std::move(rows));
  }
  if (!session.serialize(result.state))
    return {};
  session.record_stop();
  const auto recording = session.take_recording();
  if (recording.frame_count() != 8)
    return {};
  const auto count = session.analyze_audio_events(recording);
  const auto *events = session.audio_events();
  if (count != 0 && events == nullptr)
    return {};
  for (std::uint32_t i = 0; i < count; ++i)
    result.closed.push_back(values(events[i]));
  if (mode == 1 &&
      (!sink.valid || !matches(sink.snapshot(Use::analysis), result.closed) ||
       sink.active_rows == 0 || sink.closed_rows == 0 ||
       sink.learning_rows == 0 || result.closed.empty())) {
    std::cerr << "session trace invalid: active=" << sink.active_rows
              << " closed=" << sink.closed_rows
              << " analysis=" << result.closed.size()
              << " learning=" << sink.learning_rows << " valid=" << sink.valid
              << '\n';
    return {};
  }
  if (mode == 2 && !sink.lost)
    return {};
  if (mode == 1)
    std::cout << "Observed active rows=" << sink.active_rows
              << " live closed rows=" << sink.learning_rows
              << " analysis rows=" << result.closed.size() << '\n';
  session.reset();
  std::array<AytherAudioActive, 64> reset{};
  if (session.audio_live_active(reset.data(),
                                static_cast<std::uint32_t>(reset.size())) != 0)
    return {};
  if (mode == 1 && (!sink.valid || sink.snapshot(Use::inspection).count != 0))
    return {};
  return result;
}
} // namespace

int main() try {
  const auto plain = run(0);
  const auto observed = run(1);
  const auto lost = run(2);
  if (!plain || !observed || !lost || *plain != *observed || *plain != *lost) {
    std::cerr << "detector session equivalence failed\n";
    return 1;
  }
  std::cout << "Detector outputs and serialized game state match with "
               "observation on, off, and "
               "receiver loss\n";
  return 0;
} catch (...) {
  return 2;
}
