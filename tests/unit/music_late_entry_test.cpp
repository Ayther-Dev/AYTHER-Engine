#include <ayther/engine/music_late_entry.hpp>

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
  const LateEntryRequest request{100,
                                 1'000'000'000ULL,
                                 2'000'000'000ULL,
                                 0,
                                 48'000,
                                 44'100,
                                 true,
                                 true,
                                 true,
                                 true};
  const auto direct = calculate_late_entry(request);
  check(direct.status == LateEntryStatus::ready &&
            direct.source_offset == 48'100 &&
            direct.delivery_crossfade_frames == 221 && direct.suppress_original,
        "late entry uses rounded source time and a five-ms delivery", failures);

  auto paused = request;
  paused.verified_pause_ns = 250'000'000ULL;
  const auto excluded = calculate_late_entry(paused);
  check(excluded.source_offset == 36'100,
        "verified pause time is excluded from delta", failures);
  paused.pause_is_verified = false;
  check(calculate_late_entry(paused).source_offset == 48'100,
        "unverified pause is not subtracted", failures);

  auto invalid = request;
  invalid.appearance_valid = false;
  check(calculate_late_entry(invalid).status == LateEntryStatus::waiting &&
            !calculate_late_entry(invalid).suppress_original,
        "invalid appearance waits without muting original", failures);
  invalid.appearance_valid = true;
  invalid.region_valid = false;
  check(calculate_late_entry(invalid).status == LateEntryStatus::waiting,
        "invalid source region waits", failures);

  auto mixed = request;
  mixed.membership_preserves_unrelated_effects = false;
  const auto unresolved = calculate_late_entry(mixed);
  check(unresolved.status == LateEntryStatus::membership_unconfirmed &&
            unresolved.diagnostic == "membership_unconfirmed" &&
            !unresolved.suppress_original && unresolved.keep_hd_mix,
        "unconfirmed membership preserves original and HD mix", failures);
  return failures == 0 ? 0 : 1;
}
