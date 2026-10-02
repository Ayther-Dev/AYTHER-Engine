#include <ayther/engine/music_audio_math.hpp>

#include <cmath>
#include <cstdio>

namespace {
void check(bool value, const char *message, int &failures) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}
} // namespace

int main() {
  using namespace ayther::engine;
  int failures = 0;
  check(region_frame_count({10, 13}) == 3,
        "half-open region contains no duplicated endpoint", failures);
  check(convert_frame_count(1, 96'000, 44'100) == 1,
        "positive sub-sample duration rounds up to one", failures);
  check(convert_frame_count(1, 2, 1) == 1,
        "half tie rounds away from zero", failures);
  check(convert_frame_count(48'000, 48'000, 44'100) == 44'100,
        "resampling preserves one-second duration", failures);
  check(convert_frame_count(0, 48'000, 44'100) == 0,
        "zero duration remains zero", failures);

  constexpr auto fade = envelope_frames(48'000, 5);
  check(fade == 240, "five milliseconds uses musical sample time", failures);
  check(linear_envelope(1.0, 0.0, 0, fade) == 1.0 &&
            std::abs(linear_envelope(1.0, 0.0, fade / 2, fade) - 0.5) <
                1e-12 &&
            linear_envelope(1.0, 0.0, fade, fade) == 0.0,
        "linear envelope includes exact initial midpoint and final values",
        failures);
  check(linear_envelope(1.0, 0.25, 0, 0) == 0.25,
        "zero-duration envelope applies final value", failures);
  check(effective_fade_frames(100, fade) == 100,
        "source shorter than five milliseconds shortens fade", failures);

  check(gain_within_db(1.0, std::pow(10.0, 0.5 / 20.0), 0.5) &&
            !gain_within_db(1.0, std::pow(10.0, 0.51 / 20.0), 0.5),
        "gain comparison enforces plus or minus 0.5 dB", failures);
  check(gain_within_db(0.0, 0.0, 0.5) &&
            !gain_within_db(0.0, 0.0001, 0.5),
        "mute is checked exactly without logarithmic ratio", failures);
  return failures == 0 ? 0 : 1;
}
