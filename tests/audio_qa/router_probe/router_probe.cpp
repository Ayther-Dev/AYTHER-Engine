#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <voice_router.h>

namespace {
enum class Kind { reset, rate, push, sample, trim };
constexpr std::array<const char *, 5> names{"reset", "rate", "push", "sample",
                                            "trim"};
struct Event {
  Kind kind{};
  double before{};
  double after{};
  double step{};
  std::size_t size_before{};
  std::size_t size_after{};
  std::size_t amount{};
  long first{};
  long last{};
  float left{};
  float right{};
  std::size_t sequence{};
  std::size_t block{};
};
std::array<Event, 1024> events{};
std::size_t count{};
std::size_t sequence{};
std::size_t block{};
std::string_view fault;
bool overflow{};
[[maybe_unused]] void qa_record(Event event) noexcept {
  event.sequence = sequence++;
  event.block = block;
  if (fault == "lost" && event.kind == Kind::reset && event.size_before > 0) {
    return;
  }
  if (fault == "phase" && event.kind == Kind::sample && event.amount == 5) {
    event.before += 0.25;
  }
  if (count == events.size()) {
    overflow = true;
    return;
  }
  events[count++] = event;
}
void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}
struct RouterHarness {
  ayther::StreamResampler voice_rs;
  std::vector<float> voice_out;
  std::size_t vr_resyncs{};
  std::size_t vr_starved{};
  std::size_t consume(std::size_t n) {
#include <boundary.inc>
    return got;
  }
};
struct Boundary {
  std::size_t begin{};
  std::size_t requested{};
  std::size_t produced{};
  std::size_t resyncs{};
};
} // namespace

#if QA_EXPECT_OBSERVER
#include <observed.inc>
#else
#include <reference.inc>
#endif

