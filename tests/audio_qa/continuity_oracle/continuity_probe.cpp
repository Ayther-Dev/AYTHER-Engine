#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../include/ayther/audio_seq_anchor.h"

namespace qa_continuity {
struct Sample {
  std::uint64_t occurrence{};
  std::uint64_t key{};
  std::uint64_t output{};
  std::size_t source{};
  std::size_t next_source{};
  float gain{};
  std::uint32_t fade_left{};
};
struct Gate {
  const char *kind{};
  std::uint64_t frame{};
};
std::array<Sample, 128> samples{};
std::size_t sample_count{};
std::array<Gate, 8> gates{};
std::size_t gate_count{};
bool overflow{};
bool lose{};
ayther::SeqTriggerDisposition route = ayther::SeqTriggerDisposition::individual;
std::uint64_t owner_key{};
void sample(Sample value) noexcept {
  if (sample_count == samples.size()) {
    overflow = true;
    return;
  }
  samples[sample_count++] = value;
}
void gate(const char *kind, std::uint64_t frame) noexcept {
  if (lose && frame == 3) {
    return;
  }
  if (gate_count == gates.size()) {
    overflow = true;
    return;
  }
  gates[gate_count++] = {kind, frame};
}
void require(bool valid, const char *message) {
  if (!valid) {
    throw std::runtime_error(message);
  }
}
} // namespace qa_continuity

#if QA_CONTINUITY_OBSERVER
#include <observed_mixer.h>
#else
#include <audio_hd_mixer.h>
#endif

// The decoded-asset boundary supplies fixed PCM; no filesystem or decoder is
// simulated.
class TestPlayer {
public:
  struct WavEntry {
    std::array<std::uint8_t, 1> pcm{1};
  };
  TestPlayer() {
    auto pcm = std::make_shared<std::vector<std::int16_t>>(256);
    for (std::size_t i = 0; i < pcm->size(); ++i) {
      (*pcm)[i] = static_cast<std::int16_t>(1000 + i * 37);
    }
    pcm_ = pcm;
  }
  bool play_oneshot_asset_file(const std::string &path, std::uint64_t key,
                               double offset_seconds = 0, float gain = 1,
                               bool preview = false);
  const WavEntry *get_wav_disk(const std::string &path) const {
    qa_continuity::require(path == "fixture.wav", "unexpected_asset");
    return &wav_;
  }
  HdMixPcm get_mix_pcm(const WavEntry *wav, const std::string &path) const {
    qa_continuity::require(wav == &wav_ && path == "fixture.wav",
                           "unexpected_decoded_input");
    return pcm_;
  }
  bool device_{true};
  std::uint64_t timeline_samples_{};
  std::size_t frame_mark_{};
  HdMixer hd_mixer_;

private:
  WavEntry wav_;
  HdMixPcm pcm_;
};

#include <player.inc>

struct Impl {
#include <instance.inc>
  bool audio_enabled{true};
  bool transport_playing{true};
  bool audio_live_bypass{};
  std::uint64_t frame_index{};
  std::uint64_t hd_claimed{};
  std::uint64_t hd_fallback{};
  std::unordered_set<std::uint64_t> audio_live_prev;
  std::unordered_set<std::uint64_t> hd_failed_keys;
  std::unordered_map<std::uint64_t, std::uint32_t> audio_event_duration;
  std::unordered_map<std::uint64_t, LiveInstance> audio_live_inst;
  TestPlayer audio;
#include <fired.inc>
};

void process(Impl &im) {
  const std::uint64_t sig = 1000;
  const std::uint64_t asig = 1000;
  const std::uint32_t ev_bit = 1;
  const bool can = true;
  const std::unordered_map<std::uint64_t, std::string> catalog{
      {asig, "fixture.wav"}};
  const auto it = catalog.find(asig);
  const std::unordered_set<std::uint64_t> anchor_now, opened_now;
  std::unordered_map<std::uint64_t, ayther::SeqTriggerResolution>
      sequence_routes;
  if (qa_continuity::route != ayther::SeqTriggerDisposition::individual) {
    sequence_routes.emplace(
        asig, ayther::SeqTriggerResolution{qa_continuity::route, asig, asig,
                                           qa_continuity::owner_key, 0});
  }
  const auto open_seq_window = [](std::uint64_t, std::uint64_t, std::uint32_t,
                                  const std::string &, std::uint32_t) {
    throw std::runtime_error("sequence_window_outside_fixture");
  };
#if QA_CONTINUITY_CURRENT
#include <current_gate.inc>
#elif QA_CONTINUITY_OBSERVER
#include <observed_gate.inc>
#else
#include <reference_gate.inc>
#endif
}

