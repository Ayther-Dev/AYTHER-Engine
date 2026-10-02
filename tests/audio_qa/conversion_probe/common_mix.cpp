#include "observer.h"
#include <ayther/audio_player.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
constexpr std::size_t input_frames = 1024;
constexpr std::size_t output_limit = 8192;
constexpr std::size_t route_count = 5;
constexpr Sint64 phase_unit = Sint64{1} << 32;
enum class Route : std::size_t {
  main,
  synth,
  pcm_preview,
  asset_preview,
  legacy,
  none
};
constexpr std::array<const char *, route_count> names{
    "main", "synth", "pcm_preview", "asset_preview", "legacy"};

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct RouteState {
  SDL_AudioStream *stream{};
  SDL_AudioSpec source{};
  std::size_t submitted{};
  std::size_t consumed{};
  std::size_t produced{};
  std::size_t in_block{};
  std::size_t available_before_close{};
  std::size_t expected_output_at_close{};
  Sint64 phase{};
  bool destroyed{};
};

struct Lifecycle {
  Route route{};
  bool destroyed{};
  std::size_t output_boundary{};
  std::size_t consumed{};
  std::size_t queued_frames{};
  Sint64 phase_before{};
  Sint64 phase_after{};
};

struct Span {
  Route route{};
  std::size_t input_begin{};
  std::size_t output_begin{};
  std::size_t pcm_offset{};
  AytherQaConversion conversion{};
};

struct InputChunk {
  Route route{};
  std::size_t begin{};
  std::size_t frames{};
  bool zero{};
};

struct Capture {
  std::array<RouteState, route_count> routes{};
  std::array<Span, 256> spans{};
  std::array<InputChunk, 16> inputs{};
  std::size_t input_count{};
  std::size_t span_count{};
  std::size_t frames{};
  std::size_t blocks{};
  std::size_t target = output_limit;
  std::vector<float> pcm = std::vector<float>(output_limit * 2);
  std::vector<float> check_sum = std::vector<float>(output_limit * 2);
  std::vector<float> contributions =
      std::vector<float>(output_limit * route_count * 2);
  std::size_t contribution_samples{};
  std::array<Lifecycle, 8> lifecycle{};
  std::size_t lifecycle_count{};
  SDL_AudioDeviceID device{};
  bool switch_case{};
  std::atomic<bool> checkpoint{false};
  Route submitting = Route::none;
  bool invalid{};
  bool omit_synth{};
  bool lose_prime{};
  bool prime_lost{};
  int output_rate = 44100;
  std::atomic<bool> frozen{false};
  std::atomic<std::size_t> submissions_after_freeze{0};
  std::atomic<bool> enabled{false};
  std::atomic<bool> done{false};

  void observe_lifecycle(SDL_AudioStream *stream,
                         const Lifecycle &event) noexcept {
    if (!enabled.load(std::memory_order_acquire)) {
      return;
    }
    const auto found =
        std::find_if(routes.begin(), routes.end(),
                     [&](const auto &route) { return route.stream == stream; });
    if (found == routes.end() || lifecycle_count == lifecycle.size()) {
      invalid = true;
      return;
    }
    auto copy = event;
    copy.route = static_cast<Route>(found - routes.begin());
    copy.output_boundary = frames;
    copy.consumed = found->consumed;
    if (copy.phase_before != found->phase ||
        copy.queued_frames != found->submitted - found->consumed) {
      invalid = true;
    }
    lifecycle[lifecycle_count++] = copy;
    found->phase = copy.phase_after;
    found->destroyed = copy.destroyed;
    if (copy.destroyed) {
      found->stream = nullptr;
    }
  }

  static void SDLCALL reset(void *userdata,
                            const AytherQaReset *event) noexcept {
    auto &self = *static_cast<Capture *>(userdata);
    const int width = SDL_AUDIO_FRAMESIZE(event->source);
    if (width <= 0 ||
        event->queued_bytes_before % static_cast<std::size_t>(width) != 0 ||
        event->queued_bytes_after != 0) {
      self.invalid = true;
      return;
    }
    self.observe_lifecycle(
        event->stream,
        {Route::none, false, 0, 0,
         event->queued_bytes_before / static_cast<std::size_t>(width),
         event->phase_before, event->phase_after});
  }

