#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
struct Sample {
  std::uint64_t occurrence{};
  std::uint64_t key{};
  std::uint64_t output{};
  std::size_t cursor_before{};
  std::size_t source{};
  std::size_t cursor_after{};
  float gain{};
  std::uint32_t fade_left{};
  std::int16_t source_left{};
  std::int16_t source_right{};
  std::int16_t before_left{};
  std::int16_t before_right{};
  std::int16_t after_left{};
  std::int16_t after_right{};
};
std::array<Sample, 4096> samples{};
std::size_t count{};
bool overflow{};
bool lose_record{};
bool record_lost{};
[[maybe_unused]] void qa_record(const Sample &sample) noexcept {
  if (lose_record && sample.occurrence == 1 && sample.output == 80) {
    record_lost = true;
    return;
  }
  if (count == samples.size()) {
    overflow = true;
    return;
  }
  samples[count++] = sample;
}
void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}
} // namespace

#include <audio_hd_mixer.h>

int main(int argc, char *argv[]) {
  try {
    require(argc == 3, "invalid_arguments");
    const std::filesystem::path directory{argv[1]};
    const std::string_view mode{argv[2]};
    require(mode == "complete" || mode == "lost", "invalid_case");
    for (const auto *name : {"mixer.s16le", "voices.toml"}) {
      require(!std::filesystem::exists(directory / name), "output_exists");
    }
    lose_record = mode == "lost";
    std::array<std::int16_t, 2048> output{};
    for (std::size_t index = 0; index < output.size(); ++index) {
      output[index] =
          static_cast<std::int16_t>(static_cast<int>(index % 17) * 100 - 800);
    }
    const auto initial = output;
    auto writable_pcm = std::make_shared<std::vector<std::int16_t>>(34);
    for (std::size_t index = 0; index < writable_pcm->size(); ++index) {
      (*writable_pcm)[index] = static_cast<std::int16_t>(1000 + 137 * index);
    }
    const HdMixPcm pcm = writable_pcm;
    HdMixer mixer;
    require(mixer.start(101, pcm, 13, 0, 0.5F, true, false, UINT64_MAX,
                        UINT64_MAX, 0, 3, 11),
            "loop_start_failed");
    require(
        mixer.start(202, pcm, 0, 3, 0.0F, false, false, UINT64_MAX, UINT64_MAX),
        "silent_start_failed");
    mixer.mix_into(output.data(), 128, 0);
    require(mixer.start(101, pcm, 210, 7, 0.3F, false, false, UINT64_MAX,
                        UINT64_MAX),
            "retrigger_failed");
    require(mixer.set_gain(101, 0.75F), "new_voice_gain_failed");
    require(mixer.start(303, pcm, 300, 0, 1.0F, true, true, 2, 2, 8),
            "fade_start_failed");
    mixer.mix_into(output.data() + 256, 128, 128);
    mixer.tick_frame(3);
    mixer.mix_into(output.data() + 512, 256, 256);
    mixer.mix_into(output.data() + 1024, 256, 512);
    mixer.cut_all();
    mixer.mix_into(output.data() + 1536, 256, 768);
    require(mixer.started() == 4 && mixer.voice_count() == 0 &&
                mixer.mixed_samples() == 1024,
            "fixture_lifecycle_differs");

    auto verified = initial;
    std::array<std::size_t, 4> per_voice{};
    std::array<std::size_t, 4> next_source{0, 3, 7, 0};
    std::array<std::uint64_t, 4> next_output{13, 0, 210, 300};
    bool intact = !overflow;
    bool wrapped = false;
    bool faded = false;
    bool coexist = false;
    for (std::size_t index = 0; index < count; ++index) {
      const auto &s = samples[index];
      require(s.occurrence >= 1 && s.occurrence <= 4 && s.output < 1024,
              "invalid_observed_identity");
      const auto id = static_cast<std::size_t>(s.occurrence - 1);
      intact = intact && s.cursor_before == next_source[id] &&
               s.output == next_output[id] && s.cursor_after == s.source + 1;
      next_source[id] = s.cursor_after;
      next_output[id] = s.output + 1;
      ++per_voice[id];
      const auto at = static_cast<std::size_t>(s.output) * 2;
      intact = intact && verified[at] == s.before_left &&
               verified[at + 1] == s.before_right;
      verified[at] = s.after_left;
      verified[at + 1] = s.after_right;
      if (s.occurrence == 1) {
        intact = intact && s.key == 101 && s.source < 11;
        wrapped = wrapped || (s.cursor_before == 11 && s.source == 3);
        faded = faded || (s.output > 128 && s.gain < 0.5F && s.fade_left > 0);
        coexist = coexist || (s.output >= 210 && s.output < 220);
      } else if (s.occurrence == 2) {
        intact = intact && s.gain == 0 && s.before_left == s.after_left &&
                 s.before_right == s.after_right;
      } else if (s.occurrence == 3) {
        intact = intact && s.key == 101 && s.gain == 0.75F;
      } else {
        intact = intact &&
                 s.gain == static_cast<float>(308 - s.output) / 8.0F &&
                 s.fade_left == 307 - s.output;
      }
    }
    intact = intact && verified == output &&
             per_voice == std::array<std::size_t, 4>{755, 14, 10, 8} &&
             wrapped && faded && coexist;
    std::ofstream file;
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.open(directory / "mixer.s16le", std::ios::binary);
    file.write(reinterpret_cast<const char *>(output.data()), sizeof(output));
    file.close();
    file.open(directory / "voices.toml");
    file.precision(17);
    file << "schema_version = 1\nobserver_enabled = "
         << (QA_EXPECT_OBSERVER ? "true" : "false")
         << "\nsource_rate = 44100\nchannels = 2\nframes = 1024\nrecords = "
         << count << "\ntrace_complete = " << (intact ? "true" : "false")
         << "\nrecord_lost = " << (record_lost ? "true" : "false") << '\n';
    for (std::size_t index = 0; index < count; ++index) {
      const auto &s = samples[index];
      file << "\n[[samples]]\noccurrence = " << s.occurrence
           << "\nkey = " << s.key << "\noutput = " << s.output
           << "\ncursor_before = " << s.cursor_before
           << "\nsource = " << s.source << "\ncursor_after = " << s.cursor_after
           << "\ngain = " << s.gain << "\nfade_left = " << s.fade_left
           << "\nsource_values = [" << s.source_left << ", " << s.source_right
           << "]"
           << "\nbefore_values = [" << s.before_left << ", " << s.before_right
           << "]"
           << "\nafter_values = [" << s.after_left << ", " << s.after_right
           << "]\n";
    }
    file.close();
    if (QA_EXPECT_OBSERVER && !intact) {
      std::fputs("voice_trace_incomplete\n", stderr);
      return 2;
    }
    require(QA_EXPECT_OBSERVER || count == 0, "reference_was_instrumented");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "voice_probe_failed: %s\n", error.what());
    return 1;
  }
}
