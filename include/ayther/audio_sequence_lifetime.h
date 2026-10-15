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

/// The signatures an assigned sound EFFECT owns: its trigger and the head and
/// member signatures of its sequence.
struct EffectSequenceSignatures {
  std::uint64_t trigger{};
  std::span<const std::uint64_t> head;
  std::span<const std::uint64_t> members;
};

/// An original event an assigned effect explains is not a replacement of an
/// exclusive sequence: the game lent the channel to the sound effect, and the
/// music comes back when it ends. Golden Axe, 2026-10-14: the second voice of
/// the character-select effect — a head of its sequence, never an assigned
/// signature itself — landed on a channel of «The Battle» and cut its HD loop.
[[nodiscard]] inline bool explained_by_effect_sequence(
    const std::uint64_t signature,
    const std::span<const EffectSequenceSignatures> effects) noexcept {
  for (const auto &effect : effects) {
    if (effect.trigger == signature ||
        std::find(effect.head.begin(), effect.head.end(), signature) !=
            effect.head.end() ||
        std::find(effect.members.begin(), effect.members.end(), signature) !=
            effect.members.end())
      return true;
  }
  return false;
}

} // namespace ayther
