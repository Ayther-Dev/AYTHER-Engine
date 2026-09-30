#include <ayther/engine/audio_observer.hpp>

#include <ayther/audio_match_rule.h>

#include <algorithm>
#include <array>
#include <string>

namespace obs = ayther::engine::audio_observation;

namespace {
// A bounded test receiver, not the production Runtime bridge. Its views are
// rebound to its own fixed storage. Keeping this owner stationary is required.
struct Snapshot {
  Snapshot() = default;
  Snapshot(const Snapshot &) = delete;
  Snapshot &operator=(const Snapshot &) = delete;
  Snapshot(Snapshot &&) = delete;
  Snapshot &operator=(Snapshot &&) = delete;
  ~Snapshot() = default;

  std::array<char, 2048> text{};
  std::size_t used = 0;
  std::array<obs::Cause, 4> causes{};
  std::array<obs::FieldView, 4> fields{};
  std::array<obs::StateOrder, 4> orders{};
  obs::FactView fact;
  std::array<std::byte, 64> samples{};
  std::array<char, 64> timeline{};
  std::array<char, 64> capture_point{};
  std::array<obs::Cause, 4> pcm_causes{};
  std::array<char, 64> pcm_context{};
  obs::PcmView pcm;
  unsigned received = 0;
  unsigned lost = 0;
  bool valid = true;

  std::string_view copy(std::string_view value) noexcept {
    if (value.size() > text.size() - used) {
      valid = false;
      return {};
    }
    const auto start = used;
    std::copy(value.begin(), value.end(),
              text.begin() + static_cast<std::ptrdiff_t>(used));
    used += value.size();
    return {text.data() + start, value.size()};
  }

  static void receive(void *context, const obs::FactView &source) noexcept {
    auto &self = *static_cast<Snapshot *>(context);
    if (source.causes.size() > self.causes.size() ||
        source.fields.size() > self.fields.size() ||
        source.state_orders.size() > self.orders.size()) {
      ++self.lost;
      return;
    }
    self.used = 0;
    self.fact = source;
    self.fact.kind = self.copy(source.kind);
    self.fact.frame.unavailable_reason =
        self.copy(source.frame.unavailable_reason);
    for (std::size_t i = 0; i < source.causes.size(); ++i) {
      self.causes[i] = source.causes[i];
      if (auto *initial =
              std::get_if<obs::PreexistingContext>(&self.causes[i])) {
        initial->state_id = self.copy(initial->state_id);
      }
    }
    for (std::size_t i = 0; i < source.fields.size(); ++i) {
      self.fields[i] = source.fields[i];
      auto &field = self.fields[i];
      field.name = self.copy(field.name);
      field.unavailable_reason = self.copy(field.unavailable_reason);
      if (auto *value = std::get_if<std::string_view>(&field.value)) {
        *value = self.copy(*value);
      }
    }
    for (std::size_t i = 0; i < source.state_orders.size(); ++i) {
      self.orders[i] = source.state_orders[i];
      self.orders[i].state_id = self.copy(self.orders[i].state_id);
    }
    self.fact.causes = std::span{self.causes}.first(source.causes.size());
    self.fact.fields = std::span{self.fields}.first(source.fields.size());
    self.fact.state_orders =
        std::span{self.orders}.first(source.state_orders.size());
    if (self.valid) {
      ++self.received;
    } else {
      ++self.lost;
    }
  }

