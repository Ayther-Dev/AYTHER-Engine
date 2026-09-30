#include "observer.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <ayther_file.h>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <decode_limits.h>
#include <dr_flac.h>
#include <filesystem>
#include <fstream>
#include <log.h>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c> // NOLINT(bugprone-suspicious-include): vendor declaration-only mode.

namespace qa_stage {
constexpr std::size_t max_frames = 4096;
struct Batch {
  std::uint64_t hash{};
  std::size_t begin{};
  std::size_t frames{};
};
struct State {
  const char *kind{};
  std::size_t frame{};
  std::size_t mark{};
  std::uint64_t timeline{};
  std::size_t frames{};
  std::array<std::int16_t, 256> pcm{};
  std::array<Batch, 4> batches{};
  std::size_t batch_count{};
};
std::array<State, 32> states{};
std::size_t state_count{};
std::size_t frame{};
bool invalid{};
bool lose{};
struct VoiceSample {
  std::uint64_t key{};
  std::uint64_t timeline{};
  std::size_t source{};
  std::size_t next_source{};
  float gain{};
  std::int16_t before_left{};
  std::int16_t before_right{};
  std::int16_t after_left{};
  std::int16_t after_right{};
};
std::array<VoiceSample, 96> voices{};
std::size_t voice_count{};
bool lose_voice{};
const char *resume_reason{};
bool lose_resume{};
void record_resume_reason(const char *reason) noexcept {
  if (!lose_resume) {
    resume_reason = reason;
  }
}
template <class T> T number(std::string_view text) {
  T value{};
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    throw std::runtime_error("invalid_resume_number");
  }
  return value;
}
void record_voice(VoiceSample sample) noexcept {
  if (lose_voice && sample.timeline == 60) {
    return;
  }
  if (voice_count == voices.size()) {
    invalid = true;
    return;
  }
  voices[voice_count++] = sample;
}
struct Coordinates {
  std::size_t mark{};
  std::uint64_t timeline{};
};
bool states_match(std::string_view mode) {
  if (state_count < 5) {
    return false;
  }
  const auto state_is = [](std::size_t i, std::string_view kind, std::size_t f,
                           std::size_t mark, std::size_t frames,
                           std::size_t batches) {
    return i < state_count && states[i].kind == kind && states[i].frame == f &&
           states[i].mark == mark && states[i].timeline == 0 &&
           states[i].frames == frames && states[i].batch_count == batches;
  };
  const auto batch_is = [](std::size_t state, std::size_t i, Batch expected) {
    const auto &b = states[state].batches[i];
    return b.hash == expected.hash && b.begin == expected.begin &&
           b.frames == expected.frames;
  };
  bool valid =
      state_is(0, "frame", 0, 0, 0, 0) && state_is(1, "batch", 0, 0, 32, 1) &&
      state_is(2, "batch", 0, 0, 48, 2) && state_is(3, "frame", 1, 48, 48, 2) &&
      state_is(4, "batch", 1, 48, 96, 3) && batch_is(1, 0, {11, 0, 32}) &&
      batch_is(2, 1, {12, 32, 16}) && batch_is(4, 2, {22, 48, 48});
  std::size_t flush = 5;
  std::size_t expected_frame = 1;
  std::size_t expected_mark = 48;
  std::size_t expected_frames = 96;
  std::size_t expected_batches = 3;
  if (mode == "add" || mode == "replace") {
    expected_batches = 1;
    expected_frames = mode == "replace" ? 32 : 96;
    expected_mark = mode == "replace" ? 0 : 48;
    valid = valid &&
            state_is(5, mode == "replace" ? "router_replace" : "router_add", 1,
                     expected_mark, expected_frames, 1) &&
            batch_is(5, 0, {0xA17E'2600'0000'0001ULL, 0, expected_frames});
    ++flush;
  } else if (mode == "discard") {
    valid = valid && state_is(5, "discard", 1, 48, 96, 3) &&
            state_is(6, "discarded", 1, 0, 0, 0) &&
            state_is(7, "frame", 2, 0, 0, 0) &&
            state_is(8, "batch", 2, 0, 24, 1) && batch_is(8, 0, {33, 0, 24});
    flush = 9;
    expected_frame = 2;
    expected_mark = 0;
    expected_frames = 24;
    expected_batches = 1;
  }
  valid = valid && state_count == flush + 3 &&
          state_is(flush, "flush_begin", expected_frame, expected_mark,
                   expected_frames, expected_batches) &&
          state_is(flush + 1, "pre_mix", expected_frame, expected_mark,
                   expected_frames, expected_batches) &&
          state_is(flush + 2, "post_mix", expected_frame, expected_mark,
                   expected_frames, expected_batches);
  if (!valid) {
    return false;
  }
  const auto &before = states[flush];
  const auto &muted = states[flush + 1];
  for (std::size_t i = 0; i < expected_frames * 2; ++i) {
    const auto expected = mode == "suppress" || mode == "voice_suppress" ||
                                  (mode == "mute" && i >= 64 && i < 96)
                              ? 0
                              : before.pcm[i];
    if (muted.pcm[i] != expected) {
      return false;
    }
  }
  return true;
}
template <class Pcm, class Batches>
void record(const char *kind, Coordinates coordinates, const Pcm &pcm,
            const Batches &batches) noexcept {
  if (lose && std::string_view(kind) == "post_mix") {
    return;
  }
  if (state_count == states.size() || pcm.size() > 256 || batches.size() > 4) {
    invalid = true;
    return;
  }
  auto &s = states[state_count++];
  s.kind = kind;
  s.frame = frame;
  s.mark = coordinates.mark;
  s.timeline = coordinates.timeline;
  s.frames = pcm.size() / 2;
  std::copy(pcm.begin(), pcm.end(), s.pcm.begin());
  for (const auto &b : batches) {
    s.batches[s.batch_count++] = {b.hash, b.frame_offset, b.frames};
  }
}
void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}
struct Submission {
  std::size_t begin{};
  std::size_t frames{};
  std::size_t state_count{};
};
struct Span {
  AytherQaConversion event{};
  std::size_t input{};
  std::size_t output{};
};
struct Capture {
  bool preparing{};
  SDL_AudioStream *stream{};
  std::array<std::int16_t, max_frames * 2> input{};
  std::size_t inputs{};
  std::array<Submission, 4> submissions{};
  std::size_t submission_count{};
  std::array<Span, 32> spans{};
  std::size_t span_count{};
  std::size_t consumed{};
  std::size_t converted{};
  std::size_t captured{};
  std::size_t target{};
  Sint64 phase{};
  std::array<float, max_frames * 2> conversion{};
  std::array<float, max_frames * 2> output{};
  std::atomic<bool> done{};
  bool invalid{};
  static void SDLCALL put(void *userdata, SDL_AudioStream *stream,
                          const SDL_AudioSpec *spec, int bytes,
                          const void *data) noexcept {
    auto &c = *static_cast<Capture *>(userdata);
    if (c.preparing) {
      return;
    }
    if ((c.stream != nullptr && c.stream != stream) ||
        spec->format != SDL_AUDIO_S16 || spec->channels != 2 ||
        spec->freq != 44100 || bytes <= 0 || bytes % 4 != 0 ||
        c.submission_count == c.submissions.size()) {
      c.invalid = true;
      return;
    }
    const auto frames = static_cast<std::size_t>(bytes) / 4;
    if (frames > max_frames - c.inputs) {
      c.invalid = true;
      return;
    }
    c.stream = stream;
    c.submissions[c.submission_count++] = {c.inputs, frames,
                                           qa_stage::state_count};
    std::memcpy(c.input.data() + c.inputs * 2, data,
                static_cast<std::size_t>(bytes));
    c.inputs += frames;
  }
  static void SDLCALL convert(void *userdata,
                              const AytherQaConversion *event) noexcept {
    auto &c = *static_cast<Capture *>(userdata);
    if (c.preparing) {
      return;
    }
    if (event->stream != c.stream ||
        event->destination.format != SDL_AUDIO_F32 ||
        event->destination.channels != 2 || event->destination.freq != 44100 ||
        event->output_frames <= 0 || event->input_frames < 0 ||
        c.span_count == c.spans.size()) {
      c.invalid = true;
      return;
    }
    const auto size = static_cast<std::size_t>(event->output_frames);
    const auto stride = event->rate ? event->rate : Sint64{1} << 32;
    if (size > max_frames - c.converted || event->phase_before != c.phase ||
        event->phase_after != event->phase_before +
                                  event->output_frames * stride -
                                  event->input_frames * (Sint64{1} << 32)) {
      c.invalid = true;
      return;
    }
    auto &s = c.spans[c.span_count++];
    s = {*event, c.consumed, c.converted};
    s.event.stream = nullptr;
    s.event.output = nullptr;
    std::memcpy(c.conversion.data() + c.converted * 2, event->output, size * 8);
    c.converted += size;
    c.consumed += static_cast<std::size_t>(event->input_frames);
    c.phase = event->phase_after;
  }
  static void SDLCALL postmix(void *userdata, const SDL_AudioSpec *spec,
                              float *pcm, int bytes) noexcept {
    auto &c = *static_cast<Capture *>(userdata);
    if (c.done.load(std::memory_order_relaxed)) {
      return;
    }
    if (spec->format != SDL_AUDIO_F32 || spec->channels != 2 ||
        spec->freq != 44100 || bytes != 4096 || c.captured >= c.target) {
      c.invalid = true;
      c.done.store(true, std::memory_order_release);
      return;
    }
    const auto n = std::min(std::size_t{512}, c.target - c.captured);
    std::memcpy(c.output.data() + c.captured * 2, pcm, n * 8);
    c.captured += n;
    if (c.captured == c.target) {
      c.done.store(true, std::memory_order_release);
    }
  }
};
struct SdlLifetime {
  SdlLifetime() { require(SDL_Init(SDL_INIT_AUDIO), "sdl_init_failed"); }
  ~SdlLifetime() { SDL_Quit(); }
  SdlLifetime(const SdlLifetime &) = delete;
  SdlLifetime &operator=(const SdlLifetime &) = delete;
};
struct Observers {
  AytherQaSetSubmissionObserver put;
  AytherQaSetConversionObserver convert;
  Observers(SDL_SharedObject *library, Capture &c)
      : put(reinterpret_cast<AytherQaSetSubmissionObserver>(
            SDL_LoadFunction(library, "AYTHER_QA_SetSubmissionObserver"))),
        convert(reinterpret_cast<AytherQaSetConversionObserver>(
            SDL_LoadFunction(library, "AYTHER_QA_SetConversionObserver"))) {
    require(put && convert, "observers_missing");
    put(Capture::put, &c);
    convert(Capture::convert, &c);
  }
  ~Observers() {
    put(nullptr, nullptr);
    convert(nullptr, nullptr);
  }
  Observers(const Observers &) = delete;
  Observers &operator=(const Observers &) = delete;
};
} // namespace qa_stage

