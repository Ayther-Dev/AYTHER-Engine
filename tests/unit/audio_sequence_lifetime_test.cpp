#include "audio_sequence_lifetime.h"

#include <array>
#include <cstdio>

namespace {

void check(const bool condition, const char *message, int &failures) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

} // namespace

int main() {
  using ayther::ActiveAudioSignature;
  using ayther::SequencePresence;
  constexpr std::uint64_t wilderness = 0x05d714c8e7a9ba44ULL;
  constexpr std::array<std::uint64_t, 3> members{wilderness, 0x111ULL,
                                                 0x222ULL};
  constexpr std::uint32_t music_channels = (1U << 1U) | (1U << 2U);
  int failures = 0;

  check(ayther::sequence_presence(
            wilderness, members, music_channels,
            std::array{ActiveAudioSignature{0x111ULL, 1U << 1U},
                       ActiveAudioSignature{0x999ULL, 1U << 5U}}) ==
            SequencePresence::member_active,
        "Wilderness member keeps its HD sequence alive", failures);
  check(ayther::sequence_presence(
            wilderness, members, music_channels,
            std::array{ActiveAudioSignature{0x47414d454f564552ULL, 1U << 1U}}) ==
            SequencePresence::replacement_active,
        "Game Over music on an owned channel replaces Wilderness", failures);
  check(ayther::sequence_presence(
            wilderness, members, music_channels,
            std::array{ActiveAudioSignature{0x53574f5244ULL, 1U << 5U}}) ==
            SequencePresence::no_relevant_activity,
        "an unrelated sword effect does not end Wilderness", failures);
  check(ayther::sequence_presence(
            wilderness, members, music_channels,
            std::array{ActiveAudioSignature{0x4d454e55534658ULL,
                                            1U << 1U, false}}) ==
            SequencePresence::no_relevant_activity,
        "a known title/menu effect on the music channel does not restart The Battle",
        failures);
  check(ayther::sequence_presence(wilderness, members, music_channels,
                                  std::array<ActiveAudioSignature, 0>{}) ==
            SequencePresence::no_relevant_activity,
        "an empty detector frame is not treated as a terminal signal", failures);

  return failures == 0 ? 0 : 1;
}
