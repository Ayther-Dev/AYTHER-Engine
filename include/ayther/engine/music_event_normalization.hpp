#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ayther::engine {

struct EventId {
  std::uint64_t value{};
  friend constexpr bool operator==(EventId, EventId) noexcept = default;
};

struct EventProvenance {
  std::string source;
  std::uint32_t channel{};
  std::uint64_t source_ordinal{};
  std::uint64_t causal_order{};

  EventProvenance() = default;
  EventProvenance(std::string source_id, std::uint32_t source_channel,
                  std::uint64_t ordinal)
      : source(std::move(source_id)), channel(source_channel),
        source_ordinal(ordinal), causal_order(ordinal) {}
  friend bool operator==(const EventProvenance &,
                         const EventProvenance &) = default;
};

struct MusicEvent {
  EventId id{};
  std::uint64_t begin{};
  std::optional<std::uint64_t> duration;
  std::optional<std::uint64_t> signature;
  std::optional<std::uint32_t> timbre;
  std::optional<std::uint32_t> pitch;
  EventProvenance provenance;
  friend bool operator==(const MusicEvent &, const MusicEvent &) = default;
};

struct EventRange {
  std::uint64_t begin{};
  std::uint64_t end{};
};

struct NormalizedMusicEvent {
  MusicEvent event;
  bool clipped_begin{};
  bool clipped_end{};
  bool simultaneous{};
  friend bool operator==(const NormalizedMusicEvent &,
                         const NormalizedMusicEvent &) = default;
};

struct MusicalIdentityKey {
  std::uint64_t signature{};
  std::uint32_t timbre{};
  std::uint32_t pitch{};
  friend constexpr bool operator==(MusicalIdentityKey,
                                   MusicalIdentityKey) noexcept = default;
};

[[nodiscard]] inline std::optional<MusicalIdentityKey>
musical_identity_key(const MusicEvent &event) noexcept {
  if (!event.signature || !event.timbre || !event.pitch)
    return std::nullopt;
  return MusicalIdentityKey{*event.signature, *event.timbre, *event.pitch};
}

[[nodiscard]] inline bool events_equivalent(const MusicEvent &left,
                                            const MusicEvent &right) noexcept {
  const auto left_key = musical_identity_key(left);
  const auto right_key = musical_identity_key(right);
  return left_key && right_key && left_key == right_key;
}

[[nodiscard]] inline std::vector<NormalizedMusicEvent>
normalize_music_events(const std::vector<MusicEvent> &events,
                       EventRange range) {
  std::vector<NormalizedMusicEvent> result;
  if (range.begin >= range.end)
    return result;
  result.reserve(events.size());
  for (const auto &event : events) {
    const auto event_end = event.duration
                               ? std::optional{event.begin + *event.duration}
                               : std::nullopt;
    const bool intersects =
        event_end ? event.begin < range.end && *event_end > range.begin
                  : event.begin >= range.begin && event.begin < range.end;
    if (!intersects)
      continue;
    result.push_back({event, event.begin < range.begin,
                      event_end && *event_end > range.end, false});
  }
  std::ranges::sort(result, {}, [](const NormalizedMusicEvent &value) {
    const auto &event = value.event;
    return std::tuple{event.begin,
                      event.provenance.causal_order,
                      event.signature.value_or(0),
                      event.timbre.value_or(0),
                      event.pitch.value_or(0),
                      event.provenance.source,
                      event.provenance.source_ordinal,
                      event.id.value};
  });
  for (std::size_t index = 0; index < result.size(); ++index) {
    const bool same_previous =
        index > 0 && result[index - 1].event.begin == result[index].event.begin;
    const bool same_next =
        index + 1 < result.size() &&
        result[index + 1].event.begin == result[index].event.begin;
    result[index].simultaneous = same_previous || same_next;
  }
  return result;
}

} // namespace ayther::engine
