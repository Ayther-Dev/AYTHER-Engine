// Spec 002, R1/R5: scene_inventory must associate every occurrence with the
// exact parsed-SAT record that produced it. Parsed records are canonicalized
// with the same rules as Rust's process_parsed_sprites before the ordered join.
#include "session/parsed_sprite_join.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <vector>

namespace {
int failures = 0;

void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

void append_record(std::vector<std::uint8_t> &bytes, std::uint16_t y_raw,
                   std::uint16_t x_raw, std::uint16_t attributes,
                   std::uint8_t width, std::uint8_t height, std::uint8_t slot,
                   std::uint8_t chain) {
  bytes.insert(bytes.end(), {static_cast<std::uint8_t>(y_raw),
                             static_cast<std::uint8_t>(y_raw >> 8U),
                             static_cast<std::uint8_t>(x_raw),
                             static_cast<std::uint8_t>(x_raw >> 8U),
                             static_cast<std::uint8_t>(attributes),
                             static_cast<std::uint8_t>(attributes >> 8U), width,
                             height, slot, chain});
}

AytherSpriteOccurrence occurrence(std::uint64_t hash, std::int16_t x,
                                  std::int16_t y, std::uint8_t width,
                                  std::uint8_t height, std::uint8_t slot,
                                  std::uint8_t chain, std::uint8_t palette,
                                  std::uint8_t priority, bool hflip,
                                  bool vflip) {
  AytherSpriteOccurrence value{};
  value.hash = hash;
  value.screen_x = x;
  value.screen_y = y;
  value.w_tiles = width;
  value.h_tiles = height;
  value.slot = slot;
  value.link = chain;
  value.palette = palette;
  value.priority = priority;
  value.hflip = hflip ? 1 : 0;
  value.vflip = vflip ? 1 : 0;
  return value;
}
} // namespace

