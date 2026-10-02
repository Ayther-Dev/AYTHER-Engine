#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace qa_lifecycle {
struct Event {
  const char *kind{};
  std::uint64_t occurrence{};
  std::uint64_t key{};
  std::uint64_t frame{};
  std::uint64_t output{};
  std::size_t before{};
  std::size_t after{};
};
struct Sample {
  std::uint64_t occurrence{};
  std::uint64_t key{};
  std::uint64_t output{};
  std::size_t before{};
  std::size_t source{};
  std::size_t after{};
  float gain{};
  std::uint32_t fade_left{};
};
struct TickContext {
  std::uint64_t occurrence{};
  std::uint64_t key{};
  std::uint64_t frame{};
  std::uint64_t output{};
  std::uint64_t end_frame{};
  std::uint64_t cut_frame{};
  std::uint32_t fade_frames{};
  std::uint32_t fade_left{};
  bool looping{};
  std::size_t position{};
};
std::array<TickContext, 32> contexts{};
std::size_t context_count{};
std::array<Event, 32> events{};
std::size_t event_count{};
std::array<Sample, 256> samples{};
std::size_t sample_count{};
std::uint64_t frame{};
std::uint64_t boundary{};
bool lose{};
bool overflow{};
void event(Event value) noexcept {
  if (lose && std::string_view(value.kind) == "loop_wrap" &&
      value.output == 2) {
    return;
  }
  if (lose && std::string_view(value.kind) == "asset_end" &&
      value.occurrence == 1) {
    return;
  }
  if (lose && std::string_view(value.kind) == "cut_frame") {
    return;
  }
  if (event_count == events.size()) {
    overflow = true;
    return;
  }
  events[event_count++] = value;
}
void sample(Sample value) noexcept {
  if (sample_count == samples.size()) {
    overflow = true;
    return;
  }
  samples[sample_count++] = value;
}
void context(TickContext value) noexcept {
  if (context_count == contexts.size()) {
    overflow = true;
    return;
  }
  contexts[context_count++] = value;
}
void require(bool valid, const char *message) {
  if (!valid) {
    throw std::runtime_error(message);
  }
}
} // namespace qa_lifecycle

#if QA_LIFECYCLE_OBSERVER
#include <observed_mixer.h>
#else
#include <audio_hd_mixer.h>
#endif

void write_trace(const std::filesystem::path &directory, bool intact) {
  std::ofstream file;
  file.exceptions(std::ios::failbit | std::ios::badbit);
  file.open(directory / "trace.toml");
  file.precision(17);
  file << "schema_version = 1\ntrace_complete = " << (intact ? "true" : "false")
       << '\n';
  for (std::size_t i = 0; i < qa_lifecycle::event_count; ++i) {
    const auto &e = qa_lifecycle::events[i];
    file << "\n[[events]]\nkind = \"" << e.kind
         << "\"\noccurrence = " << e.occurrence << "\nkey = " << e.key
         << "\nframe = " << e.frame << "\noutput = " << e.output
         << "\nposition_before = " << e.before
         << "\nposition_after = " << e.after << '\n';
  }
  for (std::size_t i = 0; i < qa_lifecycle::sample_count; ++i) {
    const auto &s = qa_lifecycle::samples[i];
    file << "\n[[samples]]\noccurrence = " << s.occurrence
         << "\nkey = " << s.key << "\noutput = " << s.output
         << "\nposition_before = " << s.before << "\nsource = " << s.source
         << "\nposition_after = " << s.after << "\ngain = " << s.gain
         << "\nfade_left = " << s.fade_left << '\n';
  }
  for (std::size_t i = 0; i < qa_lifecycle::context_count; ++i) {
    const auto &c = qa_lifecycle::contexts[i];
    file << "\n[[tick_contexts]]\nvisit_order = " << i
         << "\noccurrence = " << c.occurrence << "\nkey = " << c.key
         << "\nframe = " << c.frame << "\noutput = " << c.output
         << "\nend_frame = \"" << c.end_frame << "\"\ncut_frame = \""
         << c.cut_frame << "\"\nfade_frames = " << c.fade_frames
         << "\nfade_left = " << c.fade_left
         << "\nlooping = " << (c.looping ? "true" : "false")
         << "\nposition = " << c.position << '\n';
  }
  file.close();
}