  // This fixture accepts one preexisting PCM cause at most. The public
  // contract supports more; production aggregate limits are tested later.
  static void audio(void *context, const obs::PcmView &source) noexcept {
    auto &self = *static_cast<Snapshot *>(context);
    if (source.bytes.size() > self.samples.size() ||
        source.range.timeline.size() > self.timeline.size() ||
        source.capture_point.size() > self.capture_point.size() ||
        source.causes.size() > 1) {
      ++self.lost;
      return;
    }
    if (!source.causes.empty()) {
      self.pcm_causes[0] = source.causes[0];
      if (auto *initial =
              std::get_if<obs::PreexistingContext>(&self.pcm_causes[0])) {
        if (initial->state_id.size() > self.pcm_context.size()) {
          ++self.lost;
          return;
        }
        std::copy(initial->state_id.begin(), initial->state_id.end(),
                  self.pcm_context.begin());
        initial->state_id = {self.pcm_context.data(), initial->state_id.size()};
      }
    }
    std::copy(source.bytes.begin(), source.bytes.end(), self.samples.begin());
    std::copy(source.range.timeline.begin(), source.range.timeline.end(),
              self.timeline.begin());
    std::copy(source.capture_point.begin(), source.capture_point.end(),
              self.capture_point.begin());
    self.pcm = source;
    self.pcm.bytes = std::span{self.samples}.first(source.bytes.size());
    self.pcm.range.timeline = {self.timeline.data(),
                               source.range.timeline.size()};
    self.pcm.capture_point = {self.capture_point.data(),
                              source.capture_point.size()};
    self.pcm.causes = std::span{self.pcm_causes}.first(source.causes.size());
  }
};

bool survives_mutation() {
  Snapshot snapshot;
  const obs::Observer observer{&snapshot, Snapshot::receive, Snapshot::audio};
  std::array<std::string, 8> texts{"voice_started", "track",        "music",
                                   "position",      "not_observed", "initial",
                                   "mixer",         "outside_step"};
  std::array<obs::Cause, 2> causes{obs::FactId{2, 4},
                                   obs::PreexistingContext{texts[5]}};
  std::array orders{obs::StateOrder{texts[6], 5}};
  std::array fields{obs::FieldView{texts[1],
                                   obs::Availability::known,
                                   obs::Unit::none,
                                   std::string_view{texts[2]},
                                   {}},
                    obs::FieldView{texts[3], obs::Availability::unknown,
                                   obs::Unit::sample_frame, std::monostate{},
                                   texts[4]}};
  const obs::FactView fact{
      {1, 6}, texts[0], {obs::Availability::unknown, 0, texts[7]},
      causes, orders,   fields};
  observer.observe(fact);
  std::array<std::byte, 4> bytes{std::byte{1}, std::byte{2}, std::byte{3},
                                 std::byte{4}};
  std::string timeline = "output";
  std::string capture = "postmix";
  observer.observe(obs::PcmView{{3, 1},
                                capture,
                                {timeline, 44100, 0, 1},
                                obs::PcmFormat::s16_le,
                                2,
                                bytes,
                                std::span{causes}.subspan(1)});

  for (auto &text : texts) {
    std::fill(text.begin(), text.end(), 'x');
  }
  std::fill(timeline.begin(), timeline.end(), 'x');
  std::fill(capture.begin(), capture.end(), 'x');
  causes.fill(obs::FactId{9, 9});
  orders.fill({"changed", 99});
  fields.fill(
      {"changed", obs::Availability::known, obs::Unit::none, false, {}});
  bytes.fill(std::byte{0});

  const auto *track =
      std::get_if<std::string_view>(&snapshot.fact.fields[0].value);
  const auto *initial =
      std::get_if<obs::PreexistingContext>(&snapshot.fact.causes[1]);
  const auto *pcm_initial =
      std::get_if<obs::PreexistingContext>(&snapshot.pcm.causes[0]);
  return snapshot.valid && snapshot.received == 1 && snapshot.lost == 0 &&
         snapshot.fact.kind == "voice_started" &&
         snapshot.fact.frame.unavailable_reason == "outside_step" &&
         snapshot.fact.fields[0].name == "track" && track != nullptr &&
         *track == "music" &&
         snapshot.fact.fields[1].unavailable_reason == "not_observed" &&
         initial != nullptr && initial->state_id == "initial" &&
         snapshot.fact.state_orders[0].state_id == "mixer" &&
         snapshot.fact.state_orders[0].sequence == 5 &&
         snapshot.pcm.range.timeline == "output" &&
         snapshot.pcm.capture_point == "postmix" && pcm_initial != nullptr &&
         pcm_initial->state_id == "initial" &&
         snapshot.pcm.bytes[0] == std::byte{1} &&
         snapshot.pcm.bytes[3] == std::byte{4};
}

struct Decision {
  bool matched = false;
  std::uint64_t assignment = 0;
  friend bool operator==(Decision, Decision) = default;
};

Decision select(const ayther::AudioMatchIndex &index, std::uint64_t instrument,
                std::uint8_t pitch, obs::Observer observer,
                std::uint64_t sequence) {
  Decision result;
  result.matched = index.resolve(instrument, pitch, &result.assignment);
  const std::array fields{obs::FieldView{"matched",
                                         obs::Availability::known,
                                         obs::Unit::none,
                                         result.matched,
                                         {}},
                          obs::FieldView{"assignment",
                                         obs::Availability::known,
                                         obs::Unit::none,
                                         result.assignment,
                                         {}}};
  observer.observe(obs::FactView{{1, sequence},
                                 "selection",
                                 {obs::Availability::known, 0, {}},
                                 {},
                                 {},
                                 fields});
  return result;
}

bool preserves_decisions() {
  ayther::AudioMatchIndex index;
  index.add(40, ayther::AudioMatchRule::kInstrument, 7, 60);
  index.add(30, ayther::AudioMatchRule::kInstrument, 7, 60);
  index.add(20, ayther::AudioMatchRule::kInstrumentPitch, 7, 60);
  index.add(10, ayther::AudioMatchRule::kInstrumentPitch, 7, 60);
  Snapshot snapshot;
  const obs::Observer enabled{&snapshot, Snapshot::receive, nullptr};
  struct Input {
    std::uint64_t instrument;
    std::uint8_t pitch;
    Decision expected;
  };
  const std::array cases{Input{7, 60, {true, 10}}, Input{7, 61, {true, 30}},
                         Input{8, 60, {false, 0}}, Input{0, 60, {false, 0}}};
  std::uint64_t sequence = 1;
  for (const auto &input : cases) {
    const auto plain =
        select(index, input.instrument, input.pitch, {}, sequence);
    const auto observed =
        select(index, input.instrument, input.pitch, enabled, sequence++);
    if (plain != input.expected || observed != plain) {
      return false;
    }
  }
  // Receiver saturation has no return channel to the selection rule.
  const obs::Observer dropping{
      &snapshot,
      [](void *context, const obs::FactView &) noexcept {
        ++static_cast<Snapshot *>(context)->lost;
      },
      nullptr};
  const auto dropped = select(index, 7, 60, dropping, sequence);
  const auto detached = select(index, 7, 60, {}, sequence);
  return snapshot.valid && snapshot.received == cases.size() &&
         snapshot.lost == 1 && dropped == cases[0].expected &&
         detached == dropped;
}
} // namespace

int main() {
  try {
    return survives_mutation() && preserves_decisions() ? 0 : 1;
  } catch (...) {
    return 2;
  }
}
