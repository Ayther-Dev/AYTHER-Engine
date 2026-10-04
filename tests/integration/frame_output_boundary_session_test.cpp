// Spec 002 (P-9, DI-12): a session with an audio observer reports, for every
// audible frame, where the frame starts on the device line
// (`audio_frame_output_boundary` on `engine_main_output`), even with no HD
// voice in the mix. Silent production (DI-8) delivers nothing to the device
// and reports no boundary.
//
// SDL's dummy driver opens at 44.1 kHz unless a logical device asks for more;
// one is held at 48 kHz so the session's device resamples.
#include <ayther/ayther_session.h>

#include "../../tools/common/synth_rom.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

using ayther::AudioOutputMode;
using ayther::AytherSession;
namespace obs = ayther::engine::audio_observation;

constexpr int kFrames = 120;

struct Boundary {
  std::uint64_t frame = 0;
  std::uint64_t position = 0;
  std::uint64_t rate = 0;
  bool timeline = false;
  bool valid = false;
};

struct Sink {
  std::mutex lock;
  std::vector<Boundary> boundaries;
  std::uint64_t pcm_end = 0;
  std::size_t output_spans = 0;

  static void fact(void *context, const obs::FactView &fact) noexcept {
    auto &sink = *static_cast<Sink *>(context);
    const std::lock_guard guard(sink.lock);
    if (fact.kind == "main_mix_output_span")
      ++sink.output_spans;
    if (fact.kind != "audio_frame_output_boundary")
      return;
    Boundary b;
    b.frame = fact.frame.emulation_frame;
    for (const auto &field : fact.fields) {
      if (field.name == "output_timeline") {
        const auto *v = std::get_if<std::string_view>(&field.value);
        b.timeline = v && *v == "engine_main_output";
      } else if (field.name == "output_position") {
        const auto *v = std::get_if<std::uint64_t>(&field.value);
        b.position = v ? *v : 0;
      } else if (field.name == "sample_rate") {
        const auto *v = std::get_if<std::uint64_t>(&field.value);
        b.rate = v ? *v : 0;
      } else if (field.name == "valid") {
        const auto *v = std::get_if<bool>(&field.value);
        b.valid = v && *v;
      }
    }
    b.valid = b.valid && fact.frame.availability == obs::Availability::known;
    sink.boundaries.push_back(b);
  }
  static void pcm(void *context, const obs::PcmView &view) noexcept {
    auto &sink = *static_cast<Sink *>(context);
    const std::lock_guard guard(sink.lock);
    sink.pcm_end = view.range.end;
  }
  std::size_t count() {
    const std::lock_guard guard(lock);
    return boundaries.size();
  }
};

std::unique_ptr<AytherSession> open(const std::string &rom, Sink &sink) {
  AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = rom;
  config.enable_audio = true;
  config.derive_core_pack = false;
  config.audio_observer = {&sink, &Sink::fact, &Sink::pcm};
  auto created = AytherSession::create(config);
  if (!created) {
    std::fprintf(stderr, "[FAIL] %s\n", created.error.message.c_str());
    return nullptr;
  }
  return std::move(*created);
}

void run(std::unique_ptr<AytherSession> &s) {
  for (int f = 0; f < kFrames; ++f) {
    s->set_input(0, static_cast<std::uint16_t>(f & 3));
    (void)s->step();
  }
}

void wait(Sink &sink, std::size_t at_least, int milliseconds) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(milliseconds);
  while (sink.count() < at_least && std::chrono::steady_clock::now() < deadline)
    SDL_Delay(5);
}
} // namespace

int main() try {
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    return 2;
  const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, 48000};
  const SDL_AudioDeviceID holder =
      SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
  check(holder != 0, "a 48 kHz logical device is held");

  const std::string rom = ayther::synth::canonical_rom_path();
  check(!rom.empty(), "the synthetic ROM is written");

  // 1. Audible: every frame that reaches the device reports its boundary.
  {
    Sink sink;
    auto s = open(rom, sink);
    if (!s)
      return 1;
    run(s);
    wait(sink, kFrames - 10, 8000);
    const std::lock_guard guard(sink.lock);
    const auto &b = sink.boundaries;
    std::printf("  audible: boundaries=%zu output_spans=%zu pcm_end=%llu\n",
                b.size(), sink.output_spans,
                static_cast<unsigned long long>(sink.pcm_end));
    if (!b.empty())
      std::printf("  first: frame %llu at %llu; last: frame %llu at %llu\n",
                  static_cast<unsigned long long>(b.front().frame),
                  static_cast<unsigned long long>(b.front().position),
                  static_cast<unsigned long long>(b.back().frame),
                  static_cast<unsigned long long>(b.back().position));
    bool fields = !b.empty();
    bool ordered = true;
    for (std::size_t i = 0; i < b.size(); ++i) {
      fields = fields && b[i].timeline && b[i].valid && b[i].rate == 48000 &&
               b[i].position < sink.pcm_end;
      if (i > 0)
        ordered = ordered && b[i].frame == b[i - 1].frame + 1 &&
                  b[i].position >= b[i - 1].position;
    }
    check(b.size() >= static_cast<std::size_t>(kFrames - 10),
          "P-9: the audible frames report their boundary on the device line");
    check(sink.output_spans == 0,
          "with no HD voice no main_mix_output_span is emitted (the boundary "
          "does not depend on it)");
    check(fields, "each boundary is valid, at 48 kHz, on engine_main_output, "
                  "inside the PCM delivered");
    check(ordered, "frames are consecutive and their positions never go back");
  }

  // 2. Silent production: no PCM reaches the device, no boundary is reported.
  {
    Sink sink;
    auto s = open(rom, sink);
    if (!s)
      return 1;
    s->set_audio_output_mode(AudioOutputMode::silent);
    run(s);
    wait(sink, 1, 1000);
    std::printf("  silent: boundaries=%zu\n", sink.count());
    check(sink.count() == 0,
          "DI-8: silent production emits no audio_frame_output_boundary");
  }

  SDL_CloseAudioDevice(holder);
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
