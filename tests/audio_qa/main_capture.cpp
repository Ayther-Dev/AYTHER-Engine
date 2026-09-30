#include <SDL3/SDL.h>
#include <ayther/audio_player.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
constexpr std::size_t input_frames = 1024;
constexpr std::size_t capture_limit = 32768;
constexpr std::size_t capture_target = 16384;
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(std::endian::native == std::endian::little);

struct Block {
  std::size_t begin{};
  std::size_t frames{};
};

struct Capture {
  std::vector<float> pcm = std::vector<float>(capture_limit * 2);
  std::array<Block, 256> blocks{};
  std::size_t frames{};
  std::size_t block_count{};
  std::size_t target = capture_target;
  bool invalid{};
  std::atomic<bool> done{false};

  static void SDLCALL observe(void *userdata, const SDL_AudioSpec *spec,
                              float *buffer, int bytes) {
    auto &capture = *static_cast<Capture *>(userdata);
    if (capture.done.load(std::memory_order_relaxed)) {
      return;
    }
    if (spec->format != SDL_AUDIO_F32 || spec->channels != 2 ||
        spec->freq != 44100 || bytes <= 0 || bytes % 8 != 0) {
      capture.invalid = true;
      capture.done.store(true, std::memory_order_release);
      return;
    }
    const auto frames = static_cast<std::size_t>(bytes) / 8;
    if (frames > capture_limit - capture.frames ||
        capture.block_count == capture.blocks.size()) {
      capture.invalid = true;
      capture.done.store(true, std::memory_order_release);
      return;
    }
    std::memcpy(capture.pcm.data() + capture.frames * 2, buffer,
                static_cast<std::size_t>(bytes));
    capture.blocks[capture.block_count++] = {capture.frames, frames};
    capture.frames += frames;
    if (capture.frames >= capture.target) {
      capture.done.store(true, std::memory_order_release);
    }
  }
};

struct SdlLifetime {
  SdlLifetime() {
    if (!SDL_Init(SDL_INIT_AUDIO)) {
      throw std::runtime_error(SDL_GetError());
    }
  }
  ~SdlLifetime() { SDL_Quit(); }
  SdlLifetime(const SdlLifetime &) = delete;
  SdlLifetime &operator=(const SdlLifetime &) = delete;
};

struct Observer {
  SDL_AudioDeviceID device;
  Observer(SDL_AudioDeviceID id, Capture &capture) : device(id) {
    if (!SDL_SetAudioPostmixCallback(device, Capture::observe, &capture)) {
      throw std::runtime_error(SDL_GetError());
    }
  }
  void detach() {
    if (device != 0) {
      if (!SDL_SetAudioPostmixCallback(device, nullptr, nullptr)) {
        throw std::runtime_error(SDL_GetError());
      }
      device = 0;
    }
  }
  ~Observer() {
    if (device != 0) {
      (void)SDL_SetAudioPostmixCallback(device, nullptr, nullptr);
    }
  }
  Observer(const Observer &) = delete;
  Observer &operator=(const Observer &) = delete;
};

std::array<std::int16_t, input_frames * 2>
read_input(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  input.exceptions(std::ios::failbit | std::ios::badbit);
  if (input.tellg() != static_cast<std::streamoff>(input_frames * 4)) {
    throw std::runtime_error("invalid_input_size");
  }
  std::array<std::int16_t, input_frames * 2> pcm{};
  input.seekg(0);
  input.read(reinterpret_cast<char *>(pcm.data()),
             static_cast<std::streamsize>(sizeof(pcm)));
  return pcm;
}

// A content check, not an oracle for resampler phase or exact effect
// boundaries.
struct Match {
  std::size_t begin{};
  double rms = std::numeric_limits<double>::infinity();
};

Match locate_input(const Capture &capture,
                   const std::array<std::int16_t, input_frames * 2> &input) {
  Match best;
  for (std::size_t begin = 0; begin + input_frames <= capture.frames; ++begin) {
    double squared_error = 0.0;
    for (std::size_t index = 0; index < input.size(); ++index) {
      const double expected = static_cast<double>(input[index]) / 32768.0;
      const double error =
          static_cast<double>(capture.pcm[begin * 2 + index]) - expected;
      squared_error += error * error;
    }
    const double rms =
        std::sqrt(squared_error / static_cast<double>(input.size()));
    if (rms < best.rms) {
      best = {begin, rms};
    }
  }
  return best;
}

struct ExactMatch {
  std::size_t begin{};
  std::size_t count{};
};

ExactMatch locate_auxiliary(const Capture &capture,
                            const std::vector<float> &expected) {
  ExactMatch result;
  const auto frames = expected.size() / 2;
  for (std::size_t begin = 0; begin + frames <= capture.frames; ++begin) {
    if (std::equal(expected.begin(), expected.end(),
                   capture.pcm.begin() +
                       static_cast<std::ptrdiff_t>(begin * 2))) {
      result.begin = begin;
      ++result.count;
    }
  }
  return result;
}
} // namespace

