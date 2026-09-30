#include "observer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {
constexpr std::size_t input_frames = 1024;
constexpr std::size_t maximum_frames = 4096;
constexpr Sint64 phase_unit = Sint64{1} << 32;
static_assert(std::endian::native == std::endian::little);

struct Observation {
  std::array<AytherQaConversion, 128> rows{};
  std::array<float, maximum_frames * 2> pcm{};
  std::size_t count{};
  std::size_t frames{};
  bool invalid{};
  std::string_view fault;
  std::size_t callbacks{};
  AytherQaReset reset{};
  std::size_t reset_count{};
  std::size_t reset_row{};

  static void SDLCALL receive_reset(void *userdata,
                                    const AytherQaReset *event) noexcept {
    auto &self = *static_cast<Observation *>(userdata);
    if (self.fault == "reset_drop") {
      return;
    }
    self.reset = *event;
    self.reset.stream = nullptr;
    self.reset_row = self.count;
    ++self.reset_count;
    if (self.fault == "reset_phase") {
      self.reset.phase_after = self.reset.phase_before;
    }
  }

  static void SDLCALL receive(void *userdata,
                              const AytherQaConversion *event) noexcept {
    auto &self = *static_cast<Observation *>(userdata);
    ++self.callbacks;
    if (self.fault == "drop" && self.callbacks == 1) {
      return;
    }
    if (self.count == self.rows.size() || event->output_frames <= 0 ||
        event->destination.format != SDL_AUDIO_F32 ||
        event->destination.channels != 2 ||
        static_cast<std::size_t>(event->output_frames) >
            maximum_frames - self.frames) {
      self.invalid = true;
      return;
    }
    self.rows[self.count] = *event;
    // Do not retain borrowed pointers after this callback.
    self.rows[self.count].output = nullptr;
    self.rows[self.count].stream = nullptr;
    if (self.fault == "phase" && self.callbacks == 1) {
      ++self.rows[self.count].phase_after;
    }
    ++self.count;
    const auto count = static_cast<std::size_t>(event->output_frames) * 2;
    std::memcpy(self.pcm.data() + self.frames * 2, event->output,
                count * sizeof(float));
    self.frames += static_cast<std::size_t>(event->output_frames);
  }
};

void require(bool value, const char *message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}

struct SdlLifetime {
  SdlLifetime() { require(SDL_Init(0), "sdl_init_failed"); }
  ~SdlLifetime() { SDL_Quit(); }
  SdlLifetime(const SdlLifetime &) = delete;
  SdlLifetime &operator=(const SdlLifetime &) = delete;
};

struct ObserverRegistration {
  AytherQaSetConversionObserver setter{};
  AytherQaSetResetObserver reset_setter{};
  ObserverRegistration(AytherQaSetConversionObserver function,
                       AytherQaSetResetObserver reset_function,
                       Observation &state)
      : setter(function), reset_setter(reset_function) {
    if (setter != nullptr) {
      setter(Observation::receive, &state);
    }
    if (reset_setter != nullptr) {
      reset_setter(Observation::receive_reset, &state);
    }
  }
  ~ObserverRegistration() {
    if (setter != nullptr) {
      setter(nullptr, nullptr);
    }
    if (reset_setter != nullptr) {
      reset_setter(nullptr, nullptr);
    }
  }
  ObserverRegistration(const ObserverRegistration &) = delete;
  ObserverRegistration &operator=(const ObserverRegistration &) = delete;
};
} // namespace

