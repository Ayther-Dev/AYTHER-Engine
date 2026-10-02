#include "audio_observation_ids.h"

#include <array>
#include <limits>
#include <thread>
#include <type_traits>

namespace qa = ayther::audio_qa;
namespace obs = ayther::engine::audio_observation;

static_assert(!std::is_copy_constructible_v<qa::IdentitySource>);
static_assert(!std::is_move_constructible_v<qa::IdentitySource>);

namespace {
bool independent_producers() {
  qa::IdentitySource identities;
  std::array<obs::FactId, 1000> pack{};
  std::array<obs::FactId, 1000> selection{};
  const auto fill = [&](qa::Producer producer, auto &output) {
    for (auto &fact : output) {
      fact = identities.next_fact(producer).value_or(obs::FactId{});
    }
  };
  {
    std::jthread first([&] { fill(qa::Producer::pack, pack); });
    std::jthread second([&] { fill(qa::Producer::selection, selection); });
  }
  for (std::size_t i = 0; i < pack.size(); ++i) {
    if (pack[i] != obs::FactId{2, i + 1} ||
        selection[i] != obs::FactId{5, i + 1} || pack[i] == selection[i]) {
      return false;
    }
  }
  // Deliberately malformed enum values exercise rejection before indexing.
  // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
  if (identities.next_fact(static_cast<qa::Producer>(0)) ||
      identities.next_fact(static_cast<qa::Producer>(10))) {
    return false;
  }
  // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
  for (std::uint32_t number = 1; number <= 9; ++number) {
    const auto fact = identities.next_fact(static_cast<qa::Producer>(number));
    const auto expected = number == 2 || number == 5 ? 1001 : 1;
    if (!fact || fact->producer != number ||
        fact->sequence != static_cast<std::uint64_t>(expected)) {
      return false;
    }
  }
  return true;
}

bool no_wrap() {
  constexpr auto last = std::numeric_limits<std::uint64_t>::max();
  qa::Sequence sequence{last - 1};
  if (sequence.next() != last - 1 || sequence.next() != last) {
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    if (sequence.next()) {
      return false;
    }
  }
  qa::Sequence invalid{0};
  return !invalid.next();
}

bool distinct_voices() {
  qa::IdentitySource identities;
  struct Voice {
    obs::OccurrenceId id;
    std::uint64_t business_key;
    bool active;
  };
  const auto first = identities.next_occurrence();
  const auto second = identities.next_occurrence();
  if (!first || !second || first == second) {
    return false;
  }
  std::array voices{Voice{*first, 77, true}, Voice{*second, 77, true}};
  // Ending one occurrence of the key does not erase the other identity.
  for (auto &voice : voices) {
    if (voice.id == *first) {
      voice.active = false;
    }
  }
  const auto third = identities.next_occurrence();
  // Interleave fact allocation: its counter cannot consume an occurrence ID.
  const auto fact = identities.next_fact(qa::Producer::mixer);
  const auto fourth = identities.next_occurrence();
  return !voices[0].active && voices[1].active &&
         voices[0].business_key == voices[1].business_key &&
         third == obs::OccurrenceId{3} && fourth == obs::OccurrenceId{4} &&
         fact == obs::FactId{6, 1};
}
} // namespace

int main() {
  try {
    return independent_producers() && no_wrap() && distinct_voices() ? 0 : 1;
  } catch (...) {
    return 2;
  }
}