int main(int argc, char *argv[]) {
  try {
    if (argc != 4) {
      std::fputs("invalid_arguments: input_pcm output_directory case\n",
                 stderr);
      return 2;
    }
    const std::string_view mode{argv[3]};
    const bool prime_case = mode == "prime" || mode == "prime_missing";
    const bool closing_case = mode == "closing" || mode == "closing_discard";
    const bool conversion_case =
        mode == "conversion" || mode == "conversion_missing" || closing_case;
    const bool auxiliary_case =
        mode == "auxiliary" || mode == "auxiliary_missing" || prime_case;
    if (mode != "known" && mode != "silence" && !auxiliary_case &&
        !conversion_case) {
      std::fputs("invalid_case\n", stderr);
      return 2;
    }
    const std::filesystem::path output_dir{argv[2]};
    if (std::filesystem::exists(output_dir / "capture.f32le") ||
        std::filesystem::exists(output_dir / "capture.toml")) {
      throw std::runtime_error("output_exists");
    }
    const auto known = read_input(argv[1]);
    const bool negative_control = std::string(argv[3]) == "silence";
    auto submitted = known;
    if (negative_control) {
      submitted.fill(0);
    }

    SdlLifetime sdl;
    if (std::string(SDL_GetCurrentAudioDriver()) != "dummy") {
      throw std::runtime_error("controlled_dummy_driver_required");
    }
    Capture capture;
    AudioPlayer player;
    if (!player.init(ayther::RuntimeOptions{})) {
      throw std::runtime_error("audio_player_init_failed");
    }
    if (closing_case) {
      capture.target = input_frames;
      if (!SDL_PauseAudioDevice(player.device_id()) ||
          !SDL_AudioDevicePaused(player.device_id())) {
        throw std::runtime_error("initial_pause_failed");
      }
    }
    Observer observer(player.device_id(), capture);
    std::vector<float> auxiliary(input_frames * 2);
    for (std::size_t frame = 0; frame < input_frames; ++frame) {
      if (conversion_case) {
        auxiliary[frame * 2] = static_cast<float>(known[frame * 2]) / 32768.0F;
        auxiliary[frame * 2 + 1] =
            static_cast<float>(known[frame * 2 + 1]) / 32768.0F;
      } else {
        auxiliary[frame * 2] =
            static_cast<float>(known[frame * 2 + 1]) / 131072.0F;
        auxiliary[frame * 2 + 1] =
            -static_cast<float>(known[frame * 2]) / 131072.0F;
      }
    }
    if (mode == "conversion" || closing_case) {
      player.play_oneshot_pcm(known.data(), input_frames);
    }
    const bool pending_at_close = closing_case && player.preview_playing();
    if (closing_case) {
      if (!pending_at_close) {
        throw std::runtime_error("no_audio_pending_at_close");
      }
      if (mode == "closing_discard") {
        player.stop_oneshot();
      }
      if (!SDL_ResumeAudioDevice(player.device_id())) {
        throw std::runtime_error("close_drain_resume_failed");
      }
    }
    constexpr std::size_t prime_frames = 512;
    if (prime_case) {
      const auto prefix = auxiliary;
      auxiliary.resize((input_frames * 2 + prime_frames) * 2, 0.0F);
      for (std::size_t frame = 0; frame < input_frames; ++frame) {
        const auto source = input_frames - 1 - frame;
        const auto destination = input_frames + prime_frames + frame;
        auxiliary[destination * 2] = -prefix[source * 2];
        auxiliary[destination * 2 + 1] = -prefix[source * 2 + 1];
      }
    }
    if (mode == "auxiliary" || prime_case) {
      player.feed_synth(auxiliary.data(), input_frames);
    }
    if (prime_case) {
      if (mode == "prime") {
        player.prime_synth(prime_frames);
      }
      player.feed_synth(auxiliary.data() + (input_frames + prime_frames) * 2,
                        input_frames);
    }
    const auto timeline_begin = player.timeline_samples();
    if (!closing_case) {
      player.mark_frame_boundary();
      player.buffer_emulator(0x5141303135ULL, submitted.data(), input_frames);
      player.flush_emulator();
    }
    const auto timeline_end = player.timeline_samples();
    const auto ratio = player.drc_ratio();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!capture.done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      SDL_Delay(1);
    }
    observer.detach(); // SDL waits for the current callback before releasing
                       // its borrowed state.

    const auto match = locate_input(capture, known);
    const auto auxiliary_match = locate_auxiliary(capture, auxiliary);
    const bool finite = std::all_of(
        capture.pcm.begin(),
        capture.pcm.begin() + static_cast<std::ptrdiff_t>(capture.frames * 2),
        [](float value) { return std::isfinite(value); });
    const bool capture_ok =
        !capture.invalid && capture.frames >= capture.target && finite &&
        timeline_begin == 0 &&
        timeline_end == (closing_case ? 0 : input_frames) &&
        (!closing_case ||
         (capture.frames == input_frames && !player.preview_playing()));
    const bool matched = capture_ok && match.rms < 0.025;
    std::ofstream pcm;
    pcm.exceptions(std::ios::failbit | std::ios::badbit);
    pcm.open(output_dir / "capture.f32le", std::ios::binary);
    pcm.write(reinterpret_cast<const char *>(capture.pcm.data()),
              static_cast<std::streamsize>(capture.frames * 2 * sizeof(float)));
    pcm.close();

    std::ofstream manifest;
    manifest.exceptions(std::ios::failbit | std::ios::badbit);
    manifest.open(output_dir / "capture.toml");
    manifest.precision(17);
    manifest << "schema_version = 1\ncapture_point = "
                "\"sdl_logical_device_postmix\"\n"
             << "format = \"f32le\"\nsample_rate = 44100\nchannels = "
                "2\nsample_begin = 0\n"
             << "sample_count = " << capture.frames
             << "\ninput_fixture = \"known-pcm-v1\"\n"
             << "input_case = \"" << argv[3]
             << "\"\ninput_timeline_begin = " << timeline_begin
             << "\ninput_timeline_end = " << timeline_end
             << "\ndrc_ratio_after_flush = " << ratio
             << "\ncontent_match = " << (matched ? "true" : "false")
             << "\nmatch_sample_begin = " << match.begin
             << "\nmatch_rms = " << match.rms
             << "\nmatch_rms_limit = 0.025\nexact_effect_alignment_verified = "
                "false\n"
             << "driver = \"dummy\"\nauxiliary_input_submitted = "
             << ((mode == "auxiliary" || prime_case || mode == "conversion" ||
                  closing_case)
                     ? "true"
                     : "false")
             << "\nauxiliary_exact_match_count = " << auxiliary_match.count
             << "\nauxiliary_sample_begin = " << auxiliary_match.begin
             << "\nauxiliary_sample_count = " << auxiliary.size() / 2
             << "\nprime_frames_submitted = "
             << (mode == "prime" ? prime_frames : 0)
             << "\nprime_exact_span_verified = "
             << ((prime_case && auxiliary_match.count == 1) ? "true" : "false")
             << "\nprime_span_available = "
             << ((prime_case && auxiliary_match.count == 1) ? "true" : "false");
    if (prime_case && auxiliary_match.count == 1) {
      manifest << "\nprime_sample_begin = "
               << auxiliary_match.begin + input_frames
               << "\nprime_sample_count = " << prime_frames;
    }
    manifest << "\nconversion_exact_span_verified = "
             << ((conversion_case && auxiliary_match.count == 1) ? "true"
                                                                 : "false");
    if (conversion_case && auxiliary_match.count == 1) {
      manifest << "\nconversion_source_format = "
                  "\"s16le\"\nconversion_output_format = \"f32le\""
               << "\nconversion_input_begin = 0\nconversion_frame_count = "
               << input_frames
               << "\nconversion_output_begin = " << auxiliary_match.begin;
    }
    manifest << "\nclosing_case = " << (closing_case ? "true" : "false")
             << "\npending_at_close = " << (pending_at_close ? "true" : "false")
             << "\nclosing_span_verified = "
             << ((closing_case && capture_ok && auxiliary_match.count == 1)
                     ? "true"
                     : "false");
    if (closing_case) {
      manifest
          << "\nproduced_limit = " << input_frames
          << "\ninput_submissions_after_close = 0\ngame_steps_executed = 0";
    }
    manifest << '\n';
    for (std::size_t index = 0; index < capture.block_count; ++index) {
      manifest << "\n[[blocks]]\nsample_begin = " << capture.blocks[index].begin
               << "\nsample_count = " << capture.blocks[index].frames << '\n';
    }
    manifest.close();
    if (!capture_ok) {
      std::cerr << "capture_incomplete\n";
      return 1;
    }
    if (!matched) {
      std::cerr << "known_input_not_found\n";
      return 3;
    }
    if ((auxiliary_case || conversion_case) && auxiliary_match.count != 1) {
      std::cerr << "auxiliary_input_not_found\n";
      return 4;
    }
    std::cout << "main_block_observed\n";
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "main_capture_failed: %s\n", error.what());
    return 1;
  } catch (...) {
    std::fputs("main_capture_failed: unknown_exception\n", stderr);
    return 1;
  }
}
