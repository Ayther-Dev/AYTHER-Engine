#include "../../../tools/common/synth_rom.h"
#include <ayther/ayther_recording.h>
#include <ayther/ayther_session.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <thread>
#include <vector>

namespace obs = ayther::engine::audio_observation;
namespace {
template <class T>
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
bool cause(const obs::FactView &fact, std::size_t i,
           obs::FactId expected) noexcept {
  if (i >= fact.causes.size())
    return false;
  const auto *actual = std::get_if<obs::FactId>(&fact.causes[i]);
  return actual && *actual == expected;
}
bool write_wav(const std::filesystem::path &path, std::int16_t sample = 1000) {
  constexpr std::uint32_t frames = 8192;
  constexpr std::uint32_t channels = 2;
  constexpr std::uint32_t bytes_per_sample = sizeof(std::int16_t);
  constexpr std::uint32_t data_size = frames * channels * bytes_per_sample;
  constexpr std::uint32_t riff_size = 36 + data_size;
  std::array<std::uint8_t, 44> header{
      'R',
      'I',
      'F',
      'F',
      static_cast<std::uint8_t>(riff_size),
      static_cast<std::uint8_t>(riff_size >> 8),
      static_cast<std::uint8_t>(riff_size >> 16),
      static_cast<std::uint8_t>(riff_size >> 24),
      'W',
      'A',
      'V',
      'E',
      'f',
      'm',
      't',
      ' ',
      16,
      0,
      0,
      0,
      1,
      0,
      2,
      0,
      0x44,
      0xAC,
      0,
      0,
      0x10,
      0xB1,
      2,
      0,
      4,
      0,
      16,
      0,
      'd',
      'a',
      't',
      'a',
      static_cast<std::uint8_t>(data_size),
      static_cast<std::uint8_t>(data_size >> 8),
      static_cast<std::uint8_t>(data_size >> 16),
      static_cast<std::uint8_t>(data_size >> 24)};
  std::vector<std::int16_t> pcm(static_cast<std::size_t>(frames) * channels,
                                sample);
  std::ofstream file{path, std::ios::binary};
  file.write(reinterpret_cast<const char *>(header.data()), header.size());
  file.write(reinterpret_cast<const char *>(pcm.data()),
             static_cast<std::streamsize>(pcm.size() * sizeof(pcm[0])));
  return file.good();
}
struct Sink {
  static constexpr std::array<std::string_view, 8> use_names{
      "live_assignment",    "live_sequence_interest",
      "live_voice_route",   "replay_trigger",
      "replay_voice_route", "asset_coverage",
      "bare_frame_mute",    "export_audio"};
  std::array<obs::FactId, 64> active{};
  std::array<std::uint64_t, 64> signatures{};
  std::array<std::uint64_t, 128> closed{};
  std::array<std::uint64_t, 128> closed_start{};
  std::array<std::uint64_t, 128> closed_end{};
  obs::FactId closed_batch;
  obs::FactId query;
  obs::FactId probe;
  obs::FactId candidate;
  std::string_view detector_use;
  std::size_t queries = 0, exact = 0, broad = 0, absent = 0, replay = 0,
              sequence = 0;
  std::size_t candidate_decisions = 0, selections = 0, no_matches = 0;
  std::size_t playback_requests = 0, playback_starts = 0,
              playback_maintains = 0, playback_restarts = 0,
              playback_replacements = 0;
  std::size_t playback_effects = 0;
  std::size_t position_spans = 0;
  std::size_t mix_participants = 0;
  std::size_t original_audio_spans = 0;
  std::size_t frame_sample_boundaries = 0;
  std::atomic<std::uint64_t> main_output_frames{0};
  std::atomic<std::size_t> main_output_blocks{0};
  std::atomic<bool> main_output_valid{true};
  obs::FactId playback_request, playback_decision;
  obs::OccurrenceId playback_occurrence;
  struct OccurrenceRequest {
    std::uint64_t occurrence = 0;
    obs::FactId request;
  };
  std::array<OccurrenceRequest, 128> playback_requests_by_occurrence{};
  std::size_t playback_request_count = 0;
  std::array<std::size_t, use_names.size()> use_counts{};
  std::size_t calls = 0;
  bool drop = false;
  bool valid = true;
  void remember_playback_request(std::uint64_t occurrence,
                                 obs::FactId request) noexcept {
    const auto end = playback_requests_by_occurrence.begin() +
                     static_cast<std::ptrdiff_t>(playback_request_count);
    const auto existing =
        std::find_if(playback_requests_by_occurrence.begin(), end,
                     [occurrence](const OccurrenceRequest &entry) {
                       return entry.occurrence == occurrence;
                     });
    if (existing != end)
      return;
    if (playback_request_count == playback_requests_by_occurrence.size()) {
      valid = false;
      return;
    }
    playback_requests_by_occurrence[playback_request_count++] =
        OccurrenceRequest{occurrence, request};
  }
  const obs::FactId *
  playback_request_for(std::uint64_t occurrence) const noexcept {
    const auto end = playback_requests_by_occurrence.begin() +
                     static_cast<std::ptrdiff_t>(playback_request_count);
    const auto found =
        std::find_if(playback_requests_by_occurrence.begin(), end,
                     [occurrence](const OccurrenceRequest &entry) {
                       return entry.occurrence == occurrence;
                     });
    return found == end ? nullptr : &found->request;
  }
#if defined(QA_SEQUENCE_SESSION)
  std::size_t pack_queries = 0, authored_queries = 0, table_queries = 0;
  std::size_t visits = 0, transforms = 0;
  std::size_t sequence_decisions = 0, sequence_selections = 0;
  std::string_view sequence_use;
  struct Transformation {
    obs::FactId id;
    std::uint64_t original = 0, target = 0;
  };
  std::array<Transformation, 64> transformations{};
#endif
  static void receive(void *context, const obs::FactView &fact) noexcept {
    auto &s = *static_cast<Sink *>(context);
    if (s.drop && ++s.calls > 64)
      return;
#if defined(QA_SEQUENCE_SESSION)
    if (fact.kind == "sequence_query") {
      const auto use = text(fact, "use");
      s.sequence_use = use;
      if (use == "live_pack")
        ++s.pack_queries;
      if (use == "live_authored")
        ++s.authored_queries;
      if (use == "replay_table")
        ++s.table_queries;
    }
    if (fact.kind == "sequence_candidate_visit") {
      ++s.visits;
      const auto *complete = field<bool>(fact, "links_complete");
      if (!complete || !*complete)
        s.valid = false;
    }
    if (fact.kind == "sequence_candidate_decision") {
      ++s.sequence_decisions;
      const auto *complete = field<bool>(fact, "links_complete");
      if (!complete || !*complete || fact.causes.size() < 2)
        s.valid = false;
    }
    if (fact.kind == "sequence_selection") {
      ++s.sequence_selections;
      if (fact.causes.size() != 1)
        s.valid = false;
    }
    if (fact.kind == "sequence_input") {
      const auto *complete = field<bool>(fact, "provenance_complete");
      if (!complete || !*complete)
        s.valid = false;
      const auto index = number(fact, "source_index");
      const auto *source = field<obs::FactId>(fact, "source");
      if (s.sequence_use == "replay_table") {
        if (!source || *source != s.closed_batch || index >= s.closed.size() ||
            number(fact, "signature") != s.closed[index])
          s.valid = false;
      } else if (!source || index >= s.active.size() ||
                 *source != s.active[index] ||
                 number(fact, "source_signature") != s.signatures[index])
        s.valid = false;
      if (text(fact, "origin") == "resolved_assignment" ||
          text(fact, "origin") == "sequence_rule_trigger") {
        const auto *id = field<obs::FactId>(fact, "transformation");
        bool found = false;
        for (std::size_t i = 0;
             i < s.transforms && i < s.transformations.size(); ++i) {
          const auto &value = s.transformations[i];
          if (id && value.id == *id &&
              value.original == number(fact, "source_signature") &&
              value.target == number(fact, "signature"))
            found = true;
        }
        if (!found)
          s.valid = false;
      }
    }
    if (fact.kind == "sequence_signature_transform") {
      if (s.transforms < s.transformations.size())
        s.transformations[s.transforms] = {fact.id,
                                           number(fact, "original_signature"),
                                           number(fact, "target_signature")};
      else
        s.valid = false;
      ++s.transforms;
    }
#endif
    if (fact.kind == "detector_output_batch") {
      s.detector_use = text(fact, "use");
      if (s.detector_use == "runtime_selection")
        s.active = {};
      if (s.detector_use == "analysis_result") {
        s.closed_batch = fact.id;
        s.closed = {};
      }
    } else if (fact.kind == "detector_active_channel" &&
               s.detector_use == "runtime_selection") {
      const auto i = number(fact, "source_index");
      if (i >= s.active.size()) {
        s.valid = false;
        return;
      }
      s.active[i] = fact.id;
      s.signatures[i] = number(fact, "signature");
    } else if (fact.kind == "detector_closed_event" &&
               s.detector_use == "analysis_result") {
      const auto i = number(fact, "source_index");
      if (i >= s.closed.size()) {
        s.valid = false;
        return;
      }
      s.closed[i] = number(fact, "signature");
      s.closed_start[i] = number(fact, "start_frame");
      s.closed_end[i] = number(fact, "end_frame");
    } else if (fact.kind == "assignment_query") {
      ++s.queries;
      s.query = fact.id;
      const auto use = text(fact, "use");
      const auto use_it = std::find(use_names.begin(), use_names.end(), use);
      if (use_it == use_names.end())
        s.valid = false;
      else
        ++s.use_counts[static_cast<std::size_t>(use_it - use_names.begin())];
      const auto i = number(fact, "source_index");
      const auto kind = text(fact, "source_kind");
      const auto *complete = field<bool>(fact, "provenance_complete");
      if (!complete || !*complete || fact.causes.size() != 1)
        s.valid = false;
      if (kind == "detector_active_row") {
        if (i >= s.active.size() || !cause(fact, 0, s.active[i]) ||
            number(fact, "signature") != s.signatures[i])
          s.valid = false;
      } else if (kind == "detector_closed_batch") {
        if (i >= s.closed.size() || !cause(fact, 0, s.closed_batch) ||
            number(fact, "signature") != s.closed[i] ||
            number(fact, "event_start") != s.closed_start[i] ||
            number(fact, "event_end") != s.closed_end[i])
          s.valid = false;
      } else
        s.valid = false;
      if (text(fact, "use") == "replay_trigger") {
        if (number(fact, "evaluated_frame") != fact.frame.emulation_frame)
          s.valid = false;
        ++s.replay;
      }
      if (text(fact, "use") == "live_sequence_interest")
        ++s.sequence;
    } else if (fact.kind == "assignment_exact_probe") {
      s.probe = fact.id;
      if (!cause(fact, 0, s.query))
        s.valid = false;
      const auto *found = field<bool>(fact, "found");
      if (!found)
        s.valid = false;
      else if (!*found)
        ++s.absent;
    } else if (fact.kind == "assignment_candidate") {
      s.candidate = fact.id;
      if (!cause(fact, 0, s.query) || !cause(fact, 1, s.probe))
        s.valid = false;
      if (text(fact, "origin") == "exact_lookup")
        ++s.exact;
      else if (text(fact, "origin") == "instrument_index")
        ++s.broad;
      else
        s.valid = false;
    } else if (fact.kind == "assignment_candidate_decision") {
      ++s.candidate_decisions;
      if (!cause(fact, 0, s.query) || !cause(fact, 1, s.candidate) ||
          text(fact, "result").empty())
        s.valid = false;
    } else if (fact.kind == "assignment_selection") {
      ++s.selections;
      const bool selected = text(fact, "result") == "selected";
      s.no_matches += selected ? 0 : 1;
      if (!cause(fact, 0, s.query) || !cause(fact, 1, s.probe) ||
          fact.causes.size() != (selected ? 3 : 2))
        s.valid = false;
    } else if (fact.kind == "hd_playback_request") {
      ++s.playback_requests;
      s.playback_request = fact.id;
      const auto *occurrence = field<obs::OccurrenceId>(fact, "occurrence");
      if (!occurrence || occurrence->value == 0 || fact.causes.size() != 1)
        s.valid = false;
      else
        s.playback_occurrence = *occurrence;
      if (occurrence && occurrence->value != 0)
        s.remember_playback_request(occurrence->value, fact.id);
    } else if (fact.kind == "hd_playback_decision") {
      s.playback_decision = fact.id;
      s.playback_starts += text(fact, "action") == "start" ? 1 : 0;
      s.playback_maintains += text(fact, "action") == "maintain" ? 1 : 0;
      s.playback_restarts += text(fact, "action") == "restart" ? 1 : 0;
      s.playback_replacements += text(fact, "action") == "replace" ? 1 : 0;
      const auto *occurrence = field<obs::OccurrenceId>(fact, "occurrence");
      if (!cause(fact, 0, s.playback_request) || !occurrence ||
          *occurrence != s.playback_occurrence)
        s.valid = false;
      if (text(fact, "action") == "restart" ||
          text(fact, "action") == "replace") {
        const auto *previous =
            field<obs::OccurrenceId>(fact, "previous_occurrence");
        const auto *prior =
            previous ? s.playback_request_for(previous->value) : nullptr;
        if (!previous || previous->value == 0 || *previous == *occurrence ||
            !prior || fact.causes.size() != 2 || !cause(fact, 1, *prior))
          s.valid = false;
      } else if (fact.causes.size() != 1) {
        s.valid = false;
      }
    } else if (fact.kind == "hd_playback_effect") {
      ++s.playback_effects;
      const auto *occurrence = field<obs::OccurrenceId>(fact, "occurrence");
      if (!cause(fact, 0, s.playback_decision) || !occurrence ||
          *occurrence != s.playback_occurrence)
        s.valid = false;
    } else if (fact.kind == "hd_voice_position_span") {
      ++s.position_spans;
      const auto begin = number(fact, "output_begin");
      const auto end = number(fact, "output_end");
      const auto source_begin = number(fact, "source_begin");
      const auto source_end = number(fact, "source_end");
      const auto source_limit = number(fact, "source_limit");
      const auto *occurrence = field<obs::OccurrenceId>(fact, "occurrence");
      if (fact.causes.size() != 1 || !occurrence || occurrence->value == 0 ||
          begin >= end || source_begin >= source_end ||
          source_end > source_limit || end - begin != source_end - source_begin)
        s.valid = false;
    } else if (fact.kind == "hd_mix_participant") {
      ++s.mix_participants;
      const auto begin = number(fact, "mix_begin");
      const auto end = number(fact, "mix_end");
      const auto track_begin = number(fact, "track_begin");
      const auto track_end = number(fact, "track_end");
      const auto track_limit = number(fact, "track_limit");
      const auto *occurrence = field<obs::OccurrenceId>(fact, "occurrence");
      const auto *complete = field<bool>(fact, "links_complete");
      const auto *gain_begin = field<double>(fact, "effective_gain_begin");
      const auto *gain_end = field<double>(fact, "effective_gain_end");
      const auto *muted = field<bool>(fact, "muted_by_gain");
      const auto *nonzero = field<bool>(fact, "nonzero_contribution");
      if (fact.causes.size() != 1 || !occurrence || occurrence->value == 0 ||
          !complete || !*complete || begin >= end || track_begin >= track_end ||
          track_end > track_limit || end - begin != track_end - track_begin ||
          number(fact, "mix_sample_rate") != 44100 ||
          number(fact, "track_sample_rate") != 44100 ||
          text(fact, "mix_timeline") != "engine_main_mix" ||
          text(fact, "track_timeline") != "hd_asset_pcm" || !gain_begin ||
          !gain_end || !muted || !nonzero ||
          (*muted && (*gain_begin != 0.0 || *gain_end != 0.0 || *nonzero)))
        s.valid = false;
    } else if (fact.kind == "audio_frame_sample_boundary") {
      ++s.frame_sample_boundaries;
      if (fact.frame.availability != obs::Availability::known ||
          !fact.causes.empty() ||
          number(fact, "output_position") < number(fact, "staged_offset") ||
          number(fact, "sample_rate") != 44100)
        s.valid = false;
    } else if (fact.kind == "original_audio_span") {
      ++s.original_audio_spans;
      const auto begin = number(fact, "mix_begin");
      const auto end = number(fact, "mix_end");
      const auto boundary = number(fact, "frame_boundary");
      const auto state = text(fact, "state");
      const auto *complete = field<bool>(fact, "observation_complete");
      const auto *cause = fact.causes.size() == 1
                              ? std::get_if<obs::FactId>(&fact.causes.front())
                              : nullptr;
      if (fact.frame.availability != obs::Availability::known || !cause ||
          cause->producer != 6 || boundary > begin || begin >= end ||
          (state != "present" && state != "suppressed" && state != "absent") ||
          !complete || !*complete)
        s.valid = false;
    }
  }
  static void receive_pcm(void *context, const obs::PcmView &pcm) noexcept {
    auto &s = *static_cast<Sink *>(context);
    const auto expected_begin =
        s.main_output_frames.load(std::memory_order_relaxed);
    const auto expected_sequence =
        s.main_output_blocks.load(std::memory_order_relaxed) + 1;
    const auto frames =
        pcm.range.end > pcm.range.begin ? pcm.range.end - pcm.range.begin : 0;
    const bool valid =
        pcm.id.producer == 7 && pcm.id.sequence == expected_sequence &&
        pcm.capture_point == "sdl_logical_device_postmix" &&
        pcm.range.timeline == "engine_main_output" &&
        pcm.range.sample_rate == 44100 && pcm.range.begin == expected_begin &&
        frames > 0 && pcm.format == obs::PcmFormat::f32_le &&
        pcm.channels == 2 && pcm.bytes.size() == frames * 2 * sizeof(float) &&
        pcm.causes.empty();
    if (!valid)
      s.main_output_valid.store(false, std::memory_order_relaxed);
    s.main_output_frames.store(pcm.range.end, std::memory_order_relaxed);
    s.main_output_blocks.store(expected_sequence, std::memory_order_relaxed);
  }
};
struct Result {
  bool valid = false;
  std::vector<std::uint8_t> state;
  std::vector<std::uint16_t> inputs;
  std::vector<std::uint32_t> anchors;
  std::string mixdown;
  std::array<std::uint64_t, 15> decisions{};
};
Result run(unsigned mode) {
  Sink sink;
  sink.drop = mode == 2;
  ayther::synth::Rom rom("AYTHER QA DETECTOR 128");
  ayther::synth::program_canonical(rom, true);
  const auto path = std::filesystem::current_path() / "match-sequence.md";
  const auto wav_path = std::filesystem::current_path() / "observer.wav";
  const auto replacement_path =
      std::filesystem::current_path() / "observer-replacement.wav";
  if (!rom.save(path) || !write_wav(wav_path) ||
      !write_wav(replacement_path, 2000))
    return {};
  ayther::AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = path.string();
  config.enable_audio = true;
  config.derive_core_pack = false;
  if (mode)
    config.audio_observer = {&sink, Sink::receive, Sink::receive_pcm};
  auto created = ayther::AytherSession::create(config);
  if (!created)
    return {};
  auto &session = **created;
  session.set_audio_runtime_substitution(true);
  session.record_start();
  (void)session.step();
  std::array<AytherAudioActive, 64> active{};
  const auto count = session.audio_live_active(active.data(), 64);
  if (count == 0 || count > active.size() || active[0].instrument == 0)
    return {};
  for (std::uint32_t i = 0; i < count; ++i)
    session.assign_audio_event(active[i].signature, "");
  (void)session.step();
  const std::string catalog =
      "[[event]]\nsignature='1'\nasset='observer.wav'\nduration=8\nmatch='"
      "instrument'\ninstrument='0x" +
      [](std::uint64_t value) {
        std::array<char, 17> bytes{};
        (void)std::snprintf(bytes.data(), bytes.size(), "%016llx",
                            static_cast<unsigned long long>(value));
        return std::string{bytes.data()};
      }(active[0].instrument) +
      "'\n";
  session.load_audio_events_toml(catalog.c_str());
#if !defined(QA_SEQUENCE_SESSION)
  session.assign_audio_event(active[0].signature, wav_path.string().c_str());
#endif
  const ayther::AytherSession::InstrumentAssign instrument{
      active[0].instrument, "missing-observer.sf2", 0, 0, 0, 1.0F};
  session.set_instrument_assigns(&instrument, 1);
  session.set_audio_runtime_substitution(false);
  session.set_audio_runtime_substitution(true);
#if defined(QA_SEQUENCE_SESSION)
  ayther::AytherSession::AudioSeqSub authored;
  authored.key = 200;
  authored.trigger_signature = 2;
  authored.duration_frames = 8;
  authored.asset = "missing-sequence.wav";
  authored.signatures = {active[0].signature};
  authored.head_signatures = {active[0].signature};
  authored.match_rule = ayther::AudioMatchRule::kInstrument;
  authored.match_instrument = active[0].instrument;
  // One sub exercises the rule conversion; another supplies real replay
  // anchors.
  auto exact = authored;
  exact.key = 201;
  exact.trigger_signature = active[0].signature;
  exact.match_rule = ayther::AudioMatchRule::kExact;
  authored.head_signatures.clear();
  session.set_audio_sequence_subs({authored, exact});
#endif
  for (unsigned i = 0; i < 6; ++i) {
#if !defined(QA_SEQUENCE_SESSION)
    if (i == 2 || i == 4)
      session.clear_audio_event_assignments();
#endif
    (void)session.step();
#if !defined(QA_SEQUENCE_SESSION)
    if (i == 2 || i == 4) {
      session.load_audio_events_toml(catalog.c_str());
      session.assign_audio_event(active[0].signature,
                                 i == 2 ? wav_path.string().c_str()
                                        : replacement_path.string().c_str());
    }
#endif
  }
  session.record_stop();
  const auto recording = session.take_recording();
  if (session.analyze_audio_events(recording) == 0)
    return {};
#if defined(QA_SEQUENCE_SESSION)
  const auto anchors = session.audio_seq_anchors(201);
  const auto queries = sink.table_queries;
  if (anchors.empty() || session.audio_seq_anchors(201) != anchors ||
      sink.table_queries != queries)
    return {};
#endif
  session.set_audio_runtime_substitution(false);
  session.set_audio_substitution_preview(true);
  for (std::uint32_t i = 0; i < recording.frame_count(); ++i)
    if (!session.replay_seek(recording, i))
      return {};
  session.set_voice_router(false);
  if (!session.replay_seek(recording, 0) ||
      !session.replay_seek(recording, recording.frame_count() - 1))
    return {};
  const auto mix_path = std::filesystem::current_path() /
                        ("match-policy-" + std::to_string(mode) + ".wav");
  if (!session.export_mixdown_wav(recording, 0, recording.frame_count(),
                                  mix_path.string().c_str(), true))
    return {};
  std::ifstream mix_file{mix_path, std::ios::binary};
  const std::string mixdown{std::istreambuf_iterator<char>{mix_file},
                            std::istreambuf_iterator<char>{}};
  std::error_code remove_error;
  (void)std::filesystem::remove(mix_path, remove_error);
  if (mixdown.empty())
    return {};
  if (mode) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (sink.main_output_blocks.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Result result;
#if defined(QA_SEQUENCE_SESSION)
  result.anchors = anchors;
#endif
  if (!session.serialize(result.state))
    return {};
  result.inputs = recording.inputs;
  result.mixdown = mixdown;
  session.audio_live_match_stats(&result.decisions[0], &result.decisions[1],
                                 &result.decisions[2], &result.decisions[3]);
  session.audio_unified_stats(&result.decisions[4], &result.decisions[5],
                              &result.decisions[6], &result.decisions[7]);
  session.audio_fallback_stats(&result.decisions[8], &result.decisions[9]);
  session.audio_resume_stats(&result.decisions[10], &result.decisions[11],
                             &result.decisions[12]);
  result.decisions[13] = session.audio_event_count();
  result.decisions[14] = session.audio_event_assignment_count();
#if defined(QA_SEQUENCE_SESSION)
  constexpr std::array<std::size_t, Sink::use_names.size()> expected_uses{
      32, 28, 2, 30, 10, 18, 42, 10};
  constexpr std::size_t expected_queries = 172;
  constexpr std::size_t expected_exact = 2, expected_broad = 112,
                        expected_absent = 170;
  constexpr std::size_t expected_candidate_decisions = 112;
  const bool playback_valid =
      sink.playback_requests == 26 && sink.playback_starts == 5 &&
      sink.playback_maintains == 13 && sink.playback_restarts == 8 &&
      sink.playback_replacements == 0 &&
      sink.playback_effects == sink.playback_requests &&
      sink.position_spans > 0 && sink.mix_participants == sink.position_spans &&
      sink.frame_sample_boundaries > 0 && sink.original_audio_spans > 0 &&
      sink.main_output_valid.load(std::memory_order_relaxed) &&
      sink.main_output_blocks.load(std::memory_order_relaxed) > 0 &&
      sink.main_output_frames.load(std::memory_order_relaxed) > 0;
#else
  constexpr std::array<std::size_t, Sink::use_names.size()> expected_uses{
      32, 24, 2, 30, 10, 18, 42, 10};
  constexpr std::size_t expected_queries = 168;
  constexpr std::size_t expected_exact = 54, expected_broad = 56,
                        expected_absent = 114;
  constexpr std::size_t expected_candidate_decisions = 56;
  const bool playback_valid =
      sink.playback_requests == 24 && sink.playback_starts == 5 &&
      sink.playback_maintains == 9 && sink.playback_restarts == 9 &&
      sink.playback_replacements == 1 &&
      sink.playback_effects == sink.playback_requests &&
      sink.position_spans > 0 && sink.mix_participants == sink.position_spans &&
      sink.frame_sample_boundaries > 0 && sink.original_audio_spans > 0 &&
      sink.main_output_valid.load(std::memory_order_relaxed) &&
      sink.main_output_blocks.load(std::memory_order_relaxed) > 0 &&
      sink.main_output_frames.load(std::memory_order_relaxed) > 0;
#endif
  result.valid =
      mode != 1 ||
      (sink.valid && sink.queries == expected_queries &&
       sink.exact == expected_exact && sink.broad == expected_broad &&
       sink.absent == expected_absent && sink.replay == 30 &&
       sink.sequence == expected_uses[1] &&
       sink.candidate_decisions == expected_candidate_decisions &&
       sink.selections == expected_queries && sink.no_matches == 58 &&
       playback_valid && sink.use_counts == expected_uses);
#if defined(QA_SEQUENCE_SESSION)
  result.valid =
      result.valid &&
      (mode != 1 ||
       (sink.pack_queries == 14 && sink.authored_queries == 16 &&
        sink.table_queries == 8 && sink.visits == 26 && sink.transforms == 2 &&
        sink.sequence_selections == 38 && sink.sequence_decisions == 4));
  if (mode == 1)
    std::cout << "pack_queries=" << sink.pack_queries
              << " authored_queries=" << sink.authored_queries
              << " table_queries=" << sink.table_queries
              << " visits=" << sink.visits << " transforms=" << sink.transforms
              << " sequence_decisions=" << sink.sequence_decisions
              << " sequence_selections=" << sink.sequence_selections << '\n';
#endif
  if (mode == 1)
    std::cout << "queries=" << sink.queries << " exact=" << sink.exact
              << " broad=" << sink.broad << " absent=" << sink.absent
              << " replay=" << sink.replay << " sequence=" << sink.sequence
              << " candidate_decisions=" << sink.candidate_decisions
              << " selections=" << sink.selections
              << " no_matches=" << sink.no_matches
              << " playback_requests=" << sink.playback_requests
              << " playback_starts=" << sink.playback_starts
              << " playback_maintains=" << sink.playback_maintains
              << " playback_restarts=" << sink.playback_restarts
              << " playback_replacements=" << sink.playback_replacements
              << " playback_effects=" << sink.playback_effects
              << " position_spans=" << sink.position_spans
              << " mix_participants=" << sink.mix_participants
              << " frame_sample_boundaries=" << sink.frame_sample_boundaries
              << " original_audio_spans=" << sink.original_audio_spans
              << " main_output_blocks="
              << sink.main_output_blocks.load(std::memory_order_relaxed)
              << " main_output_frames="
              << sink.main_output_frames.load(std::memory_order_relaxed)
              << " valid=" << sink.valid << '\n';
  if (mode == 1) {
    std::cout << "uses";
    for (std::size_t i = 0; i < Sink::use_names.size(); ++i)
      std::cout << ' ' << Sink::use_names[i] << '=' << sink.use_counts[i];
    std::cout << '\n';
  }
  session.clear_audio_events();
  session.reset();
  return result;
}
} // namespace
int main() {
  try {
    const auto plain = run(0), observed = run(1), dropped = run(2);
    return plain.valid && observed.valid && dropped.valid &&
                   plain.state == observed.state &&
                   plain.state == dropped.state &&
                   plain.inputs == observed.inputs &&
                   plain.inputs == dropped.inputs &&
                   plain.anchors == observed.anchors &&
                   plain.anchors == dropped.anchors &&
                   plain.decisions == observed.decisions &&
                   plain.decisions == dropped.decisions &&
                   plain.mixdown == observed.mixdown &&
                   plain.mixdown == dropped.mixdown
               ? 0
               : 1;
  } catch (...) {
    return 2;
  }
}