int main(int argc, char *argv[]) {
  try {
    require(argc == 4 || argc == 5, "invalid_arguments");
    const std::string_view fault = argc == 5 ? argv[4] : "";
    require(fault.empty() || fault == "drop" || fault == "phase" ||
                fault == "reset_drop" || fault == "reset_phase",
            "invalid_fault");
    const std::string_view mode{argv[3]};
    require(mode == "numeric" || mode == "resample" || mode == "drc" ||
                mode == "reset",
            "invalid_case");
    const std::filesystem::path directory{argv[2]};
    require(!std::filesystem::exists(directory / "conversion.f32le") &&
                !std::filesystem::exists(directory / "conversion.toml"),
            "output_exists");
    std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
    input.exceptions(std::ios::failbit | std::ios::badbit);
    require(input.tellg() == static_cast<std::streamoff>(input_frames * 4),
            "input_size");
    std::array<std::int16_t, input_frames * 2> samples{};
    input.seekg(0);
    input.read(reinterpret_cast<char *>(samples.data()), sizeof(samples));

    SdlLifetime sdl;
    using Library =
        std::unique_ptr<SDL_SharedObject, decltype(&SDL_UnloadObject)>;
    Library library(SDL_LoadObject("SDL3.dll"), SDL_UnloadObject);
    require(library != nullptr, "library_load_failed");
    const auto setter = reinterpret_cast<AytherQaSetConversionObserver>(
        SDL_LoadFunction(library.get(), "AYTHER_QA_SetConversionObserver"));
    const auto reset_setter = reinterpret_cast<AytherQaSetResetObserver>(
        SDL_LoadFunction(library.get(), "AYTHER_QA_SetResetObserver"));
    require((setter != nullptr) == (QA_EXPECT_OBSERVER != 0),
            "observer_identity_mismatch");
    require((reset_setter != nullptr) == (QA_EXPECT_OBSERVER != 0),
            "reset_observer_identity_mismatch");
    SDL_ClearError();
    Observation observation;
    observation.fault = fault;
    ObserverRegistration registration(setter, reset_setter, observation);
    const SDL_AudioSpec source{SDL_AUDIO_S16, 2, 44100};
    const SDL_AudioSpec destination{
        SDL_AUDIO_F32, 2,
        mode == "resample" || mode == "reset" ? 48000 : 44100};
    using Stream =
        std::unique_ptr<SDL_AudioStream, decltype(&SDL_DestroyAudioStream)>;
    Stream stream(SDL_CreateAudioStream(&source, &destination),
                  SDL_DestroyAudioStream);
    require(stream != nullptr, "stream_create_failed");
    require(SDL_SetAudioStreamGain(stream.get(), 0.75F), "gain_failed");
    if (mode == "drc") {
      require(
          SDL_SetAudioStreamFrequencyRatio(stream.get(), 1.000083327293396F),
          "initial_ratio_failed");
    }
    require(
        SDL_PutAudioStreamData(stream.get(), samples.data(), sizeof(samples)),
        "put_failed");
    require(SDL_FlushAudioStream(stream.get()), "flush_failed");

    std::array<float, maximum_frames * 2> output{};
    constexpr std::array<int, 4> requests{97, 131, 67, 251};
    std::size_t frames = 0;
    std::size_t calls = 0;
    for (;;) {
      const auto requested = requests[calls % requests.size()];
      require(static_cast<std::size_t>(requested) <= maximum_frames - frames,
              "output_limit");
      const int bytes = SDL_GetAudioStreamData(
          stream.get(), output.data() + frames * 2, requested * 8);
      require(bytes >= 0 && bytes % 8 == 0, "get_failed");
      if (bytes == 0) {
        break;
      }
      frames += static_cast<std::size_t>(bytes) / 8;
      ++calls;
      if (mode == "reset" && calls == 1) {
        require(SDL_ClearAudioStream(stream.get()), "clear_failed");
        std::reverse(samples.begin(), samples.end());
        require(SDL_PutAudioStreamData(stream.get(), samples.data(),
                                       sizeof(samples)),
                "refill_failed");
        require(SDL_FlushAudioStream(stream.get()), "refill_eos_failed");
      }
      if (mode == "drc" && calls == 1) {
        require(SDL_SetAudioStreamFrequencyRatio(stream.get(), 1.0075F),
                "changed_ratio_failed");
      }
    }
    require(frames > 0 && SDL_GetAudioStreamAvailable(stream.get()) == 0,
            "drain_failed");
    require(!observation.invalid, "observation_overflow");
    if (setter != nullptr) {
      require(observation.frames == frames && observation.count > 1,
              "coverage_incomplete");
      require(std::memcmp(output.data(), observation.pcm.data(), frames * 8) ==
                  0,
              "observed_pcm_differs");
      Sint64 previous_phase = 0;
      bool fractional_phase = false;
      bool rate_changed = false;
      if (mode == "reset") {
        require(observation.reset_count == 1 && observation.reset_row == 1,
                "reset_observation_missing");
        require(observation.reset.phase_before != 0 &&
                    observation.reset.phase_after == 0 &&
                    observation.reset.queued_bytes_after == 0,
                "reset_state_invalid");
        const auto consumed = observation.rows[0].input_frames;
        require(consumed > 0 && consumed < static_cast<int>(input_frames) &&
                    observation.reset.queued_bytes_before ==
                        (input_frames - consumed) * 4 &&
                    observation.reset.phase_before ==
                        observation.rows[0].phase_after,
                "discard_range_invalid");
      } else {
        require(observation.reset_count == 0, "unexpected_reset");
      }
      for (std::size_t index = 0; index < observation.count; ++index) {
        if (mode == "reset" && index == observation.reset_row) {
          previous_phase = observation.reset.phase_after;
        }
        const auto &row = observation.rows[index];
        require(row.source.freq == source.freq &&
                    row.destination.freq == destination.freq &&
                    row.source.format == source.format && row.gain == 0.75F,
                "observed_format_or_gain_differs");
        require(row.phase_before == previous_phase, "phase_discontinuity");
        const auto stride = row.rate == 0 ? phase_unit : row.rate;
        require(row.phase_after == row.phase_before +
                                       row.output_frames * stride -
                                       row.input_frames * phase_unit,
                "phase_conservation_failed");
        fractional_phase = fractional_phase || row.phase_after != 0;
        rate_changed =
            rate_changed || (index > 0 && row.rate != observation.rows[0].rate);
        previous_phase = row.phase_after;
      }
      require((mode == "numeric") != fractional_phase,
              "fractional_phase_not_exercised");
      require((mode == "drc") == rate_changed, "rate_change_not_exercised");
    }

    std::ofstream pcm;
    pcm.exceptions(std::ios::failbit | std::ios::badbit);
    pcm.open(directory / "conversion.f32le", std::ios::binary);
    pcm.write(reinterpret_cast<const char *>(output.data()),
              static_cast<std::streamsize>(frames * 8));
    pcm.close();
    std::ofstream manifest;
    manifest.exceptions(std::ios::failbit | std::ios::badbit);
    manifest.open(directory / "conversion.toml");
    manifest.precision(17);
    manifest << "schema_version = 1\ncase = \"" << mode
             << "\"\noutput_frames = " << frames << "\nobserver_enabled = "
             << (setter != nullptr ? "true" : "false") << '\n';
    if (mode == "reset" && setter != nullptr) {
      manifest << "\n[reset]\noutput_boundary = "
               << observation.rows[0].output_frames
               << "\ndiscard_input_begin = " << observation.rows[0].input_frames
               << "\ndiscard_input_end = " << input_frames
               << "\nnew_input_origin = " << input_frames
               << "\nphase_before_q32 = " << observation.reset.phase_before
               << "\nphase_after_q32 = " << observation.reset.phase_after
               << "\nqueued_bytes_before = "
               << observation.reset.queued_bytes_before
               << "\nqueued_bytes_after = "
               << observation.reset.queued_bytes_after
               << "\nrefill_transform = \"reverse_interleaved_values\"\n";
    }
    std::size_t input_begin = 0;
    std::size_t output_begin = 0;
    for (std::size_t index = 0; index < observation.count; ++index) {
      if (mode == "reset" && index == observation.reset_row) {
        input_begin = input_frames;
      }
      const auto &row = observation.rows[index];
      manifest << "\n[[spans]]\ninput_begin = " << input_begin
               << "\ninput_frames = " << row.input_frames
               << "\noutput_begin = " << output_begin
               << "\noutput_frames = " << row.output_frames
               << "\nrate_q32 = " << row.rate
               << "\nphase_before_q32 = " << row.phase_before
               << "\nphase_after_q32 = " << row.phase_after
               << "\npadding_frames = " << row.padding_frames
               << "\ngain = " << row.gain << '\n';
      input_begin += static_cast<std::size_t>(row.input_frames);
      output_begin += static_cast<std::size_t>(row.output_frames);
    }
    manifest.close();
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "conversion_probe_failed: %s\n", error.what());
    return 1;
  } catch (...) {
    std::fputs("conversion_probe_failed: unknown_exception\n", stderr);
    return 1;
  }
}