int replacement(const std::filesystem::path &directory, bool lose) {
  using qa_lifecycle::require;
  qa_lifecycle::lose = lose;
  auto writable = std::make_shared<std::vector<std::int16_t>>(128);
  for (std::size_t i = 0; i < writable->size(); ++i) {
    (*writable)[i] = static_cast<std::int16_t>(1000 + 100 * i);
  }
  const HdMixPcm pcm = writable;
  HdMixer mixer;
  std::array<std::int16_t, 144> output{};
  output.fill(100);
  require(
      mixer.start(101, pcm, 0, 0, 0.5F, false, false, UINT64_MAX, UINT64_MAX),
      "first_voice_failed");
  mixer.mix_into(output.data(), 8, 0);
  qa_lifecycle::frame = 1;
  qa_lifecycle::boundary = 8;
  require(
      mixer.start(101, pcm, 8, 0, 0.75F, false, false, UINT64_MAX, UINT64_MAX),
      "replacement_voice_failed");
  require(mixer.voice_count() == 2, "replacement_not_coexistent");
  mixer.mix_into(output.data() + 16, 64, 8);
  require(mixer.voice_count() == 0 && mixer.started() == 2,
          "reference_end_differs");
  bool intact = !qa_lifecycle::overflow && qa_lifecycle::sample_count == 128 &&
                qa_lifecycle::event_count == 3;
  std::array<std::size_t, 2> next{};
  for (std::size_t i = 0; i < qa_lifecycle::sample_count; ++i) {
    const auto &s = qa_lifecycle::samples[i];
    require(s.occurrence >= 1 && s.occurrence <= 2, "invalid_occurrence");
    const auto id = static_cast<std::size_t>(s.occurrence - 1);
    intact = intact && s.key == 101 && s.source == next[id]++ &&
             s.before == s.source && s.after == s.source + 1 &&
             s.output == s.source + (id ? 8 : 0);
    if (id == 1) {
      intact = intact && s.gain == 0.75F && s.fade_left == 0;
    } else if (s.output < 8) {
      intact = intact && s.gain == 0.5F && s.fade_left == 0;
    } else {
      intact = intact && s.gain > 0 && s.gain <= 0.5F &&
               s.fade_left == 2645 - (s.output - 8);
    }
  }
  intact = intact && next == std::array<std::size_t, 2>{64, 64};
  std::array<bool, 3> seen{};
  for (std::size_t i = 0; i < qa_lifecycle::event_count; ++i) {
    const auto &e = qa_lifecycle::events[i];
    if (std::string_view(e.kind) == "key_stop_fade") {
      intact = intact && !seen[0] && e.occurrence == 1 && e.output == 8 &&
               e.before == 8;
      seen[0] = true;
    } else if (std::string_view(e.kind) == "asset_end" && e.occurrence >= 1 &&
               e.occurrence <= 2) {
      const auto id = static_cast<std::size_t>(e.occurrence);
      intact = intact && !seen[id] && e.output == (id == 1 ? 64U : 72U) &&
               e.before == 64 && e.after == 64;
      seen[id] = true;
    } else {
      intact = false;
    }
  }
  intact = intact && seen == std::array<bool, 3>{true, true, true};
  std::ofstream file;
  file.exceptions(std::ios::failbit | std::ios::badbit);
  file.open(directory / "output.s16le", std::ios::binary);
  file.write(reinterpret_cast<const char *>(output.data()), sizeof(output));
  file.close();
  file.open(directory / "run.toml");
  file << "schema_version = 1\nkey = 101\nasset_frames = 64\nasset_sample_base "
          "= "
          "1000\nasset_sample_step = 100\noriginal_sample = 100\nsample_rate = "
          "44100\nchannels = "
          "2\nlooping = false\nend_frame = \"18446744073709551615\"\ncut_frame "
          "= "
          "\"18446744073709551615\"\nvoices_started = "
       << mixer.started() << "\nvoices_final = " << mixer.voice_count()
       << "\n[[operations]]\nkind = \"start\"\nframe = 0\noutput = 0\noffset = "
          "0\ngain = "
          "0.5\n[[operations]]\nkind = \"mix\"\nframes = "
          "8\n[[operations]]\nkind = "
          "\"start\"\nframe = 1\noutput = 8\noffset = 0\ngain = "
          "0.75\n[[operations]]\nkind = "
          "\"mix\"\nframes = 64\n";
  file.close();
  write_trace(directory, intact);
  if (QA_LIFECYCLE_OBSERVER && !intact) {
    std::fputs("lifecycle_trace_incomplete\n", stderr);
    return 2;
  }
  require(QA_LIFECYCLE_OBSERVER || (qa_lifecycle::event_count == 0 &&
                                    qa_lifecycle::sample_count == 0),
          "reference_instrumented");
  return 0;
}