int main(int argc, char *argv[]) {
  try {
    require(argc == 3, "invalid_arguments");
    const std::filesystem::path directory{argv[1]};
    fault = argv[2];
    require(fault == "complete" || fault == "lost" || fault == "phase",
            "invalid_case");
    for (const auto *name : {"router.f32le", "router.toml"}) {
      require(!std::filesystem::exists(directory / name), "output_exists");
    }
    RouterHarness router;
    std::vector<float> output;
    std::vector<Boundary> boundaries;
    std::size_t submitted = 0;
    auto push = [&](std::size_t frames) {
      std::vector<float> input(frames * 2);
      for (std::size_t i = 0; i < frames; ++i) {
        input[i * 2] = static_cast<float>((submitted + i) % 31) / 64.0F;
        input[i * 2 + 1] = -static_cast<float>((submitted + i) % 17) / 64.0F;
      }
      router.voice_rs.push(input.data(), frames);
      submitted += frames;
    };
    auto consume = [&](std::size_t frames) {
      block = boundaries.size();
      const auto got = router.consume(frames);
      require(got <= frames && router.voice_out.size() == frames * 2,
              "invalid_boundary");
      boundaries.push_back({output.size() / 2, frames, got, router.vr_resyncs});
      output.insert(output.end(), router.voice_out.begin(),
                    router.voice_out.end());
    };
    router.voice_rs.reset();
    router.voice_rs.set_rates(53267, 44100);
    push(96);
    consume(32);
    consume(64);
    push(64);
    consume(80);
    router.voice_rs.set_rates(53267, 48000);
    consume(16);
    push(256);
    consume(16);
    push(96);
    consume(40);
    consume(80);
    require(router.vr_resyncs == 1 && router.vr_starved > 0 &&
                output.size() == 656,
            "scenario_not_exercised");

    bool intact = !overflow;
    double phase = 0;
    double step = 1;
    std::size_t queued = 0;
    std::size_t origin = 0;
    std::size_t pushed = 0;
    std::size_t resets = 0;
    std::size_t trims = 0;
    std::size_t sample_count = 0;
    std::array<std::size_t, 7> per_block{};
    std::array<bool, 328> observed{};
    std::array<std::size_t, 1024> input_origins{};
    for (std::size_t i = 0; i < count; ++i) {
      const auto &event = events[i];
      input_origins[i] = origin;
      intact = intact && event.sequence == i && event.before == phase &&
               event.size_before == queued;
      if (event.kind != Kind::rate) {
        intact = intact && event.step == step;
      }
      switch (event.kind) {
      case Kind::rate:
        intact =
            intact && event.before == event.after && event.size_after == queued;
        step = event.step;
        break;
      case Kind::push:
        intact = intact && event.before == event.after &&
                 event.size_after == queued + event.amount;
        pushed += event.amount;
        break;
      case Kind::trim:
        intact =
            intact && event.amount <= queued &&
            event.size_after == queued - event.amount &&
            event.after == event.before - static_cast<double>(event.amount);
        origin += event.amount;
        ++trims;
        break;
      case Kind::reset:
        intact = intact && event.after == 0 && event.size_after == 0;
        origin = pushed;
        ++resets;
        break;
      case Kind::sample: {
        require(event.block < boundaries.size(), "unknown_block");
        const auto &boundary = boundaries[event.block];
        require(event.amount < boundary.produced,
                "sample_outside_produced_range");
        const auto at = boundary.begin + event.amount;
        intact =
            intact && !observed[at] && event.amount == per_block[event.block] &&
            event.size_after == queued && event.after == event.before + step &&
            event.first ==
                std::max(0L, static_cast<long>(std::floor(event.before)) - 8) &&
            event.last ==
                std::min(static_cast<long>(queued) - 1,
                         static_cast<long>(std::floor(event.before)) + 7) &&
            output[at * 2] == event.left && output[at * 2 + 1] == event.right;
        observed[at] = true;
        ++per_block[event.block];
        ++sample_count;
        break;
      }
      }
      phase = event.after;
      queued = event.size_after;
    }
    for (std::size_t i = 0; i < boundaries.size(); ++i) {
      const auto &b = boundaries[i];
      intact = intact && per_block[i] == b.produced;
      for (auto j = b.produced; j < b.requested; ++j) {
        const auto at = b.begin + j;
        intact = intact && !observed[at] && output[at * 2] == 0 &&
                 output[at * 2 + 1] == 0;
      }
    }
    intact = intact && pushed == submitted && resets == 2 && trims > 0 &&
             sample_count > 0;
    std::ofstream file;
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.open(directory / "router.f32le", std::ios::binary);
    file.write(reinterpret_cast<const char *>(output.data()),
               static_cast<std::streamsize>(output.size() * sizeof(float)));
    file.close();
    file.open(directory / "router.toml");
    file.precision(17);
    file << "schema_version = 1\nobserver_enabled = "
         << (QA_EXPECT_OBSERVER ? "true" : "false")
         << "\ntrace_complete = " << (intact ? "true" : "false")
         << "\noutput_frames = 328\ninput_frames = " << submitted
         << "\nresyncs = " << router.vr_resyncs
         << "\nstarved = " << router.vr_starved << '\n';
    for (const auto &b : boundaries) {
      file << "\n[[blocks]]\noutput_begin = " << b.begin
           << "\nrequested = " << b.requested << "\nproduced = " << b.produced
           << "\nresyncs = " << b.resyncs << '\n';
    }
    for (std::size_t i = 0; i < count; ++i) {
      const auto &e = events[i];
      file << "\n[[events]]\nkind = \""
           << names[static_cast<std::size_t>(e.kind)]
           << "\"\nsequence = " << e.sequence << "\nblock = " << e.block
           << "\ninput_origin = " << input_origins[i]
           << "\nphase_before = " << e.before << "\nphase_after = " << e.after
           << "\nstep = " << e.step << "\nsize_before = " << e.size_before
           << "\nsize_after = " << e.size_after << "\namount = " << e.amount
           << "\nsupport_first = " << e.first << "\nsupport_last = " << e.last
           << "\nvalues = [" << e.left << ", " << e.right << "]\n";
    }
    file.close();
    if (QA_EXPECT_OBSERVER && !intact) {
      std::fputs("router_trace_incomplete\n", stderr);
      return 2;
    }
    require(QA_EXPECT_OBSERVER || count == 0, "reference_was_instrumented");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "router_probe_failed: %s\n", error.what());
    return 1;
  }
}
