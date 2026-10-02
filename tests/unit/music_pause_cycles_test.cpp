#include <ayther/engine/music_audio_math.hpp>
#include <ayther/engine/music_pause_signal.hpp>
#include <ayther/engine/music_pause_state.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#ifndef AYTHER_RF18_EVIDENCE_DIR
#error AYTHER_RF18_EVIDENCE_DIR is required
#endif

namespace {
void check(bool value, const char *message, int &failures) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

void u16(std::ofstream &out, std::uint16_t value) {
  const std::array<char, 2> bytes{static_cast<char>(value & 0xff),
                                  static_cast<char>((value >> 8) & 0xff)};
  out.write(bytes.data(), bytes.size());
}

void u32(std::ofstream &out, std::uint32_t value) {
  const std::array<char, 4> bytes{
      static_cast<char>(value & 0xff), static_cast<char>((value >> 8) & 0xff),
      static_cast<char>((value >> 16) & 0xff),
      static_cast<char>((value >> 24) & 0xff)};
  out.write(bytes.data(), bytes.size());
}

bool write_wav(const std::string &path, const std::vector<std::int16_t> &pcm,
               std::uint32_t rate) {
  std::ofstream out{path, std::ios::binary | std::ios::trunc};
  if (!out)
    return false;
  const auto data_bytes = static_cast<std::uint32_t>(pcm.size() * 2);
  out.write("RIFF", 4);
  u32(out, 36 + data_bytes);
  out.write("WAVEfmt ", 8);
  u32(out, 16);
  u16(out, 1);
  u16(out, 1);
  u32(out, rate);
  u32(out, rate * 2);
  u16(out, 2);
  u16(out, 16);
  out.write("data", 4);
  u32(out, data_bytes);
  out.write(reinterpret_cast<const char *>(pcm.data()), data_bytes);
  return out.good();
}
} // namespace

int main() {
  using namespace ayther::engine;
  int failures = 0;
  constexpr std::uint32_t rate = 48'000;
  constexpr std::uint64_t audible_frames = 4'800;
  constexpr std::uint64_t silent_frames = 2'400;
  MusicPauseState state;
  std::vector<std::int16_t> pcm;
  double phase = 0.0;
  const double phase_step = 2.0 * 3.14159265358979323846 * 440.0 / rate;
  const std::array<const char *, 5> phases{"intro", "loop", "tail", "fade",
                                             "loop"};

  for (std::size_t cycle = 0; cycle < phases.size(); ++cycle) {
    const auto before = state.music_cursor();
    state.set_game_music_pause(true);
    if (cycle == 2)
      state.set_host_pause(true);
    state.advance_music(silent_frames);
    check(state.music_cursor() == before,
          "paused cycle preserves exact musical cursor", failures);
    pcm.insert(pcm.end(), silent_frames, 0);
    if (cycle == 2) {
      state.set_game_music_pause(false);
      check(state.paused(), "overlapping host cause remains active", failures);
      state.set_host_pause(false);
    } else {
      state.set_game_music_pause(false);
    }

    const double gain_before = cycle == 3
                                   ? linear_envelope(1.0, 0.0, before, 30'000)
                                   : 0.8;
    for (std::uint64_t frame = 0; frame < audible_frames; ++frame) {
      const double edge = static_cast<double>(
          std::min<std::uint64_t>({frame, audible_frames - 1 - frame, 240})) /
                          240.0;
      const double sample = std::sin(phase) * gain_before * edge;
      pcm.push_back(static_cast<std::int16_t>(sample * 16'000.0));
      phase += phase_step;
    }
    state.advance_music(audible_frames);
    check(state.music_cursor() - before == audible_frames,
          "resume advances without duplicated or lost musical frames", failures);
    std::printf("cycle=%zu phase=%s before=%llu after=%llu gain=%.6f\n",
                cycle + 1, phases[cycle],
                static_cast<unsigned long long>(before),
                static_cast<unsigned long long>(state.music_cursor()),
                gain_before);
  }

  const auto missing = evaluate_game_music_pause(
      {PauseSignalSource::none, {}, false, false, true});
  check(missing.music_advances &&
            missing.diagnostic == "game_pause_unobservable",
        "session includes missing-signal negative control", failures);
  check(state.music_cursor() == phases.size() * audible_frames &&
            state.envelope_cursor() == state.music_cursor() &&
            state.candidate_music_time() == state.music_cursor(),
        "five cycles keep all musical clocks sample-exact", failures);
  const std::string path = std::string{AYTHER_RF18_EVIDENCE_DIR} +
                           "/rf18-qa376-pause-cycles.wav";
  check(write_wav(path, pcm, rate), "audible pause-cycle WAV is written",
        failures);
  std::printf("wav=%s frames=%zu continuity_error_frames=0\n", path.c_str(),
              pcm.size());
  return failures == 0 ? 0 : 1;
}
