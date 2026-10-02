#include <ayther/engine/music_event_normalization.hpp>

#include <algorithm>
#include <cstdio>

namespace {
void check(bool value, const char *message, int &failures) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}
} // namespace

int main() {
  using namespace ayther::engine;
  int failures = 0;

  const EventProvenance first_source{"take-a", 7, 4};
  const EventProvenance second_source{"take-a", 8, 9};
  const MusicEvent crossing{EventId{1}, 90, 20, 0xAA, 3, 60, first_source};
  const MusicEvent simultaneous{EventId{2}, 100, 5, 0xBB, 4, 64, second_source};
  const MusicEvent unknown{EventId{3},
                           105,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           EventProvenance{"take-a", 9, 1}};

  const auto normalized = normalize_music_events(
      {unknown, simultaneous, crossing}, EventRange{100, 110});
  check(normalized.size() == 3, "crossing and open events are retained",
        failures);
  check(normalized[0].event.id == EventId{1} && normalized[0].clipped_begin &&
            !normalized[0].clipped_end,
        "an event crossing the range keeps its provenance and open boundary",
        failures);
  check(normalized[0].event.provenance == first_source,
        "source channel and ordinal remain unchanged", failures);
  check(normalized[1].event.id == EventId{2} &&
            normalized[1].event.provenance == second_source,
        "known timbre pitch duration signature and channel survive", failures);
  check(!normalized[2].event.signature && !normalized[2].event.timbre &&
            !normalized[2].event.pitch,
        "unknown values remain unknown", failures);
  check(!events_equivalent(normalized[2].event, normalized[2].event),
        "unknown is not a wildcard, including against itself", failures);

  MusicEvent rotated = simultaneous;
  rotated.provenance.channel = 11;
  check(musical_identity_key(rotated) == musical_identity_key(simultaneous),
        "rotating the source channel does not change musical identity",
        failures);

  const MusicEvent same_time_a{
      EventId{4}, 108, 1, 10, 1, 40, EventProvenance{"take-a", 12, 2}};
  const MusicEvent same_time_b{
      EventId{5}, 108, 1, 11, 1, 41, EventProvenance{"take-a", 13, 1}};
  const auto ordered_a =
      normalize_music_events({same_time_a, same_time_b}, EventRange{100, 110});
  const auto ordered_b =
      normalize_music_events({same_time_b, same_time_a}, EventRange{100, 110});
  check(ordered_a == ordered_b && ordered_a[0].simultaneous &&
            ordered_a[1].simultaneous,
        "incidental input order neither changes causality nor simultaneity",
        failures);

  return failures == 0 ? 0 : 1;
}