  static void SDLCALL end(void *userdata,
                          const AytherQaStreamEnd *event) noexcept {
    auto &self = *static_cast<Capture *>(userdata);
    const int width = SDL_AUDIO_FRAMESIZE(event->source);
    if (width <= 0 ||
        event->queued_bytes % static_cast<std::size_t>(width) != 0) {
      self.invalid = true;
      return;
    }
    self.observe_lifecycle(
        event->stream, {Route::none, true, 0, 0,
                        event->queued_bytes / static_cast<std::size_t>(width),
                        event->phase, event->phase});
  }

  static void SDLCALL submit(void *userdata, SDL_AudioStream *stream,
                             const SDL_AudioSpec *spec, int bytes,
                             const void *data) noexcept {
    auto &self = *static_cast<Capture *>(userdata);
    if (self.frozen.load(std::memory_order_acquire)) {
      self.submissions_after_freeze.fetch_add(1, std::memory_order_relaxed);
    }
    // Setup runs while the device is paused, before publishing enabled.
    if (self.submitting == Route::none) {
      return;
    }
    auto &route = self.routes[static_cast<std::size_t>(self.submitting)];
    const int frame_bytes = SDL_AUDIO_FRAMESIZE(*spec);
    if ((route.stream != nullptr && route.stream != stream) ||
        frame_bytes <= 0 || bytes <= 0 || bytes % frame_bytes != 0 ||
        data == nullptr || bytes > 65536 ||
        self.input_count == self.inputs.size()) {
      self.invalid = true;
      return;
    }
    route.stream = stream;
    route.source = *spec;
    const auto *values = static_cast<const unsigned char *>(data);
    self.inputs[self.input_count++] = {
        self.submitting, route.submitted,
        static_cast<std::size_t>(bytes / frame_bytes),
        std::all_of(values, values + bytes,
                    [](unsigned char value) { return value == 0; })};
    route.submitted += static_cast<std::size_t>(bytes / frame_bytes);
  }

  static void SDLCALL convert(void *userdata,
                              const AytherQaConversion *event) noexcept {
    auto &self = *static_cast<Capture *>(userdata);
    if (!self.enabled.load(std::memory_order_acquire) ||
        self.done.load(std::memory_order_relaxed)) {
      return;
    }
    const auto found = std::find_if(
        self.routes.begin(), self.routes.end(),
        [&](const auto &route) { return route.stream == event->stream; });
    if (found == self.routes.end() || self.span_count == self.spans.size() ||
        event->destination.format != SDL_AUDIO_F32 ||
        event->destination.channels != 2 ||
        event->destination.freq != self.output_rate ||
        event->output_frames <= 0 || event->input_frames < 0) {
      self.invalid = true;
      return;
    }
    auto &route = *found;
    const auto count = static_cast<std::size_t>(event->output_frames);
    const auto begin = self.frames + route.in_block;
    const auto stride = event->rate == 0 ? phase_unit : event->rate;
    if (begin > output_limit || count > output_limit - begin ||
        count * 2 > self.contributions.size() - self.contribution_samples ||
        route.phase != event->phase_before ||
        event->phase_after != event->phase_before +
                                  event->output_frames * stride -
                                  event->input_frames * phase_unit) {
      self.invalid = true;
      return;
    }
    const auto id = static_cast<Route>(found - self.routes.begin());
    auto &span = self.spans[self.span_count++];
    span = {id, route.consumed, begin, self.contribution_samples, *event};
    span.conversion.stream = nullptr;
    span.conversion.output = nullptr;
    route.consumed += static_cast<std::size_t>(event->input_frames);
    route.produced += count;
    route.in_block += count;
    route.phase = event->phase_after;
    const bool lose = self.lose_prime && !self.prime_lost &&
                      id == Route::synth && span.input_begin < 1536 &&
                      route.consumed > 1024;
    if ((self.omit_synth && id == Route::synth) || lose) {
      self.prime_lost = self.prime_lost || lose;
      --self.span_count;
      return;
    }
    std::memcpy(self.contributions.data() + self.contribution_samples,
                event->output, count * 2 * sizeof(float));
    self.contribution_samples += count * 2;
    const auto *values = static_cast<const float *>(event->output);
    // Verification only: the evidence PCM is copied exclusively by postmix.
    for (std::size_t index = 0; index < count * 2; ++index) {
      auto &sum = self.check_sum[begin * 2 + index];
      sum = std::clamp(sum + values[index], -1.0F, 1.0F);
    }
  }