int natural(const std::filesystem::path &directory, bool lose) {
  using qa_lifecycle::require;
  qa_lifecycle::lose = lose;
  auto writable = std::make_shared<std::vector<std::int16_t>>(16);
  for (std::size_t i = 0; i < writable->size(); ++i) {
    (*writable)[i] = static_cast<std::int16_t>(1000 + 100 * i);
  }
  const HdMixPcm pcm = writable;
  HdMixer mixer;
  std::array<std::int16_t, 32> output{};
  output.fill(100);
  require(
      mixer.start(101, pcm, 4, 0, 0.5F, false, false, UINT64_MAX, UINT64_MAX),
      "natural_voice_failed");
  mixer.mix_into(output.data(), 16, 0);
  require(mixer.voice_count() == 0 && mixer.started() == 1,
          "natural_end_differs");
  bool intact = !qa_lifecycle::overflow && qa_lifecycle::sample_count == 8 &&
                qa_lifecycle::event_count == 1;
  for (std::size_t i = 0; i < qa_lifecycle::sample_count; ++i) {
    const auto &s = qa_lifecycle::samples[i];
    intact = intact && s.occurrence == 1 && s.key == 101 && s.output == i + 4 &&
             s.before == i && s.source == i && s.after == i + 1 &&
             s.gain == 0.5F && s.fade_left == 0;
  }
  if (qa_lifecycle::event_count == 1) {
    const auto &e = qa_lifecycle::events[0];
    intact = intact && std::string_view(e.kind) == "asset_end" &&
             e.occurrence == 1 && e.key == 101 && e.output == 12 &&
             e.before == 8 && e.after == 8;
  }
  for (std::size_t i = 0; i < output.size(); ++i) {
    const int expected = (i < 8 || i >= 24)
                             ? 100
                             : 100 + (1000 + 100 * static_cast<int>(i - 8)) / 2;
    require(output[i] == expected, "original_hd_contribution_differs");
  }
  std::ofstream file;
  file.exceptions(std::ios::failbit | std::ios::badbit);
  file.open(directory / "output.s16le", std::ios::binary);
  file.write(reinterpret_cast<const char *>(output.data()), sizeof(output));
  file.close();
  file.open(directory / "run.toml");
  file
      << "schema_version = 1\nkey = 101\nasset_frames = 8\nasset_sample_base = "
         "1000\nasset_sample_step = 100\noriginal_sample = 100\nsample_rate = "
         "44100\nchannels = "
         "2\nlooping = false\nend_frame = \"18446744073709551615\"\ncut_frame "
         "= "
         "\"18446744073709551615\"\nvoices_started = 1\nvoices_final = 0\n"
         "[[operations]]\nkind = \"start\"\nframe = 0\noutput = 4\noffset = "
         "0\ngain = "
         "0.5\n[[operations]]\nkind = \"mix\"\nframes = 16\n";
  file.close();
  write_trace(directory, intact);
  if (QA_LIFECYCLE_OBSERVER && !intact) {
    std::fputs("lifecycle_trace_incomplete\n", stderr);
    return 2;
  }
  require(QA_LIFECYCLE_OBSERVER || (qa_lifecycle::event_count == 0 &&
                                    qa_lifecycle::sample_count == 0),
          "reference_instrumented");
  return 0;
}

