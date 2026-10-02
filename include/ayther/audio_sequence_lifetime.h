#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

namespace ayther {

struct ActiveAudioSignature {
  std::uint64_t signature{};
  std::uint32_t channel_bit{};
  bool replacement_candidate{true};
};

enum class SequencePresence {
  no_relevant_activity,
  member_active,
  replacement_active,
};

[[nodiscard]] inline SequencePresence
sequence_presence(const std::uint64_t sequence_signature,
                  const std::span<const std::uint64_t> members,
                  const std::uint32_t owned_channels,
                  const std::span<const ActiveAudioSignature> active) noexcept {
  bool replacement = false;
  for (const auto &event : active) {
    const bool member = event.signature == sequence_signature ||
                        std::find(members.begin(), members.end(),
                                  event.signature) != members.end();
    if (member)
      return SequencePresence::member_active;
    replacement = replacement || (event.replacement_candidate &&
                                  (event.channel_bit & owned_channels) != 0U);
  }
  return replacement ? SequencePresence::replacement_active
                     : SequencePresence::no_relevant_activity;
}

} // namespace ayther