  static void SDLCALL postmix(void *userdata, const SDL_AudioSpec *spec,
                              float *values, int bytes) noexcept {
    auto &self = *static_cast<Capture *>(userdata);
    if (self.done.load(std::memory_order_relaxed)) {
      return;
    }
    if (spec->format != SDL_AUDIO_F32 || spec->channels != 2 ||
        spec->freq != self.output_rate || bytes != 512 * 8 ||
        self.frames > output_limit - 512) {
      self.invalid = true;
      self.done.store(true, std::memory_order_release);
      return;
    }
    const auto kept = std::min(std::size_t{512}, self.target - self.frames);
    std::memcpy(self.pcm.data() + self.frames * 2, values, kept * 8);
    for (auto &route : self.routes) {
      if (route.in_block > 512) {
        self.invalid = true;
      }
      route.in_block = 0;
    }
    self.frames += kept;
    ++self.blocks;
    if (self.switch_case && self.frames == 512) {
      // Test scheduling only: pause at a real output boundary before the host
      // changes state.
      if (!SDL_PauseAudioDevice(self.device)) {
        self.invalid = true;
        self.done.store(true, std::memory_order_release);
      }
      self.checkpoint.store(true, std::memory_order_release);
    }
    if (self.frames == self.target) {
      self.done.store(true, std::memory_order_release);
    }
  }
};
static_assert(std::atomic<bool>::is_always_lock_free);

struct PrimeAudit {
  bool submitted{};
  bool complete{};
  std::size_t touched_begin = output_limit;
  std::size_t touched_end{};
  std::size_t pure_begin = output_limit;
  std::size_t pure_end{};
};

PrimeAudit inspect_prime(const Capture &capture) {
  PrimeAudit result;
  result.submitted =
      std::count_if(capture.inputs.begin(),
                    capture.inputs.begin() +
                        static_cast<std::ptrdiff_t>(capture.input_count),
                    [](const auto &input) {
                      return input.route == Route::synth &&
                             input.begin == 1024 && input.frames == 512 &&
                             input.zero;
                    }) == 1;
  std::size_t next_input = 0;
  std::size_t next_output = 0;
  bool intact = true;
  bool before_signal = false;
  bool after_signal = false;
  for (std::size_t index = 0; index < capture.span_count; ++index) {
    const auto &span = capture.spans[index];
    if (span.route != Route::synth) {
      continue;
    }
    const auto &row = span.conversion;
    intact = intact && span.input_begin == next_input &&
             span.output_begin == next_output && row.rate > 0 &&
             row.support_left > 0 && row.support_right > 0;
    next_input = span.input_begin + static_cast<std::size_t>(row.input_frames);
    next_output =
        span.output_begin + static_cast<std::size_t>(row.output_frames);
    for (int offset = 0; offset < row.output_frames; ++offset) {
      // Match SDL's signed Q32 source cursor, including its actual filter
      // neighbourhood.
      const auto phase = row.phase_before + offset * row.rate;
      const auto center = static_cast<Sint64>(span.input_begin) + (phase >> 32);
      const auto left = center - row.support_left;
      const auto right = center + row.support_right + 1;
      const auto output = span.output_begin + static_cast<std::size_t>(offset);
      const auto sample =
          span.pcm_offset + static_cast<std::size_t>(offset) * 2;
      const bool zero = capture.contributions[sample] == 0.0F &&
                        capture.contributions[sample + 1] == 0.0F;
      if (left < 1536 && right > 1024) {
        result.touched_begin = std::min(result.touched_begin, output);
        result.touched_end = output + 1;
      }
      if (left >= 1024 && right <= 1536) {
        intact = intact && zero &&
                 (result.pure_end == 0 || result.pure_end == output);
        result.pure_begin = std::min(result.pure_begin, output);
        result.pure_end = output + 1;
      }
      before_signal = before_signal || (right <= 1024 && !zero);
      after_signal = after_signal || (left >= 1536 && !zero);
    }
  }
  const auto &route = capture.routes[static_cast<std::size_t>(Route::synth)];
  result.complete = result.submitted && intact && before_signal &&
                    after_signal && result.pure_begin < result.pure_end &&
                    next_input == 2560 && next_input == route.submitted &&
                    next_output == route.produced;
  return result;
}

