#include "audio_catalog_observation.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#if defined(QA_CATALOG_SESSION)
#include "../../../tools/common/synth_rom.h"
#include "trusted_pack_fixture.h"
#include <ayther/ayther_session.h>
#endif

namespace obs = ayther::engine::audio_observation;
namespace qa = ayther::audio_qa;
static_assert(sizeof(AytherAudioCatalogObservation) == 72);
static_assert(offsetof(AytherAudioCatalogObservation, signature_text) == 40);
static_assert(offsetof(AytherAudioCatalogObservation, asset_bytes) == 64);

namespace {
constexpr std::uint64_t absent = std::numeric_limits<std::uint64_t>::max();
struct Record {
  std::array<char, 64> kind{};
  std::array<char, 64> reason{};
  std::array<char, 64> stage{};
  std::array<char, 64> source{};
  std::array<char, 64> attempt_reason{};
  obs::FactId id;
  obs::FactId cause;
  std::uint64_t ordinal = absent;
  std::uint64_t duplicate = absent;
  bool accepted = false;
  bool attempted_now = false;
  bool ready = false;
  obs::Availability ready_availability = obs::Availability::unknown;
  std::array<obs::FactId, 2> causes{};
  std::size_t cause_count = 0;
};
const obs::FieldView *field(const obs::FactView &fact,
                            std::string_view name) noexcept {
  for (const auto &value : fact.fields)
    if (value.name == name)
      return &value;
  return nullptr;
}
template <typename T>
const T *value(const obs::FactView &fact, std::string_view name) noexcept {
  const auto *entry = field(fact, name);
  return entry ? std::get_if<T>(&entry->value) : nullptr;
}
void copy(std::string_view text, std::array<char, 64> &out) noexcept {
  const auto count = std::min(text.size(), out.size() - 1);
  std::copy_n(text.begin(), count, out.begin());
  out[count] = '\0';
}
struct Collector {
  std::array<Record, 32> records{};
  std::size_t count = 0;
  std::uint64_t declared = absent;
  std::uint64_t accepted = absent;
  std::uint64_t duplicates = absent;
  bool complete = false;
  bool limit_seen = false;
  std::uint64_t assignment_updates = absent;
  std::uint64_t distinct_assignments = absent;
  bool load_complete = false;
  static void receive(void *context, const obs::FactView &fact) noexcept {
    auto &self = *static_cast<Collector *>(context);
    if (self.count < self.records.size()) {
      auto &row = self.records[self.count];
      copy(fact.kind, row.kind);
      row.id = fact.id;
      row.cause_count = fact.causes.size();
      for (std::size_t i = 0;
           i < std::min(fact.causes.size(), row.causes.size()); ++i) {
        if (const auto *cause = std::get_if<obs::FactId>(&fact.causes[i]))
          row.causes[i] = *cause;
      }
      if (!fact.causes.empty()) {
        if (const auto *cause = std::get_if<obs::FactId>(&fact.causes[0]))
          row.cause = *cause;
      }
      if (const auto *number = value<std::uint64_t>(fact, "ordinal"))
        row.ordinal = *number;
      if (const auto *number =
              value<std::uint64_t>(fact, "duplicate_of_ordinal"))
        row.duplicate = *number;
      if (const auto *flag = value<bool>(fact, "accepted"))
        row.accepted = *flag;
      if (const auto *why = value<std::string_view>(fact, "reason"))
        copy(*why, row.reason);
      if (const auto *stage = value<std::string_view>(fact, "stage"))
        copy(*stage, row.stage);
      if (const auto *source = value<std::string_view>(fact, "source"))
        copy(*source, row.source);
      if (const auto *why = value<std::string_view>(fact, "attempt_reason"))
        copy(*why, row.attempt_reason);
      if (const auto *flag = value<bool>(fact, "attempted_now"))
        row.attempted_now = *flag;
      if (const auto *flag = value<bool>(fact, "ready"))
        row.ready = *flag;
      if (const auto *ready = field(fact, "ready"))
        row.ready_availability = ready->availability;
    }
    ++self.count;
    if (const auto *number = value<std::uint64_t>(fact, "declared_entries"))
      self.declared = *number;
    if (const auto *number =
            value<std::uint64_t>(fact, "parser_accepted_entries"))
      self.accepted = *number;
    if (const auto *number =
            value<std::uint64_t>(fact, "duplicate_declarations"))
      self.duplicates = *number;
    if (const auto *flag = value<bool>(fact, "inventory_complete"))
      self.complete = *flag;
    if (const auto *number = value<std::uint64_t>(fact, "assignment_updates"))
      self.assignment_updates = *number;
    if (const auto *number =
            value<std::uint64_t>(fact, "distinct_loaded_assignments"))
      self.distinct_assignments = *number;
    if (const auto *flag = value<bool>(fact, "load_observation_complete"))
      self.load_complete = *flag;
    for (const auto &item : fact.fields) {
      if (item.unavailable_reason == "text_limit" ||
          item.unavailable_reason == "signature_tracking_limit")
        self.limit_seen = true;
    }
  }
};

bool same_assignment(const AytherEventSub &left,
                     const AytherEventSub &right) noexcept {
  return left.signature == right.signature && left.channels == right.channels &&
         left.looping == right.looping &&
         left.duration_frames == right.duration_frames &&
         left.span_frames == right.span_frames &&
         left.match_instrument == right.match_instrument &&
         left.match_rule == right.match_rule &&
         left.match_pitch == right.match_pitch && left.bus == right.bus &&
         left._pad == right._pad &&
         std::ranges::equal(std::span{left.asset}, std::span{right.asset}) &&
         std::ranges::equal(std::span{left._pad2}, std::span{right._pad2});
}

bool actual_parser_inventory() {
  const char *text = "event=[{signature='0x1',asset='first'},{signature='0X01',"
                     "asset='second'},"
                     "{signature='bad!',asset='bad'},{signature='2'},7,{"
                     "signature='0',asset=''}]";
  qa::IdentitySource identities;
  Collector sink;
  auto recorder = std::make_unique<qa::CatalogRecorder>(
      obs::Observer{&sink, Collector::receive, nullptr}, identities,
      "provided_text");
  std::array<AytherEventSub, 3> actual{};
  std::array<AytherEventSub, 3> plain{};
  const auto count = ayther_audio_events_parse_observed(
      text, actual.data(), 3, qa::CatalogRecorder::callback, recorder.get());
  const auto ordinary = ayther_audio_events_parse(text, plain.data(), 3);
  if (count != 3 || ordinary != count ||
      !std::equal(actual.begin(), actual.end(), plain.begin(),
                  same_assignment) ||
      sink.count != 14 || sink.declared != 6 || sink.accepted != 3 ||
      sink.duplicates != 1 || !sink.complete || !recorder->complete())
    return false;
  for (std::size_t i = 0; i < 6; ++i) {
    const auto &declaration = sink.records[1 + i * 2];
    const auto &result = sink.records[2 + i * 2];
    if (std::string_view{declaration.kind.data()} !=
            "pack_assignment_declared" ||
        declaration.ordinal != i || result.ordinal != i ||
        result.cause != declaration.id ||
        declaration.cause != sink.records[0].id ||
        result.accepted != (i == 0 || i == 1 || i == 5))
      return false;
  }
  if (sink.records[3].duplicate != 0 ||
      std::string_view{sink.records[6].reason.data()} != "invalid_signature" ||
      std::string_view{sink.records[8].reason.data()} != "missing_asset")
    return false;

  // A short output buffer does not truncate observations or overwrite its
  // guard.
  Collector short_sink;
  auto short_recorder = std::make_unique<qa::CatalogRecorder>(
      obs::Observer{&short_sink, Collector::receive, nullptr}, identities,
      "provided_text");
  std::array<AytherEventSub, 2> short_output{};
  short_output[1].signature = 999;
  return ayther_audio_events_parse_observed(text, short_output.data(), 1,
                                            qa::CatalogRecorder::callback,
                                            short_recorder.get()) == 3 &&
         short_output[0].signature == 1 && short_output[1].signature == 999 &&
         short_sink.declared == 6;
}

bool empty_and_unavailable() {
  for (const char *text :
       std::array<const char *, 4>{"event=[]", "[[event", "", nullptr}) {
    qa::IdentitySource identities;
    Collector sink;
    auto recorder = std::make_unique<qa::CatalogRecorder>(
        obs::Observer{&sink, Collector::receive, nullptr}, identities,
        "provided_text");
    const auto count = ayther_audio_events_parse_observed(
        text, nullptr, 0, qa::CatalogRecorder::callback, recorder.get());
    if (count != 0)
      return false;
    const bool empty = text && std::string_view{text} == "event=[]";
    if (empty != recorder->complete() || empty != sink.complete ||
        (empty ? sink.declared != 0 : sink.declared != absent))
      return false;
  }
  return true;
}

bool bounded_tracking() {
  qa::IdentitySource identities;
  Collector sink;
  auto recorder = std::make_unique<qa::CatalogRecorder>(
      obs::Observer{&sink, Collector::receive, nullptr}, identities,
      "provided_text");
  AytherAudioCatalogObservation event{};
  event.kind = 1;
  event.count = 4097;
  qa::CatalogRecorder::callback(recorder.get(), &event);
  event.kind = 2;
  event.signature_known = 1;
  for (std::uint64_t i = 0; i < 4097; ++i) {
    event.ordinal = i;
    event.signature = i;
    qa::CatalogRecorder::callback(recorder.get(), &event);
    if (i == 4095 && !recorder->complete())
      return false;
  }
  if (recorder->complete() || !sink.limit_seen)
    return false;
  Collector text_sink;
  auto text_recorder = std::make_unique<qa::CatalogRecorder>(
      obs::Observer{&text_sink, Collector::receive, nullptr}, identities,
      "provided_text");
  const std::string text =
      "event=[{signature='1',asset='" + std::string(4097, 'a') + "'}]";
  const auto count = ayther_audio_events_parse_observed(
      text.c_str(), nullptr, 0, qa::CatalogRecorder::callback,
      text_recorder.get());
  return count == 1 && !text_recorder->complete() && text_sink.limit_seen &&
         !text_sink.complete;
}

bool bounded_load_links() {
  for (const std::size_t entries : {4096u, 4097u}) {
    std::string catalog = "event=[";
    for (std::size_t i = 0; i < entries; ++i)
      catalog += "{signature='1',asset='x'},";
    catalog += ']';
    qa::IdentitySource identities;
    Collector sink;
    auto recorder = std::make_unique<qa::CatalogRecorder>(
        obs::Observer{&sink, Collector::receive, nullptr}, identities,
        "provided_text");
    std::vector<AytherEventSub> assignments(entries);
    if (ayther_audio_events_parse_observed(catalog.c_str(), assignments.data(),
                                           static_cast<std::uint32_t>(entries),
                                           qa::CatalogRecorder::callback,
                                           recorder.get()) != entries)
      return false;
    sink = Collector{};
    if (!recorder->loaded(entries - 1, assignments.back(), false))
      return false;
    recorder->finish_load(1);
    const bool within_limit = entries == 4096;
    if (sink.assignment_updates != 1 || sink.distinct_assignments != 1 ||
        sink.load_complete != within_limit ||
        recorder->complete() != within_limit ||
        sink.records[0].ordinal != (within_limit ? entries - 1 : absent) ||
        sink.records[0].cause_count != (within_limit ? 1u : 0u))
      return false;
  }
  return true;
}

#if defined(QA_CATALOG_SESSION)
const Record *find_record(const Collector &sink, std::string_view kind,
                          std::uint64_t ordinal) {
  for (std::size_t i = 0; i < std::min(sink.count, sink.records.size()); ++i) {
    const auto &record = sink.records[i];
    if (std::string_view{record.kind.data()} == kind &&
        record.ordinal == ordinal)
      return &record;
  }
  return nullptr;
}

std::array<std::uint8_t, 60> known_wav() {
  return {'R',  'I',  'F', 'F', 52,   0,    0, 0, 'W', 'A', 'V', 'E',
          'f',  'm',  't', ' ', 16,   0,    0, 0, 1,   0,   2,   0,
          0x44, 0xAC, 0,   0,   0x10, 0xB1, 2, 0, 4,   0,   16,  0,
          'd',  'a',  't', 'a', 16,   0,    0, 0, 1,   0,   1,   0,
          2,    0,    2,   0,   3,    0,    3, 0, 4,   0,   4,   0};
}

bool assignment_load_stages() {
  ayther::test::TrustedPackFixture fixture{"qa_load_stages"};
  const std::string manifest =
      "[pack]\nname='QA load'\nversion='1.0.0'\ngame_id='audio-test'\n"
      "ayther_min='0.1.0'\n[regions]\ndefault='NTSC'\nsupported=['NTSC']\n";
  const std::string catalog =
      "event=[{signature='1',asset='ready.wav'},"
      "{signature='2',asset='qa046_missing_catalog_asset.wav'},"
      "{signature='3',asset='broken.wav'},{signature='invalid',asset='ready."
      "wav'},"
      "{signature='0x01',asset='ready.wav'},{signature='4',asset=''}]";
  const auto wav = known_wav();
  const std::array<std::uint8_t, 3> corrupt{'b', 'a', 'd'};
  if (!fixture.add_bytes(
          "manifest.toml",
          reinterpret_cast<const std::uint8_t *>(manifest.data()),
          manifest.size()) ||
      !fixture.add_bytes("audio_events.toml",
                         reinterpret_cast<const std::uint8_t *>(catalog.data()),
                         catalog.size()) ||
      !fixture.add_bytes("assets/ready.wav", wav.data(), wav.size()) ||
      !fixture.add_bytes("assets/broken.wav", corrupt.data(), corrupt.size()))
    return false;
  std::array<char, 512> error{};
  if (!fixture.finish(error.data(), error.size()))
    return false;
  Collector sink;
  ayther::AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = ayther::synth::canonical_rom_path();
  config.pack_path = fixture.pack_path();
  config.trust_registry = fixture.registry_path();
  config.enable_audio = false;
  config.derive_core_pack = false;
  config.audio_observer = {&sink, Collector::receive, nullptr};
  auto session = ayther::AytherSession::create(config);
  if (!session)
    return false;
  (*session)->load_audio_events_from_pack();
  if (sink.accepted != 5 || sink.assignment_updates != 5 ||
      sink.distinct_assignments != 4 || !sink.load_complete ||
      (*session)->audio_event_assignment_count() != 4) {
    std::fprintf(stderr,
                 "stages summary: accepted=%llu updates=%llu distinct=%llu "
                 "complete=%d map=%u\n",
                 static_cast<unsigned long long>(sink.accepted),
                 static_cast<unsigned long long>(sink.assignment_updates),
                 static_cast<unsigned long long>(sink.distinct_assignments),
                 sink.load_complete,
                 (*session)->audio_event_assignment_count());
    return false;
  }
  for (const auto ordinal : {0u, 1u, 2u, 4u, 5u}) {
    const auto *loaded = find_record(sink, "pack_assignment_loaded", ordinal);
    const auto *parsed =
        find_record(sink, "pack_assignment_parse_result", ordinal);
    const auto *asset =
        find_record(sink, "pack_assignment_asset_result", ordinal);
    if (!loaded || !parsed || !asset || !loaded->accepted ||
        loaded->cause != parsed->id || asset->cause != loaded->id ||
        std::string_view{loaded->stage.data()} != "session_assignment_map" ||
        std::string_view{asset->stage.data()} != "asset_prewarm")
      return false;
  }
  const auto *ready = find_record(sink, "pack_assignment_asset_result", 0);
  const auto *missing = find_record(sink, "pack_assignment_asset_result", 1);
  const auto *broken = find_record(sink, "pack_assignment_asset_result", 2);
  const auto *reused = find_record(sink, "pack_assignment_asset_result", 4);
  const auto *empty = find_record(sink, "pack_assignment_asset_result", 5);
  const auto *replacement = find_record(sink, "pack_assignment_loaded", 4);
  const auto *first = find_record(sink, "pack_assignment_loaded", 0);
  if (!ready || !missing || !broken || !reused || !empty || !replacement ||
      !first) {
    std::fprintf(stderr, "stages records missing\n");
    return false;
  }
  if (!ready->ready || !ready->attempted_now ||
      std::string_view{ready->source.data()} != "pack" || missing->ready ||
      missing->ready_availability != obs::Availability::known ||
      std::string_view{missing->reason.data()} != "missing" ||
      std::string_view{missing->source.data()} != "disk" || broken->ready ||
      std::string_view{broken->reason.data()} != "corrupt" || !reused->ready ||
      reused->attempted_now ||
      std::string_view{reused->attempt_reason.data()} != "already_prewarmed" ||
      empty->ready_availability != obs::Availability::unknown ||
      std::string_view{empty->attempt_reason.data()} != "empty_asset" ||
      replacement->cause_count != 2 || replacement->causes[1] != first->id ||
      find_record(sink, "pack_assignment_loaded", 3) != nullptr) {
    std::fprintf(
        stderr,
        "stages detail: ready=%d attempt=%d missing=%s broken=%s "
        "reused=%d reused_attempt=%d empty_avail=%u replacement_causes=%zu\n",
        ready->ready, ready->attempted_now, missing->reason.data(),
        broken->reason.data(), reused->ready, reused->attempted_now,
        static_cast<unsigned>(empty->ready_availability),
        replacement->cause_count);
    return false;
  }

  // Zero accepted entries still clear the map and publish its actual size.
  sink = Collector{};
  (*session)->load_audio_events_toml(
      "event=[{signature='invalid',asset='x'},{signature='1'}]");
  if (sink.accepted != 0 || sink.assignment_updates != 0 ||
      sink.distinct_assignments != 0 || !sink.load_complete ||
      (*session)->audio_event_assignment_count() != 0) {
    std::fprintf(stderr, "stages zero-accepted contract failed\n");
    return false;
  }
  const auto *rejected0 = find_record(sink, "pack_assignment_parse_result", 0);
  const auto *rejected1 = find_record(sink, "pack_assignment_parse_result", 1);
  if (!rejected0 || !rejected1 ||
      std::string_view{rejected0->reason.data()} != "invalid_signature" ||
      std::string_view{rejected1->reason.data()} != "missing_asset") {
    std::fprintf(stderr, "stages rejection reasons failed\n");
    return false;
  }
  sink = Collector{};
  (*session)->load_audio_events_toml("[[event");
  const bool malformed = sink.distinct_assignments == absent &&
                         !sink.complete && !sink.load_complete;
  if (!malformed)
    std::fprintf(stderr,
                 "stages malformed: distinct=%llu complete=%d load=%d\n",
                 static_cast<unsigned long long>(sink.distinct_assignments),
                 sink.complete, sink.load_complete);
  return malformed;
}

bool actual_session() {
  ayther::test::TrustedPackFixture fixture{"qa_catalog"};
  const char *manifest =
      "[pack]\nname='QA catalog'\nversion='1.0.0'\ngame_id='audio-test'\n"
      "ayther_min='0.1.0'\n[regions]\ndefault='NTSC'\nsupported=['NTSC']\n";
  const char *catalog = "event=[{signature='0x1',asset='first'},{signature='"
                        "0X01',asset='second'},"
                        "{signature='bad!',asset='bad'},{signature='2'},7,{"
                        "signature='0',asset=''}]";
  if (!fixture.add_bytes("manifest.toml",
                         reinterpret_cast<const std::uint8_t *>(manifest),
                         std::strlen(manifest)) ||
      !fixture.add_bytes("audio_events.toml",
                         reinterpret_cast<const std::uint8_t *>(catalog),
                         std::strlen(catalog)))
    return false;
  std::array<char, 512> error{};
  if (!fixture.finish(error.data(), error.size()))
    return false;
  Collector sink;
  ayther::AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = ayther::synth::canonical_rom_path();
  config.pack_path = fixture.pack_path();
  config.trust_registry = fixture.registry_path();
  config.enable_audio = false;
  config.derive_core_pack = false;
  config.audio_observer = {&sink, Collector::receive, nullptr};
  auto observed = ayther::AytherSession::create(config);
  if (!observed)
    return false;
  (*observed)->load_audio_events_from_pack();
  if (sink.declared != 6 || sink.accepted != 3 || sink.duplicates != 1 ||
      !sink.complete || (*observed)->audio_event_assignment_count() != 2 ||
      (*observed)->audio_event_asset(1) != "second")
    return false;
  // A repeated load is a new catalog instance, not a reset of producer IDs.
  const auto first_id = sink.records[0].id;
  sink = Collector{};
  (*observed)->load_audio_events_from_pack();
  if (sink.records[0].id.sequence <= first_id.sequence || sink.declared != 6)
    return false;
  const auto assignments = (*observed)->audio_event_assignment_count();
  (*observed).reset();
  config.audio_observer = {};
  auto ordinary = ayther::AytherSession::create(config);
  if (!ordinary)
    return false;
  (*ordinary)->load_audio_events_from_pack();
  return (*ordinary)->audio_event_assignment_count() == assignments &&
         (*ordinary)->audio_event_asset(1) == "second";
}
#endif
} // namespace

int main() {
  try {
    const bool parser = actual_parser_inventory();
    const bool empty = empty_and_unavailable();
    const bool tracking = bounded_tracking();
    const bool links = bounded_load_links();
    bool passed = parser && empty && tracking && links;
#if defined(QA_CATALOG_SESSION)
    const bool session = actual_session();
    const bool stages = assignment_load_stages();
    passed = session && stages && passed;
    if (!passed)
      std::fprintf(stderr,
                   "catalog checks: parser=%d empty=%d tracking=%d links=%d "
                   "session=%d stages=%d\n",
                   parser, empty, tracking, links, session, stages);
#endif
    return passed ? 0 : 1;
  } catch (...) {
    return 2;
  }
}