int main() try {
  using ayther::session::canonical_parsed_sprites;
  using ayther::session::join_parsed_sprites;
  using ayther::session::parsed_sprite_chain_ranks;

  constexpr std::uint16_t kFlags =
      0x0800U | (1U << 13U) | (1U << 15U); // H flip, palette 1, priority 1.
  constexpr std::uint16_t kAttrA = kFlags | 0x0123U;
  constexpr std::uint16_t kAttrB = kFlags | 0x0456U;
  constexpr std::uint16_t kAttrC = kFlags | 0x0789U;

  {
    ayther_sprite_v1 native{};
    native.yr = 0x1234;
    native.xr = 0x5678;
    native.attr = 0x9ABC;
    native.w = 3;
    native.h = 2;
    native.sat_idx = 0x4D;
    native.chain_pos = 0x5E;
    const std::array<std::uint8_t, 10> expected{0x34, 0x12, 0x78, 0x56, 0xBC,
                                                0x9A, 3,    2,    0x4D, 0x5E};
    check(ayther::session::parsed_sprite_le_bytes(native) == expected,
          "the native-endian ABI struct is normalized explicitly to the "
          "little-endian byte contract consumed by Rust and the join");
  }

  std::vector<std::uint8_t> bytes;
  append_record(bytes, 128, 128, 0, 1, 1, 1, 0); // Tile zero: no occurrence.
  append_record(bytes, 160, 200, kAttrA, 2, 3, 7, 9);
  append_record(bytes, 160, 200, kAttrA, 2, 3, 7, 1); // Exact duplicate.
  append_record(bytes, 160, 200, kAttrB, 2, 3, 7, 5);
  append_record(bytes, 160, 200, kAttrB, 2, 3, 7, 5); // Stable duplicate.
  // Same observable metadata as B, but another pattern. Only source order can
  // distinguish these two records; the old slot+x+y lookup chose A for both.
  append_record(bytes, 160, 200, kAttrC, 2, 3, 7, 5);
  append_record(bytes, 128, 464, 0x0022, 1, 1, 8, 6); // Fully off screen.
  append_record(bytes, 152, 144, 0x1077, 0, 0, 9, 6); // 0 dimensions => 1x1.
  append_record(bytes, static_cast<std::uint16_t>(128U | 0x0200U),
                static_cast<std::uint16_t>(128U | 0x0200U), 0x0234, 1, 1, 10,
                7); // Coordinates are nine-bit values.
  bytes.insert(bytes.end(), {0xAA, 0xBB, 0xCC}); // Incomplete record.

  const auto records = canonical_parsed_sprites(bytes);
  check(records.size() == 7,
        "the first nine bytes are deduplicated without dropping tile-zero or "
        "off-screen records, and a truncated tail is ignored safely");
  check(records.size() > 1 && records[1].chain == 1 &&
            records[1].chain_ambiguous,
        "RF-10.3/O1: a duplicate retains the minimum chain but records that "
        "its raster-time rank changed");
  const auto ambiguous_identity =
      ayther::session::parsed_sprite_scene_identity(&records[1]);
  check(ambiguous_identity.chain == 1 &&
            !ambiguous_identity.raster_identity_known,
        "RF-10.3/O1: chain ambiguity preserves render/observation depth "
        "while withholding only final-SAT raster authorization");
  check(records.size() > 2 && !records[2].chain_ambiguous,
        "a duplicate at the same chain rank remains a known identity");

  const std::array occurrences{
      occurrence(0xA, 72, 32, 2, 3, 7, 1, 1, 1, true, false),
      occurrence(0xB, 72, 32, 2, 3, 7, 5, 1, 1, true, false),
      occurrence(0xC, 72, 32, 2, 3, 7, 5, 1, 1, true, false),
      occurrence(0xD, 16, 24, 1, 1, 9, 6, 0, 0, false, true),
      occurrence(0xE, 0, 0, 1, 1, 10, 7, 0, 0, false, false),
  };
  const auto joined = join_parsed_sprites(records, occurrences);
  check(joined.size() == occurrences.size(),
        "the join has one result per occurrence");
  check(joined[0] && records[*joined[0]].pattern() == 0x0123,
        "the first occurrence maps to the first valid source record");
  check(joined[1] && records[*joined[1]].pattern() == 0x0456,
        "same slot and position maps by parsed order, not to the first slot "
        "candidate");
  check(joined[2] && records[*joined[2]].pattern() == 0x0789,
        "records identical outside their pattern remain distinct in source "
        "order");
  check(joined[3] && records[*joined[3]].width_tiles() == 1 &&
            records[*joined[3]].height_tiles() == 1,
        "zero raw dimensions normalize to one tile like the Rust parser");
  check(joined[4] && records[*joined[4]].screen_x() == 0 &&
            records[*joined[4]].screen_y() == 0,
        "raw X and Y are masked to nine bits before the coordinate offset");
  const auto ranks = parsed_sprite_chain_ranks(records, occurrences);
  check(ranks.size() == occurrences.size() && ranks[0] == 1 && ranks[1] == 5 &&
            ranks[2] == 5 && ranks[3] == 6 && ranks[4] == 7,
        "RF-8.1/RF-8.3/RF-8.4: canonical 9-to-1 duplicates and distinct "
        "same-slot occurrences retain their exact per-occurrence depth");

  auto mismatched = occurrences;
  mismatched[1].palette = 2;
  mismatched[2].priority = 0;
  mismatched[3].vflip = 0;
  const auto rejected = join_parsed_sprites(records, mismatched);
  check(!rejected[1] && !rejected[2] && !rejected[3],
        "palette, priority and flips are validated instead of accepting a "
        "positional fallback");

  auto wrong_geometry = occurrences;
  wrong_geometry[0].w_tiles = 3;
  wrong_geometry[1].link = 6;
  const auto geometry_rejected = join_parsed_sprites(records, wrong_geometry);
  check(!geometry_rejected[0] && !geometry_rejected[1],
        "dimensions and chain position are part of the exact join contract");

  std::vector<AytherSpriteOccurrence> excess(occurrences.begin(),
                                             occurrences.end());
  excess.push_back(occurrence(0xF, 8, 8, 1, 1, 11, 8, 0, 0, false, false));
  const auto short_join = join_parsed_sprites(records, excess);
  check(short_join.size() == excess.size() && !short_join.back(),
        "an occurrence without a source candidate remains explicitly unjoined");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