struct SdlLifetime {
  SdlLifetime() { require(SDL_Init(SDL_INIT_AUDIO), "sdl_init_failed"); }
  ~SdlLifetime() { SDL_Quit(); }
  SdlLifetime(const SdlLifetime &) = delete;
  SdlLifetime &operator=(const SdlLifetime &) = delete;
};

struct DeviceFixture {
  SDL_AudioDeviceID device{};
  explicit DeviceFixture(bool prime) {
    if (prime) {
      // Open the physical dummy device first, as a 48 kHz output environment.
      // AudioPlayer then queries this actual format through its unchanged
      // normal path.
      const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 48000};
      device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
      require(device != 0, "fixture_device_failed");
    }
  }
  ~DeviceFixture() {
    if (device != 0) {
      SDL_CloseAudioDevice(device);
    }
  }
  DeviceFixture(const DeviceFixture &) = delete;
  DeviceFixture &operator=(const DeviceFixture &) = delete;
};

struct Observers {
  AytherQaSetConversionObserver conversion;
  AytherQaSetSubmissionObserver submission;
  AytherQaSetResetObserver reset;
  AytherQaSetEndObserver end;
  Observers(SDL_SharedObject *library, Capture &capture)
      : conversion(reinterpret_cast<AytherQaSetConversionObserver>(
            SDL_LoadFunction(library, "AYTHER_QA_SetConversionObserver"))),
        submission(reinterpret_cast<AytherQaSetSubmissionObserver>(
            SDL_LoadFunction(library, "AYTHER_QA_SetSubmissionObserver"))),
        reset(reinterpret_cast<AytherQaSetResetObserver>(
            SDL_LoadFunction(library, "AYTHER_QA_SetResetObserver"))),
        end(reinterpret_cast<AytherQaSetEndObserver>(
            SDL_LoadFunction(library, "AYTHER_QA_SetEndObserver"))) {
    require(conversion && submission && reset && end, "observers_missing");
    conversion(Capture::convert, &capture);
    submission(Capture::submit, &capture);
    reset(Capture::reset, &capture);
    end(Capture::end, &capture);
  }
  ~Observers() {
    conversion(nullptr, nullptr);
    submission(nullptr, nullptr);
    reset(nullptr, nullptr);
    end(nullptr, nullptr);
  }
  Observers(const Observers &) = delete;
  Observers &operator=(const Observers &) = delete;
};

struct Postmix {
  SDL_AudioDeviceID device;
  Postmix(SDL_AudioDeviceID id, Capture &capture) : device(id) {
    require(SDL_SetAudioPostmixCallback(device, Capture::postmix, &capture),
            "postmix_failed");
  }
  void detach() {
    if (device != 0) {
      require(SDL_SetAudioPostmixCallback(device, nullptr, nullptr),
              "postmix_detach_failed");
      device = 0;
    }
  }
  ~Postmix() {
    if (device != 0) {
      (void)SDL_SetAudioPostmixCallback(device, nullptr, nullptr);
    }
  }
  Postmix(const Postmix &) = delete;
  Postmix &operator=(const Postmix &) = delete;
};

void write_wave(const std::filesystem::path &path,
                const std::array<std::int16_t, input_frames * 2> &samples) {
  // PCM WAV, 22050 Hz, stereo, S16LE, exactly 4096 payload bytes.
  constexpr std::array<unsigned char, 44> header{
      'R', 'I', 'F',  'F',  0x24, 0x10, 0,    0,    'W',  'A', 'V',
      'E', 'f', 'm',  't',  ' ',  16,   0,    0,    0,    1,   0,
      2,   0,   0x22, 0x56, 0,    0,    0x88, 0x58, 1,    0,   4,
      0,   16,  0,    'd',  'a',  't',  'a',  0,    0x10, 0,   0};
  std::ofstream out;
  out.exceptions(std::ios::failbit | std::ios::badbit);
  out.open(path, std::ios::binary);
  out.write(reinterpret_cast<const char *>(header.data()), header.size());
  out.write(reinterpret_cast<const char *>(samples.data()), sizeof(samples));
  out.close();
}
} // namespace

