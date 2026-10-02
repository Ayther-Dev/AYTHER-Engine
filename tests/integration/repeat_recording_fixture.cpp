#include <ayther/ayther_recording.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace {

template <class T>
bool repeat_values(std::vector<T> &values, const std::uint32_t repetitions) {
  const auto original = values;
  if (original.size() > std::numeric_limits<std::size_t>::max() / repetitions)
    return false;
  values.clear();
  values.reserve(original.size() * repetitions);
  for (std::uint32_t i = 0; i < repetitions; ++i)
    values.insert(values.end(), original.begin(), original.end());
  return true;
}

template <class T>
bool repeat_csr(std::vector<T> &values, std::vector<std::uint32_t> &offsets,
                const std::uint32_t frames, const std::uint32_t repetitions) {
  if (offsets.empty() && values.empty())
    return true;
  if (offsets.size() != static_cast<std::size_t>(frames) + 1U ||
      offsets.back() != values.size())
    return false;
  const auto original_values = values;
  const auto original_offsets = offsets;
  values.clear();
  offsets.clear();
  offsets.reserve(static_cast<std::size_t>(frames) * repetitions + 1U);
  offsets.push_back(0U);
  for (std::uint32_t repetition = 0; repetition < repetitions; ++repetition) {
    const auto base = static_cast<std::uint32_t>(values.size());
    values.insert(values.end(), original_values.begin(), original_values.end());
    for (std::uint32_t frame = 1; frame <= frames; ++frame)
      offsets.push_back(base + original_offsets[frame]);
  }
  return true;
}

} // namespace

int main(const int argc, char *argv[]) {
  if (argc != 4) {
    std::fprintf(stderr,
                 "usage: repeat_recording_fixture INPUT OUTPUT COUNT\n");
    return 2;
  }
  const auto repetitions =
      static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10));
  if (repetitions < 2U || repetitions > 16U)
    return 2;
  auto recording = ayther::AytherRecording::load(argv[1]);
  if (!recording || recording->empty())
    return 3;
  const auto frames = recording->frame_count();
  if (frames > std::numeric_limits<std::uint32_t>::max() / repetitions)
    return 4;
  if (!repeat_values(recording->inputs, repetitions) ||
      !repeat_values(recording->stats, repetitions) ||
      !repeat_csr(recording->sprite_hashes, recording->hash_offsets, frames,
                  repetitions) ||
      !repeat_csr(recording->audio_hashes, recording->audio_offsets, frames,
                  repetitions))
    return 5;
  recording->keyframes.clear();
  recording->name += " x" + std::to_string(repetitions) + " continuous";
  recording->trim_in = 0U;
  recording->trim_out = recording->frame_count();
  if (!recording->save(argv[2]))
    return 6;
  const auto verified = ayther::AytherRecording::load(argv[2]);
  if (!verified || verified->frame_count() != frames * repetitions ||
      verified->initial_state != recording->initial_state)
    return 7;
  std::printf("repeat_recording_fixture: %u -> %u frames, one initial state\n",
              frames, verified->frame_count());
  return 0;
}