#if QA_STAGE_OBSERVER
#include <stage_player.h>
#include <stage_resume.h>

#include <observed_methods.inc>
using Player = QaStagePlayer;
#else
#include <audio_live_resume.h>
#include <audio_player.h>
using Player = AudioPlayer;
#endif

int main(int argc, char *argv[]) {
  using qa_stage::require;
  try {
    require(argc == 5 || argc == 12, "invalid_arguments");
    const std::filesystem::path directory{argv[1]};
    const std::string_view mode{argv[2]};
    require(mode == "batches" || mode == "mute" || mode == "add" ||
                mode == "replace" || mode == "discard" || mode == "suppress" ||
                mode == "lost" || mode == "voice" || mode == "voice_suppress" ||
                mode == "voice_lost" || mode == "resume_voice" ||
                mode == "resume_lost",
            "invalid_case");
    for (const auto *name : {"input.s16le", "mix.f32le", "staging.toml"}) {
      require(!std::filesystem::exists(directory / name), "output_exists");
    }
    qa_stage::lose = mode == "lost";
    qa_stage::lose_voice = mode == "voice_lost";
    const bool resumes = mode == "resume_voice" || mode == "resume_lost";
    require(argc == (resumes ? 12 : 5), "invalid_resume_arguments");
    qa_stage::lose_resume = mode == "resume_lost";
    const bool has_voice = mode == "voice" || mode == "voice_suppress" ||
                           mode == "voice_lost" || resumes;
    ayther::LiveResumeDecision resume_decision{};
    std::uint64_t resume_end = UINT64_MAX;
    std::uint64_t resume_cut = UINT64_MAX;
    if (resumes) {
      const auto resume_frame = qa_stage::number<std::uint64_t>(argv[5]);
      const auto start_frame = qa_stage::number<std::uint64_t>(argv[6]);
      const auto end_frame = qa_stage::number<std::uint64_t>(argv[7]);
      const auto cut_frame = qa_stage::number<std::uint64_t>(argv[8]);
      resume_end = end_frame;
      resume_cut = cut_frame;
      const auto looping = qa_stage::number<unsigned>(argv[9]);
      const auto fps = qa_stage::number<double>(argv[10]);
      const auto duration = qa_stage::number<double>(argv[11]);
      require(looping == 1 && std::isfinite(fps) && std::isfinite(duration),
              "invalid_resume_fixture");
#if QA_STAGE_OBSERVER
      resume_decision = ayther::qa_live_resume_decide(
          resume_frame, start_frame, end_frame, cut_frame, true, fps, duration);
#else
      resume_decision = ayther::live_resume_decide(
          resume_frame, start_frame, end_frame, cut_frame, true, fps, duration);
#endif
      require(resume_decision.action == ayther::LiveResumeAction::Restart &&
                  resume_decision.offset_seconds == 1.5,
              "resume_decision_differs");
    }
    using Pack = std::unique_ptr<AyArchive, decltype(&ayther_pack_close)>;
    Pack pack(ayther_pack_open_trusted(argv[3], argv[4]), ayther_pack_close);
    require(pack != nullptr, "fixture_pack_failed");
    qa_stage::SdlLifetime sdl;
    require(std::string_view(SDL_GetCurrentAudioDriver()) == "dummy",
            "dummy_required");
    using Library =
        std::unique_ptr<SDL_SharedObject, decltype(&SDL_UnloadObject)>;
    Library library(SDL_LoadObject("SDL3.dll"), SDL_UnloadObject);
    require(library != nullptr, "library_load_failed");
    auto capture = std::make_unique<qa_stage::Capture>();
    qa_stage::Observers observers(library.get(), *capture);
    Player player;
    require(player.init(ayther::RuntimeOptions{}), "player_init_failed");
    require(SDL_PauseAudioDevice(player.device_id()), "pause_failed");
    std::array<std::int16_t, 96> source{};
    for (std::size_t i = 0; i < source.size(); ++i) {
      source[i] = static_cast<std::int16_t>(1000 + 17 * i);
    }
    player.mark_frame_boundary();
    player.buffer_emulator(11, source.data(), 32);
    player.buffer_emulator(12, source.data() + 64, 16);
    qa_stage::frame = 1;
    player.mark_frame_boundary();
    player.buffer_emulator(22, source.data(), 48);
    if (has_voice) {
      // Asset preparation has its own converter; gameplay capture starts at
      // submission.
      capture->preparing = true;
      require(player.play_event_hd(pack.get(), "fixture.wav", resumes, 901,
                                   resume_end, resume_cut,
                                   resumes ? resume_decision.offset_seconds
                                           : 3.0 / 44100.0,
                                   0, 0.25F),
              "voice_start_failed");
      capture->preparing = false;
      require(player.hd_voice_count() == 1, "voice_not_active");
    }
    if (mode == "mute") {
      const std::uint64_t hash = 12;
      player.set_user_mute_hashes(&hash, 1);
    }
    std::array<float, 64> router{};
    for (std::size_t i = 0; i < router.size(); ++i) {
      router[i] = (i % 2 == 0 ? 1.0F : -1.0F) * 0.0625F;
    }
    if (mode == "add" || mode == "replace") {
      player.buffer_router(router.data(), 32, mode == "add");
    }
    if (mode == "discard") {
      player.discard_emulator();
      qa_stage::frame = 2;
      player.mark_frame_boundary();
      player.buffer_emulator(33, source.data(), 24);
    }
    player.flush_emulator(mode == "suppress" || mode == "voice_suppress");
    require(!capture->invalid && capture->submission_count == 2 &&
                capture->submissions[0].frames == 3072 &&
                capture->stream != nullptr,
            "input_submission_incomplete");
    const auto &submitted = capture->submissions[1];
    require(player.timeline_samples() == submitted.frames &&
                submitted.begin == 3072,
            "timeline_domain_differs");
    require(SDL_FlushAudioStream(capture->stream), "eos_failed");
    const int available = SDL_GetAudioStreamAvailable(capture->stream);
    require(available > 0 && available % 8 == 0 && available <= 32768,
            "output_limit_invalid");
    capture->target = static_cast<std::size_t>(available) / 8;
    require(SDL_SetAudioPostmixCallback(
                player.device_id(), qa_stage::Capture::postmix, capture.get()),
            "postmix_failed");
    require(SDL_ResumeAudioDevice(player.device_id()), "resume_failed");
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!capture->done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      SDL_Delay(1);
    }
    require(SDL_PauseAudioDevice(player.device_id()), "final_pause_failed");
    require(SDL_SetAudioPostmixCallback(player.device_id(), nullptr, nullptr),
            "detach_failed");
    require(!capture->invalid && capture->captured == capture->target &&
                capture->consumed == capture->inputs &&
                capture->converted == capture->target &&
                capture->output == capture->conversion,
            "capture_incomplete");
    bool aligned = false;
    if (QA_STAGE_OBSERVER && submitted.state_count > 0) {
      const auto &state = qa_stage::states[submitted.state_count - 1];
      aligned = !qa_stage::invalid && qa_stage::states_match(mode) &&
                std::string_view(state.kind) == "post_mix" &&
                state.timeline == 0 && state.frames == submitted.frames &&
                std::equal(state.pcm.begin(),
                           state.pcm.begin() +
                               static_cast<std::ptrdiff_t>(state.frames * 2),
                           capture->input.begin() + static_cast<std::ptrdiff_t>(
                                                        submitted.begin * 2));
      if (has_voice && aligned) {
        auto reconstructed = qa_stage::states[qa_stage::state_count - 2].pcm;
        aligned = qa_stage::voice_count == 48;
        for (std::size_t i = 0; i < qa_stage::voice_count; ++i) {
          const auto &v = qa_stage::voices[i];
          aligned = aligned && v.key == 901 && v.timeline == 48 + i &&
                    v.source == (resumes ? 614 : 3) + i &&
                    v.next_source == v.source + 1 && v.gain == 0.25F;
          require(v.timeline < state.frames, "voice_timeline_invalid");
          const auto at = static_cast<std::size_t>(v.timeline) * 2;
          aligned = aligned && reconstructed[at] == v.before_left &&
                    reconstructed[at + 1] == v.before_right;
          reconstructed[at] = v.after_left;
          reconstructed[at + 1] = v.after_right;
        }
        aligned = aligned && reconstructed == state.pcm;
        if (resumes) {
          aligned = aligned && qa_stage::resume_reason != nullptr &&
                    std::string_view(qa_stage::resume_reason) ==
                        "resume_at_emulated_offset";
        }
        // This case has an observed unit step: preserve the exact domain
        // translation.
        for (std::size_t i = 0; i < capture->span_count; ++i) {
          const auto &span = capture->spans[i];
          aligned = aligned && span.input == span.output &&
                    span.event.rate == 0 && span.event.phase_before == 0 &&
                    span.event.phase_after == 0;
        }
      }
    }
    std::ofstream file;
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.open(directory / "input.s16le", std::ios::binary);
    file.write(reinterpret_cast<const char *>(capture->input.data()),
               static_cast<std::streamsize>(capture->inputs * 4));
    file.close();
    file.open(directory / "mix.f32le", std::ios::binary);
    file.write(reinterpret_cast<const char *>(capture->output.data()),
               static_cast<std::streamsize>(capture->captured * 8));
    file.close();
    file.open(directory / "staging.toml");
    file.precision(17);
    file << "schema_version = 1\ncase = \"" << mode
         << "\"\nobserver_enabled = " << (QA_STAGE_OBSERVER ? "true" : "false")
         << "\nstaging_aligned = " << (aligned ? "true" : "false")
         << "\ninput_frames = " << capture->inputs
         << "\noutput_frames = " << capture->captured
         << "\nmain_timeline = " << player.timeline_samples() << '\n';
    if (resumes) {
      file << "resume_action = \"restart\"\nresume_offset_seconds = "
           << resume_decision.offset_seconds << "\nresume_reason = \""
           << (qa_stage::resume_reason ? qa_stage::resume_reason : "unobserved")
           << "\"\nresume_voice_count = " << player.hd_voice_count() << '\n';
    }
    for (std::size_t i = 0; i < capture->submission_count; ++i) {
      const auto &s = capture->submissions[i];
      file << "\n[[submissions]]\ninput_begin = " << s.begin
           << "\nframes = " << s.frames << "\nstates_before = " << s.state_count
           << '\n';
    }
    for (std::size_t i = 0; i < qa_stage::state_count; ++i) {
      const auto &s = qa_stage::states[i];
      file << "\n[[states]]\nkind = \"" << s.kind << "\"\nframe = " << s.frame
           << "\nmark = " << s.mark << "\ntimeline = " << s.timeline
           << "\nframes = " << s.frames << "\npcm = [";
      for (std::size_t j = 0; j < s.frames * 2; ++j) {
        file << (j ? ", " : "") << s.pcm[j];
      }
      file << "]\n";
      for (std::size_t j = 0; j < s.batch_count; ++j) {
        const auto &b = s.batches[j];
        file << "[[states.batches]]\nhash = \"" << b.hash
             << "\"\nbegin = " << b.begin << "\nframes = " << b.frames << '\n';
      }
    }
    for (std::size_t i = 0; i < capture->span_count; ++i) {
      const auto &s = capture->spans[i];
      const auto &e = s.event;
      file << "\n[[spans]]\ninput_begin = " << s.input
           << "\noutput_begin = " << s.output
           << "\ninput_frames = " << e.input_frames
           << "\noutput_frames = " << e.output_frames
           << "\nphase_before_q32 = " << e.phase_before
           << "\nphase_after_q32 = " << e.phase_after
           << "\nrate_q32 = " << e.rate << "\nsupport_left = " << e.support_left
           << "\nsupport_right = " << e.support_right << '\n';
    }
    for (std::size_t i = 0; i < qa_stage::voice_count; ++i) {
      const auto &v = qa_stage::voices[i];
      file << "\n[[voice_samples]]\nkey = " << v.key
           << "\nframe = 1\ntimeline = " << v.timeline
           << "\nsource_frame = " << v.source
           << "\nnext_source_frame = " << v.next_source << "\ngain = " << v.gain
           << "\ninput_frame = " << submitted.begin + v.timeline
           << "\noutput_frame = " << submitted.begin + v.timeline
           << "\nbefore = [" << v.before_left << ", " << v.before_right
           << "]\nafter = [" << v.after_left << ", " << v.after_right << "]\n";
    }
    file.close();
    if (QA_STAGE_OBSERVER && !aligned) {
      std::fputs("staging_trace_incomplete\n", stderr);
      return 2;
    }
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "staging_probe_failed: %s\n", error.what());
    return 1;
  }
}