int simultaneous(const std::filesystem::path &directory, bool lose) {
  using qa_lifecycle::require;
  qa_lifecycle::lose = lose;
  auto writable = std::make_shared<std::vector<std::int16_t>>(32);
  for (std::size_t i = 0; i < writable->size(); ++i) {
    (*writable)[i] = static_cast<std::int16_t>(1000 + 100 * i);
  }
  const HdMixPcm pcm = writable;
  HdMixer mixer;
  std::array<std::int16_t, 16> output{};
  output.fill(100);
  require(mixer.start(101, pcm, 0, 0, 0.5F, true, true, 1, 1, 4),
          "fade_voice_failed");
  require(mixer.start(202, pcm, 0, 0, 0.25F, true, true, 1, 1),
          "cut_voice_failed");
  mixer.mix_into(output.data(), 4, 0);
  qa_lifecycle::frame = 2;
  qa_lifecycle::boundary = 4;
  mixer.tick_frame(2);
  require(mixer.voice_count() == 1, "simultaneous_tick_differs");
  mixer.mix_into(output.data() + 8, 4, 4);
  require(mixer.voice_count() == 0 && mixer.started() == 2,
          "simultaneous_end_differs");
  bool intact = !qa_lifecycle::overflow && qa_lifecycle::sample_count == 12 &&
                qa_lifecycle::event_count == 3 &&
                qa_lifecycle::context_count == 2;
  if (qa_lifecycle::context_count == 2) {
    for (std::size_t i = 0; i < 2; ++i) {
      const auto &c = qa_lifecycle::contexts[i];
      intact = intact && c.occurrence == 2 - i && c.key == (i ? 101U : 202U) &&
               c.frame == 2 && c.output == 4 && c.end_frame == 1 &&
               c.cut_frame == 1 && c.fade_frames == (i ? 4U : 0U) &&
               c.fade_left == 0 && c.looping && c.position == 4;
    }
  }
  if (qa_lifecycle::event_count == 3) {
    constexpr std::array<std::string_view, 3> kinds{
        "cut_frame", "authored_fade_begin", "fade_complete"};
    for (std::size_t i = 0; i < 3; ++i) {
      const auto &e = qa_lifecycle::events[i];
      intact = intact && e.kind == kinds[i] && e.occurrence == (i ? 1U : 2U) &&
               e.frame == 2 && e.output == (i == 2 ? 8U : 4U) &&
               e.before == (i == 2 ? 8U : 4U) && e.after == e.before;
    }
  }
  std::array<std::size_t, 2> next{};
  for (std::size_t i = 0; i < qa_lifecycle::sample_count; ++i) {
    const auto &s = qa_lifecycle::samples[i];
    require(s.occurrence >= 1 && s.occurrence <= 2, "invalid_occurrence");
    const auto id = static_cast<std::size_t>(s.occurrence - 1);
    const auto pos = next[id]++;
    const float gain =
        id == 1 ? 0.25F
                : (pos < 4 ? 0.5F : 0.5F * static_cast<float>(8 - pos) / 4.0F);
    intact = intact && s.before == pos && s.source == pos &&
             s.after == pos + 1 && s.output == pos && s.gain == gain &&
             s.fade_left == (id == 0 && pos >= 4 ? 7 - pos : 0);
  }
  intact = intact && next == std::array<std::size_t, 2>{8, 4};
  std::ofstream file;
  file.exceptions(std::ios::failbit | std::ios::badbit);
  file.open(directory / "output.s16le", std::ios::binary);
  file.write(reinterpret_cast<const char *>(output.data()), sizeof(output));
  file.close();
  file.open(directory / "run.toml");
  file
      << "schema_version = 1\nasset_frames = 16\nasset_sample_base = "
         "1000\nasset_sample_step = 100\noriginal_sample = 100\nsample_rate = "
         "44100\nchannels = "
         "2\nvoices_started = 2\nvoices_final = 0\n"
         "[[operations]]\nkind = \"start\"\nkey = 101\noutput = 0\noffset = "
         "0\ngain = "
         "0.5\nlooping = true\nend_frame = 1\ncut_frame = 1\nfade_frames = 4\n"
         "[[operations]]\nkind = \"start\"\nkey = 202\noutput = 0\noffset = "
         "0\ngain = "
         "0.25\nlooping = true\nend_frame = 1\ncut_frame = 1\nfade_frames = 0\n"
         "[[operations]]\nkind = \"mix\"\nframes = 4\n[[operations]]\nkind = "
         "\"tick\"\nframe = 2\noutput = 4\n[[operations]]\nkind = "
         "\"mix\"\nframes = 4\n";
  file.close();
  write_trace(directory, intact);
  if (QA_LIFECYCLE_OBSERVER && !intact) {
    std::fputs("lifecycle_trace_incomplete\n", stderr);
    return 2;
  }
  require(QA_LIFECYCLE_OBSERVER || (qa_lifecycle::event_count == 0 &&
                                    qa_lifecycle::sample_count == 0 &&
                                    qa_lifecycle::context_count == 0),
          "reference_instrumented");
  return 0;
}