int main(int argc, char *argv[]) {
  try {
    require(argc == 6, "invalid_arguments");
    const std::string_view mode{argv[3]};
    const bool prime = mode == "prime_resample" || mode == "prime_missing" ||
                       mode == "prime_loss";
    const bool gain_switch =
        mode == "gain_switch" || mode == "gain_switch_loss";
    const bool cut = mode == "transport_cut" || mode == "previews_cut";
    const bool switching =
        gain_switch || cut || mode == "mute_switch" || mode == "natural_end";
    const bool closing =
        mode == "drain" || mode == "drain_loss" || prime || switching;
    require(closing || mode == "all" || mode == "gain" || mode == "muted" ||
                mode == "main_only" || mode == "missing_contribution",
            "invalid_case");
    const std::filesystem::path directory{argv[2]};
    for (const auto *name :
         {"fixture.wav", "mix.f32le", "contributions.f32le", "mix.toml"}) {
      require(!std::filesystem::exists(directory / name), "output_exists");
    }
    std::array<std::int16_t, input_frames * 2> samples{};
    std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
    input.exceptions(std::ios::failbit | std::ios::badbit);
    require(input.tellg() == static_cast<std::streamoff>(sizeof(samples)),
            "input_size");
    input.seekg(0);
    input.read(reinterpret_cast<char *>(samples.data()), sizeof(samples));
    const auto wave = (directory / "fixture.wav").string();
    write_wave(wave, samples);
    using Pack = std::unique_ptr<AyArchive, decltype(&ayther_pack_close)>;
    Pack pack(ayther_pack_open_trusted(argv[4], argv[5]), ayther_pack_close);
    require(pack != nullptr, "pack_open_failed");

    SdlLifetime sdl;
    require(std::string_view(SDL_GetCurrentAudioDriver()) == "dummy",
            "dummy_required");
    DeviceFixture device_fixture(prime);
    using Library =
        std::unique_ptr<SDL_SharedObject, decltype(&SDL_UnloadObject)>;
    Library library(SDL_LoadObject("SDL3.dll"), SDL_UnloadObject);
    require(library != nullptr, "library_load_failed");
    auto capture = std::make_unique<Capture>();
    capture->omit_synth = mode == "missing_contribution";
    capture->lose_prime = mode == "prime_loss";
    capture->output_rate = prime ? 48000 : 44100;
    capture->switch_case = switching;
    Observers observers(library.get(), *capture);
    AudioPlayer player;
    require(player.init(ayther::RuntimeOptions{}), "player_init_failed");
    capture->device = player.device_id();
    require(SDL_PauseAudioDevice(player.device_id()) &&
                SDL_AudioDevicePaused(player.device_id()),
            "initial_pause_failed");
    SDL_AudioSpec actual_spec{};
    int actual_frames = 0;
    require(SDL_GetAudioDeviceFormat(player.device_id(), &actual_spec,
                                     &actual_frames) &&
                actual_spec.freq == capture->output_rate &&
                actual_spec.channels == 2 && actual_frames == 512,
            "device_format_differs");
    // Cache the HD conversion before observing device consumption. It remains a
    // real mixer voice.
    require(player.play_oneshot_asset_file(wave, 101, 0.0, 0.25F, false),
            "hd_voice_failed");
    player.set_game_gain(mode == "gain" ? 0.25F : 1.0F);
    player.set_muted(mode == "muted");
    if (mode != "main_only") {
      capture->submitting = Route::asset_preview;
      require(player.play_oneshot_asset_file(wave, 102, 0.0, 0.5F, true),
              "asset_preview_failed");
      capture->submitting = Route::legacy;
      AytherAudioSub substitution{};
      substitution.hash = 103;
      std::memcpy(substitution.asset_path, "fixture.wav",
                  sizeof("fixture.wav"));
      player.play_substitutions(pack.get(), &substitution, 1);
      capture->submitting = Route::pcm_preview;
      player.play_oneshot_pcm(samples.data(), input_frames);
      capture->submitting = Route::synth;
      std::array<float, input_frames * 2> synth{};
      for (std::size_t index = 0; index < samples.size(); ++index) {
        synth[index] = static_cast<float>(samples[index]) / 131072.0F;
      }
      player.feed_synth(synth.data(), input_frames);
      if (prime) {
        if (mode != "prime_missing") {
          player.prime_synth(512);
        }
        for (auto &value : synth) {
          value = -value;
        }
        std::reverse(synth.begin(), synth.end());
        player.feed_synth(synth.data(), input_frames);
      }
    }
    capture->submitting = Route::main;
    player.mark_frame_boundary();
    player.buffer_emulator(104, samples.data(), input_frames);
    player.flush_emulator();
    capture->submitting = Route::none;
    for (std::size_t index = 0; index < route_count; ++index) {
      const auto &route = capture->routes[index];
      const bool active =
          mode != "main_only" || index == static_cast<std::size_t>(Route::main);
      require((route.stream != nullptr && route.submitted > 0) == active,
              "route_submission_missing");
    }
    require(player.timeline_samples() == input_frames, "timeline_changed");
    Postmix postmix(player.device_id(), *capture);
    if (closing) {
      capture->frozen.store(true, std::memory_order_release);
      capture->target = 0;
      for (auto &route : capture->routes) {
        const auto before = SDL_GetAudioStreamAvailable(route.stream);
        require(before >= 0 && before % 8 == 0,
                "available_before_close_failed");
        route.available_before_close = static_cast<std::size_t>(before) / 8;
        require(SDL_FlushAudioStream(route.stream), "eos_failed");
        const auto after = SDL_GetAudioStreamAvailable(route.stream);
        require(after > 0 && after % 8 == 0, "available_at_close_failed");
        route.expected_output_at_close = static_cast<std::size_t>(after) / 8;
        capture->target =
            std::max(capture->target, route.expected_output_at_close);
      }
      require(capture->target > 0 && capture->target <= output_limit,
              "close_limit_exceeded");
      if (mode == "drain_loss") {
        require(SDL_ClearAudioStream(capture->routes[0].stream),
                "loss_control_failed");
      }
    }
    capture->enabled.store(true, std::memory_order_release);
    require(SDL_ResumeAudioDevice(player.device_id()), "resume_failed");
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool changed = false;
    while (!capture->done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      if (capture->checkpoint.load(std::memory_order_acquire) && !changed) {
        require(SDL_AudioDevicePaused(player.device_id()) &&
                    capture->frames == 512,
                "change_boundary_not_paused");
        for (const auto &route : capture->routes) {
          require(route.consumed > 0 && route.consumed < route.submitted,
                  "change_has_no_pending_audio");
        }
        if (gain_switch) {
          player.set_game_gain(0.25F);
        } else if (mode == "mute_switch") {
          player.set_muted(true);
        } else if (mode == "transport_cut") {
          (void)player.cut_transport_audio();
        } else if (mode == "previews_cut") {
          player.stop_oneshot();
          player.stop_preview_sfx();
        }
        changed = true;
        require(SDL_ResumeAudioDevice(player.device_id()),
                "change_resume_failed");
      }
      SDL_Delay(1);
    }
    require(SDL_PauseAudioDevice(player.device_id()), "final_pause_failed");
    postmix.detach();
    if (mode == "natural_end") {
      player.tick();
    }
    capture->enabled.store(false, std::memory_order_release);
    require(!capture->invalid && capture->frames == capture->target,
            "capture_incomplete");
    for (const auto &route : capture->routes) {
      require(mode == "drain_loss" ||
                  (route.consumed > 0 && route.produced > 0) ==
                      (route.submitted > 0),
              "route_not_consumed");
    }
    require(player.hd_voices_started() == 1 &&
                player.hd_voice_count() == (mode == "transport_cut" ? 0 : 1),
            "hd_voice_not_present");
    for (std::size_t index = 0; index < capture->span_count; ++index) {
      const auto &span = capture->spans[index];
      const float expected_gain =
          mode == "muted" || (mode == "mute_switch" && span.output_begin >= 512)
              ? 0.0F
              : (span.route == Route::asset_preview
                     ? 0.5F
                     : (span.route == Route::main &&
                                (mode == "gain" ||
                                 (gain_switch && span.output_begin >= 512))
                            ? 0.25F
                            : 1.0F));
      require(span.conversion.gain == expected_gain, "effective_gain_differs");
    }
    const bool drained =
        closing && capture->submissions_after_freeze.load() == 0 &&
        std::all_of(capture->routes.begin(), capture->routes.end(),
                    [](const auto &route) {
                      return route.consumed == route.submitted &&
                             route.produced == route.expected_output_at_close &&
                             (route.destroyed ||
                              SDL_GetAudioStreamAvailable(route.stream) == 0);
                    });
    const bool matches = std::equal(capture->pcm.begin(), capture->pcm.end(),
                                    capture->check_sum.begin());
    const auto prime_audit = prime ? inspect_prime(*capture) : PrimeAudit{};
    bool transition_complete = changed && mode != "gain_switch_loss";
    if (switching) {
      const auto expected_lifecycle =
          mode == "transport_cut" || mode == "natural_end"
              ? 3U
              : (mode == "previews_cut" ? 2U : 0U);
      transition_complete =
          transition_complete && capture->lifecycle_count == expected_lifecycle;
      for (std::size_t i = 0; i < capture->lifecycle_count; ++i) {
        const auto &e = capture->lifecycle[i];
        const auto &r = capture->routes[static_cast<std::size_t>(e.route)];
        const bool expected_route =
            mode == "transport_cut"
                ? ((e.route == Route::main || e.route == Route::synth)
                       ? !e.destroyed
                       : (e.route == Route::legacy && e.destroyed))
                : (e.destroyed &&
                   (e.route == Route::pcm_preview ||
                    e.route == Route::asset_preview ||
                    (mode == "natural_end" && e.route == Route::legacy)));
        transition_complete = transition_complete && expected_route;
        transition_complete =
            transition_complete &&
            (mode == "natural_end"
                 ? (e.destroyed && e.queued_frames == 0 &&
                    r.consumed == r.submitted)
                 : (e.output_boundary == 512 && e.queued_frames > 0 &&
                    r.produced == 512));
      }
      if (cut) {
        for (std::size_t i = 0; i < route_count; ++i) {
          const auto id = static_cast<Route>(i);
          const auto &r = capture->routes[i];
          const bool stopped =
              mode == "transport_cut"
                  ? (id == Route::main || id == Route::synth ||
                     id == Route::legacy)
                  : (id == Route::pcm_preview || id == Route::asset_preview);
          transition_complete =
              transition_complete &&
              (stopped ? (r.produced == 512 && r.consumed < r.submitted)
                       : (r.produced == r.expected_output_at_close &&
                          r.consumed == r.submitted));
        }
      }
    }
    if (mode == "muted") {
      require(std::all_of(capture->pcm.begin(), capture->pcm.end(),
                          [](float value) { return value == 0.0F; }),
              "mute_not_observed");
    }
    std::ofstream pcm;
    pcm.exceptions(std::ios::failbit | std::ios::badbit);
    pcm.open(directory / "mix.f32le", std::ios::binary);
    pcm.write(reinterpret_cast<const char *>(capture->pcm.data()),
              static_cast<std::streamsize>(capture->frames * 8));
    pcm.close();
    pcm.open(directory / "contributions.f32le", std::ios::binary);
    pcm.write(reinterpret_cast<const char *>(capture->contributions.data()),
              static_cast<std::streamsize>(capture->contribution_samples *
                                           sizeof(float)));
    pcm.close();
    std::ofstream manifest;
    manifest.exceptions(std::ios::failbit | std::ios::badbit);
    manifest.open(directory / "mix.toml");
    manifest.precision(17);
    manifest << "schema_version = 1\ncase = \"" << mode
             << "\"\nsample_count = " << capture->frames
             << "\nblocks = " << capture->blocks
             << "\ncontributions_match = " << (matches ? "true" : "false")
             << "\norigin = \"first_postmix_after_setup\"\nmain_timeline = "
             << player.timeline_samples() << '\n';
    manifest << "output_rate = " << capture->output_rate << '\n';
    if (switching) {
      manifest << "transition_complete = "
               << (transition_complete ? "true" : "false")
               << "\nchange_recorded = "
               << (mode != "gain_switch_loss" ? "true" : "false") << '\n';
      if (mode != "gain_switch_loss") {
        manifest << "change_output_boundary = 512\n";
      }
    }
    manifest << "closing_case = " << (closing ? "true" : "false")
             << "\ndrained = " << (drained ? "true" : "false") << '\n';
    if (closing) {
      manifest << "produced_output_limit = " << capture->target
               << "\ninput_submissions_after_close = "
               << capture->submissions_after_freeze.load()
               << "\ngame_steps_executed = 0\nhd_voices_at_close = "
               << player.hd_voice_count() << '\n';
    }
    if (prime) {
      manifest << "prime_input_observed = "
               << (prime_audit.submitted ? "true" : "false")
               << "\nprime_alignment_complete = "
               << (prime_audit.complete ? "true" : "false")
               << "\nprime_record_dropped = "
               << (capture->prime_lost ? "true" : "false")
               << "\nprime_input_begin = 1024\nprime_input_end = 1536\n"
               << "prime_support_output_begin = " << prime_audit.touched_begin
               << "\nprime_support_output_end = " << prime_audit.touched_end
               << "\nprime_pure_output_begin = " << prime_audit.pure_begin
               << "\nprime_pure_output_end = " << prime_audit.pure_end << '\n';
    }
    for (std::size_t index = 0; index < capture->input_count; ++index) {
      const auto &input_chunk = capture->inputs[index];
      manifest << "\n[[inputs]]\nroute = \""
               << names[static_cast<std::size_t>(input_chunk.route)]
               << "\"\ninput_begin = " << input_chunk.begin
               << "\ninput_frames = " << input_chunk.frames
               << "\nall_zero = " << (input_chunk.zero ? "true" : "false")
               << '\n';
    }
    for (std::size_t index = 0; index < capture->lifecycle_count; ++index) {
      const auto &e = capture->lifecycle[index];
      manifest << "\n[[lifecycle]]\nroute = \""
               << names[static_cast<std::size_t>(e.route)]
               << "\"\noperation = \"" << (e.destroyed ? "destroy" : "clear")
               << "\"\noutput_boundary = " << e.output_boundary
               << "\nconsumed = " << e.consumed
               << "\nqueued_frames = " << e.queued_frames
               << "\nphase_before_q32 = " << e.phase_before
               << "\nphase_after_q32 = " << e.phase_after << '\n';
    }
    for (std::size_t index = 0; index < route_count; ++index) {
      const auto &route = capture->routes[index];
      manifest << "\n[[routes]]\nid = \"" << names[index]
               << "\"\nsubmitted = " << route.submitted
               << "\nconsumed = " << route.consumed
               << "\nproduced = " << route.produced
               << "\nactive = " << (route.submitted > 0 ? "true" : "false")
               << '\n';
      if (closing) {
        manifest << "available_before_close = " << route.available_before_close
                 << "\nexpected_output_at_close = "
                 << route.expected_output_at_close << '\n';
      }
      if (route.submitted > 0) {
        manifest << "source_rate = " << route.source.freq << '\n';
      }
    }
    for (std::size_t index = 0; index < capture->span_count; ++index) {
      const auto &span = capture->spans[index];
      const auto &row = span.conversion;
      manifest << "\n[[spans]]\nroute = \""
               << names[static_cast<std::size_t>(span.route)]
               << "\"\ninput_begin = " << span.input_begin
               << "\ninput_frames = " << row.input_frames
               << "\noutput_begin = " << span.output_begin
               << "\noutput_frames = " << row.output_frames
               << "\nrate_q32 = " << row.rate
               << "\ncontribution_value_offset = " << span.pcm_offset
               << "\nphase_before_q32 = " << row.phase_before
               << "\nphase_after_q32 = " << row.phase_after
               << "\npadding_frames = " << row.padding_frames
               << "\ngain = " << row.gain
               << "\nsupport_left = " << row.support_left
               << "\nsupport_right = " << row.support_right << '\n';
    }
    manifest.close();
    if (switching && !transition_complete) {
      std::fputs("transition_trace_incomplete\n", stderr);
      return 6;
    }
    if (closing && !cut && !drained) {
      std::fputs("drain_incomplete\n", stderr);
      return 4;
    }
    if (prime && !prime_audit.complete) {
      std::fputs("prime_alignment_incomplete\n", stderr);
      return 5;
    }
    if (!matches) {
      std::fputs("contribution_mismatch\n", stderr);
      return 3;
    }
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "common_mix_failed: %s\n", error.what());
    return 1;
  } catch (...) {
    std::fputs("common_mix_failed: unknown_exception\n", stderr);
    return 1;
  }
}