int main(int argc, char *argv[]) {
  using qa_continuity::require;
  try {
    require(argc == 3, "invalid_arguments");
    const std::filesystem::path directory{argv[1]};
    const std::string_view mode{argv[2]};
    require(mode == "keep" || mode == "repeat" || mode == "lost" ||
                mode == "internal" || mode == "claimed" ||
                mode == "internal_repeat" || mode == "claimed_repeat",
            "invalid_case");
    for (const char *name : {"output.s16le", "run.toml", "trace.toml"}) {
      require(!std::filesystem::exists(directory / name), "output_exists");
    }
    const bool repeated = mode != "keep";
    qa_continuity::lose = mode == "lost";
    Impl im;
    if (mode == "internal" || mode == "claimed" || mode == "internal_repeat" ||
        mode == "claimed_repeat") {
      // A03: the sequence resolver rejected this trigger as internal to an
      // already-open step. The unchanged historical consumer receives the
      // real duration/catalogue state and an empty anchor set. It must stop
      // the rejected route instead of treating it as a free one-shot.
      std::uint64_t owner_key = 0;
      const bool claimed_mode = mode == "claimed" || mode == "claimed_repeat";
      const std::size_t attempts = mode.ends_with("_repeat") ? 8U : 1U;
      if (claimed_mode) {
        ayther::SeqAnchorSub owner;
        owner.key = 2000;
        owner.trigger_signature = 2000;
        owner.duration_frames = 100;
        owner.signatures = {2000, 1000};
        ayther::SeqAnchorSub candidate;
        candidate.key = 1000;
        candidate.trigger_signature = 1000;
        candidate.duration_frames = 64;
        candidate.signatures = {1000};
        const std::vector<ayther::SeqAnchorSub> subs{owner, candidate};
        std::vector<ayther::SeqAnchorState> states(subs.size());
        states[0].next_free = 100;
        states[0].win_start = 0;
        states[0].win_end = 100;
        states[0].open = true;
        ayther::SeqAnchorDecisionView verdict;
        bool decided = false;
        const auto selected = ayther::seq_anchor_frame_decided(
            10, {1000}, subs, states,
            [](const ayther::SeqAnchorCandidateView &) noexcept {},
            [&](const ayther::SeqAnchorDecisionView &value) noexcept {
              verdict = value;
              decided = true;
            },
            [](const ayther::SeqAnchorResolutionView &) noexcept {});
        require(
            selected.empty() && decided && verdict.sub_index == 1 &&
                verdict.result ==
                    ayther::SeqAnchorDecisionResult::claimed_by_open_sequence &&
                verdict.related_sub_index == 0,
            "claimed_fixture_not_rejected");
        owner_key = subs[verdict.related_sub_index].key;
      }
      qa_continuity::route =
          claimed_mode ? ayther::SeqTriggerDisposition::rejected_claimed
                       : ayther::SeqTriggerDisposition::rejected_internal;
      qa_continuity::owner_key = owner_key;
      im.audio_event_duration.emplace(1000, 64);
      for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
        im.frame_index = attempt;
        process(im);
      }
      const auto instance = im.audio_live_inst.find(1000);
      std::fprintf(
          stderr,
          "%s input_signature=1000 duration=64 anchor_count=0 "
          "owner_key=%llu "
          "claimed=%llu instances=%zu voices_started=%llu "
          "voices_active=%zu instance_key=%llu cursor_frame=%llu\n",
          claimed_mode ? "A04" : "A03",
          static_cast<unsigned long long>(owner_key),
          static_cast<unsigned long long>(im.hd_claimed),
          im.audio_live_inst.size(),
          static_cast<unsigned long long>(im.audio.hd_mixer_.started()),
          im.audio.hd_mixer_.voice_count(),
          static_cast<unsigned long long>(
              instance == im.audio_live_inst.end() ? 0 : instance->first),
          static_cast<unsigned long long>(instance == im.audio_live_inst.end()
                                              ? 0
                                              : instance->second.start_frame));
      require(im.hd_claimed == attempts,
              claimed_mode ? "claimed_rejection_not_observed"
                           : "internal_rejection_not_observed");
      require(im.audio_live_inst.empty(),
              claimed_mode ? "claimed_rejection_created_instance"
                           : "internal_rejection_created_instance");
      require(im.audio.hd_mixer_.started() == 0,
              claimed_mode ? "claimed_rejection_created_voice"
                           : "internal_rejection_created_voice");
      return 0;
    }
    std::array<std::int16_t, 128> output{};
    output.fill(200);
    struct Frame {
      bool input{};
      bool previous{};
      std::uint64_t started{};
      std::size_t voices{};
      std::uint64_t anchor{};
    };
    std::array<Frame, 4> frames{};
    for (std::size_t f = 0; f < frames.size(); ++f) {
      im.frame_index = f;
      const bool active = !(repeated && f == 2);
      const bool previous = im.audio_live_prev.contains(1000);
      std::unordered_set<std::uint64_t> now;
      if (active) {
        now.insert(1000);
        process(im);
      }
      im.audio_live_prev = std::move(now);
      im.audio.hd_mixer_.mix_into(output.data() + f * 32, 16,
                                  im.audio.timeline_samples_);
      im.audio.timeline_samples_ += 16;
      frames[f] = {active, previous, im.audio.hd_mixer_.started(),
                   im.audio.hd_mixer_.voice_count(),
                   im.audio_live_inst.at(1000).start_frame};
    }
    require(im.audio.hd_mixer_.started() == (repeated ? 2U : 1U) &&
                im.audio.hd_mixer_.voice_count() == (repeated ? 2U : 1U) &&
                im.hd_fallback == 0,
            "reference_state_differs");
    bool intact = !qa_continuity::overflow &&
                  qa_continuity::gate_count == (repeated ? 3U : 4U) &&
                  qa_continuity::sample_count == (repeated ? 80U : 64U);
    std::array<std::uint64_t, 2> next_output{0, 48};
    std::array<std::size_t, 2> next_source{};
    for (std::size_t i = 0; i < qa_continuity::sample_count; ++i) {
      const auto &s = qa_continuity::samples[i];
      require(s.occurrence >= 1 && s.occurrence <= 2,
              "invalid_observed_occurrence");
      const auto index = static_cast<std::size_t>(s.occurrence - 1);
      intact = intact && s.key == 1000 && s.output == next_output[index]++ &&
               s.source == next_source[index]++ &&
               s.next_source == s.source + 1;
      const bool fading = repeated && s.occurrence == 1 && s.output >= 48;
      intact = intact && (fading ? (s.gain <= 1 && s.gain > 0 &&
                                    s.fade_left == 2645 - (s.output - 48))
                                 : (s.gain == 1 && s.fade_left == 0));
    }
    for (std::size_t i = 0; i < qa_continuity::gate_count; ++i) {
      const auto &g = qa_continuity::gates[i];
      const auto expected_frame = repeated && i == 2 ? 3 : i;
      intact = intact && g.frame == expected_frame &&
               std::string_view(g.kind) ==
                   (expected_frame == 0 || (repeated && expected_frame == 3)
                        ? "rising_edge"
                        : "gate_not_entered");
    }
    std::ofstream file;
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.open(directory / "output.s16le", std::ios::binary);
    file.write(reinterpret_cast<const char *>(output.data()), sizeof(output));
    file.close();
    file.open(directory / "run.toml");
    file << "schema_version = 1\nsource = "
            "\"controlled_active_events\"\nsignature = 1000\nasset "
            "= \"fixture.wav\"\noriginal_sample = 200\nasset_frames = "
            "128\nasset_sample_base = "
            "1000\nasset_sample_step = 37\nsample_rate = 44100\nchannels = "
            "2\naudio_enabled = "
            "true\ntransport_playing = true\nbypass = false\nwindow_duration = "
            "0\n";
    for (std::size_t f = 0; f < frames.size(); ++f) {
      const auto &s = frames[f];
      file << "\n[[frames]]\nframe = " << f
           << "\nactive = " << (s.input ? "true" : "false")
           << "\nprevious_active = " << (s.previous ? "true" : "false")
           << "\nvoices_started = " << s.started
           << "\nvoices_active = " << s.voices
           << "\nlogical_anchor = " << s.anchor << "\noutput_begin = " << f * 16
           << "\noutput_end = " << (f + 1) * 16 << '\n';
    }
    file.close();
    file.open(directory / "trace.toml");
    file.precision(17);
    file << "schema_version = 1\ntrace_complete = "
         << (intact ? "true" : "false") << '\n';
    for (std::size_t i = 0; i < qa_continuity::gate_count; ++i) {
      const auto &g = qa_continuity::gates[i];
      file << "\n[[gates]]\nframe = " << g.frame << "\nbranch = \"" << g.kind
           << "\"\n";
    }
    for (std::size_t i = 0; i < qa_continuity::sample_count; ++i) {
      const auto &s = qa_continuity::samples[i];
      file << "\n[[samples]]\noccurrence = " << s.occurrence
           << "\nkey = " << s.key << "\noutput = " << s.output
           << "\nsource = " << s.source << "\nnext_source = " << s.next_source
           << "\ngain = " << s.gain << "\nfade_left = " << s.fade_left << '\n';
    }
    file.close();
    if (QA_CONTINUITY_OBSERVER && !intact) {
      std::fputs("continuity_trace_incomplete\n", stderr);
      return 2;
    }
    require(QA_CONTINUITY_OBSERVER || (qa_continuity::gate_count == 0 &&
                                       qa_continuity::sample_count == 0),
            "reference_instrumented");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "continuity_probe_failed: %s\n", error.what());
    return 1;
  }
}