int main(int argc, char *argv[]) {
  using qa_lifecycle::require;
  try {
    require(argc == 3, "invalid_arguments");
    const std::filesystem::path directory{argv[1]};
    const std::string_view mode{argv[2]};
    require(mode == "loop" || mode == "window" || mode == "lost" ||
                mode == "replacement" || mode == "replacement_lost" ||
                mode == "natural" || mode == "natural_lost" ||
                mode == "simultaneous" || mode == "simultaneous_lost",
            "invalid_case");
    for (const char *name : {"output.s16le", "run.toml", "trace.toml"}) {
      require(!std::filesystem::exists(directory / name), "output_exists");
    }
    if (mode == "replacement" || mode == "replacement_lost") {
      return replacement(directory, mode == "replacement_lost");
    }
    if (mode == "natural" || mode == "natural_lost") {
      return natural(directory, mode == "natural_lost");
    }
    if (mode == "simultaneous" || mode == "simultaneous_lost") {
      return simultaneous(directory, mode == "simultaneous_lost");
    }
    const bool closes = mode == "window";
    qa_lifecycle::lose = mode == "lost";
    auto writable = std::make_shared<std::vector<std::int16_t>>(16);
    for (std::size_t i = 0; i < writable->size(); ++i) {
      (*writable)[i] = static_cast<std::int16_t>(1000 + 100 * i);
    }
    const HdMixPcm pcm = writable;
    HdMixer mixer;
    const std::uint64_t end = closes ? 2 : UINT64_MAX;
    require(
        mixer.start(101, pcm, 0, 4, 0.5F, true, true, end, UINT64_MAX, 0, 2, 6),
        "voice_start_failed");
    std::array<std::int16_t, 48> output{};
    output.fill(100);
    constexpr std::array<std::size_t, 4> counts{8, 8, 4, 4};
    std::array<std::size_t, 4> voice_counts{};
    for (std::size_t f = 0; f < counts.size(); ++f) {
      qa_lifecycle::frame = f;
      mixer.tick_frame(f);
      voice_counts[f] = mixer.voice_count();
      mixer.mix_into(output.data() + qa_lifecycle::boundary * 2, counts[f],
                     qa_lifecycle::boundary);
      qa_lifecycle::boundary += counts[f];
    }
    require(voice_counts == (closes ? std::array<std::size_t, 4>{1, 1, 1, 0}
                                    : std::array<std::size_t, 4>{1, 1, 1, 1}),
            "reference_window_differs");
    bool intact = !qa_lifecycle::overflow &&
                  qa_lifecycle::sample_count == (closes ? 20U : 24U) &&
                  qa_lifecycle::event_count == 6;
    for (std::size_t i = 0; i < qa_lifecycle::sample_count; ++i) {
      const auto &s = qa_lifecycle::samples[i];
      const std::size_t source = 2 + (i + 2) % 4;
      const std::size_t before = i == 0 ? 4 : 2 + (i + 1) % 4 + 1;
      intact = intact && s.occurrence == 1 && s.key == 101 && s.output == i &&
               s.source == source && s.before == before &&
               s.after == source + 1 && s.gain == 0.5F && s.fade_left == 0;
    }
    for (std::size_t i = 0; i < qa_lifecycle::event_count; ++i) {
      const auto &e = qa_lifecycle::events[i];
      if (closes && i == 5) {
        intact = intact && std::string_view(e.kind) == "window_end" &&
                 e.frame == 3 && e.output == 20 && e.before == 4 &&
                 e.after == 4;
      } else {
        intact = intact && std::string_view(e.kind) == "loop_wrap" &&
                 e.output == 2 + i * 4 && e.before == 6 && e.after == 2;
      }
    }
    std::ofstream file;
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.open(directory / "output.s16le", std::ios::binary);
    file.write(reinterpret_cast<const char *>(output.data()), sizeof(output));
    file.close();
    file.open(directory / "run.toml");
    file << "schema_version = 1\nkey = 101\nasset_frames = "
            "8\nasset_sample_base = "
            "1000\nasset_sample_step = 100\noriginal_sample = 100\nloop_begin "
            "= 2\nloop_end = "
            "6\noffset = 4\ngain = 0.5\nend_frame = \""
         << end
         << "\"\ncut_frame = \"18446744073709551615\"\nsample_rate = "
            "44100\nchannels = 2\n";
    std::size_t boundary = 0;
    for (std::size_t f = 0; f < counts.size(); ++f) {
      file << "\n[[frames]]\nframe = " << f << "\noutput_begin = " << boundary
           << "\noutput_frames = " << counts[f]
           << "\nvoices_after_tick = " << voice_counts[f] << '\n';
      boundary += counts[f];
    }
    file.close();
    write_trace(directory, intact);
    if (QA_LIFECYCLE_OBSERVER && !intact) {
      std::fputs("lifecycle_trace_incomplete\n", stderr);
      return 2;
    }
    require(QA_LIFECYCLE_OBSERVER || (qa_lifecycle::event_count == 0 &&
                                      qa_lifecycle::sample_count == 0),
            "reference_instrumented");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "lifecycle_probe_failed: %s\n", error.what());
    return 1;
  }
}
