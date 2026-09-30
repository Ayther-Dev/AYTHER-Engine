#include <audio_live_resume.h>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace {
template <class T> T number(std::string_view text) {
  T value{};
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    throw std::runtime_error("invalid_number");
  }
  return value;
}
} // namespace

int main(int argc, char *argv[]) {
  try {
    if (argc != 9) {
      throw std::runtime_error("invalid_arguments");
    }
    const auto frame = number<std::uint64_t>(argv[1]);
    const auto start = number<std::uint64_t>(argv[2]);
    const auto end = number<std::uint64_t>(argv[3]);
    const auto cut = number<std::uint64_t>(argv[4]);
    const auto looping = number<unsigned>(argv[5]);
    const auto fps = number<double>(argv[6]);
    const auto duration = number<double>(argv[7]);
    if (looping > 1 || !std::isfinite(fps) || !std::isfinite(duration)) {
      throw std::runtime_error("invalid_fixture");
    }
    const std::filesystem::path output{argv[8]};
    if (std::filesystem::exists(output)) {
      throw std::runtime_error("output_exists");
    }
    // Invoke the pinned production rule directly; no replica or inferred
    // reason.
    const auto decision = ayther::live_resume_decide(
        frame, start, end, cut, looping != 0, fps, duration);
    std::ofstream file;
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.open(output);
    file.precision(17);
    file
        << "schema_version = 1\ncomponent = \"live_resume_decide\"\naction = \""
        << (decision.action == ayther::LiveResumeAction::Restart ? "restart"
                                                                 : "finished")
        << "\"\noffset_seconds = " << decision.offset_seconds
        << "\nreason_observed = false\nplayer_effect_observed = false\n";
    file.close();
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "reference_decision_failed: %s\n", error.what());
    return 1;
  }
}
