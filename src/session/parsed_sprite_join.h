// Parsed-SAT canonicalization and occurrence join used by scene_inventory.
#pragma once

#include <ayther/ayther_core_ffi.h>
#include <ayther/libretro_host/ayther_api.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ayther::session {

inline constexpr std::size_t kParsedSpriteRecordSize =
    AYTHER_PARSED_SPRITE_RECORD_SIZE;

/// Converts the ABI's native-endian in-process struct to the canonical
/// little-endian byte record consumed by the Rust hasher and this file's
/// parser. Keeping the conversion field-wise avoids depending on host byte
/// order, struct aliasing, or padding.
[[nodiscard]] constexpr std::array<std::uint8_t, kParsedSpriteRecordSize>
parsed_sprite_le_bytes(const ayther_sprite_v1 &sprite) noexcept {
  static_assert(sizeof(ayther_sprite_v1) == kParsedSpriteRecordSize);
  return {
      static_cast<std::uint8_t>(sprite.yr),
      static_cast<std::uint8_t>(sprite.yr >> 8U),
      static_cast<std::uint8_t>(sprite.xr),
      static_cast<std::uint8_t>(sprite.xr >> 8U),
      static_cast<std::uint8_t>(sprite.attr),
      static_cast<std::uint8_t>(sprite.attr >> 8U),
      sprite.w,
      sprite.h,
      sprite.sat_idx,
      sprite.chain_pos,
  };
}

/// One canonical 10-byte record from AYTHER_MEMORY_PARSED_SPRITES.
///
/// Width and height retain their raw bytes so deduplication can use the exact
/// first nine bytes. Accessors expose the normalized values consumed by
/// Rust's SpriteHasher::process_parsed_sprites.
struct ParsedSpriteRecord {
  std::uint16_t y_raw{};
  std::uint16_t x_raw{};
  std::uint16_t attributes{};
  std::uint8_t width_raw{};
  std::uint8_t height_raw{};
  std::uint8_t slot{};
  std::uint8_t chain{};
  /// The same first-nine-byte sprite was parsed at more than one chain rank.
  /// `chain` remains the minimum to mirror Rust, but it is not a stable
  /// frame-wide identity for final-SAT authorization.
  bool chain_ambiguous = false;

  [[nodiscard]] constexpr std::uint8_t width_tiles() const noexcept {
    return (std::max)(width_raw, std::uint8_t{1});
  }

  [[nodiscard]] constexpr std::uint8_t height_tiles() const noexcept {
    return (std::max)(height_raw, std::uint8_t{1});
  }

  [[nodiscard]] constexpr std::int16_t screen_x() const noexcept {
    return static_cast<std::int16_t>(x_raw & 0x01FFU) - 128;
  }

  [[nodiscard]] constexpr std::int16_t screen_y() const noexcept {
    return static_cast<std::int16_t>(y_raw & 0x01FFU) - 128;
  }

  [[nodiscard]] constexpr std::uint16_t pattern() const noexcept {
    return attributes & 0x07FFU;
  }

  [[nodiscard]] constexpr std::uint8_t palette() const noexcept {
    return static_cast<std::uint8_t>((attributes >> 13U) & 0x03U);
  }

  [[nodiscard]] constexpr std::uint8_t priority() const noexcept {
    return static_cast<std::uint8_t>((attributes >> 15U) & 0x01U);
  }

  [[nodiscard]] constexpr bool hflip() const noexcept {
    return (attributes & 0x0800U) != 0;
  }

  [[nodiscard]] constexpr bool vflip() const noexcept {
    return (attributes & 0x1000U) != 0;
  }
};

struct ParsedSpriteSceneIdentity {
  std::uint8_t chain = 0xFF;
  bool raster_identity_known = false;
};

/// Preserves the chain rank used by normal depth and observation while
/// withholding only the final-SAT proof when that rank changed mid-frame.
[[nodiscard]] constexpr ParsedSpriteSceneIdentity
parsed_sprite_scene_identity(const ParsedSpriteRecord *record) noexcept {
  return record ? ParsedSpriteSceneIdentity{record->chain,
                                            !record->chain_ambiguous}
                : ParsedSpriteSceneIdentity{};
}

[[nodiscard]] constexpr bool
same_parsed_sprite_key(const ParsedSpriteRecord &left,
                       const ParsedSpriteRecord &right) noexcept {
  return left.y_raw == right.y_raw && left.x_raw == right.x_raw &&
         left.attributes == right.attributes &&
         left.width_raw == right.width_raw &&
         left.height_raw == right.height_raw && left.slot == right.slot;
}

/// Mirrors process_parsed_sprites' exact first-nine-byte deduplication. The
/// first record keeps its position while repeated parses lower its chain rank.
/// A trailing partial record is ignored.
[[nodiscard]] inline std::vector<ParsedSpriteRecord>
canonical_parsed_sprites(std::span<const std::uint8_t> bytes) {
  const std::size_t count = bytes.size() / kParsedSpriteRecordSize;
  std::vector<ParsedSpriteRecord> records;
  records.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const std::size_t offset = index * kParsedSpriteRecordSize;
    const auto word = [&](std::size_t at) {
      return static_cast<std::uint16_t>(
          bytes[offset + at] |
          (static_cast<std::uint16_t>(bytes[offset + at + 1]) << 8U));
    };
    ParsedSpriteRecord record{word(0),           word(2),
                              word(4),           bytes[offset + 6],
                              bytes[offset + 7], bytes[offset + 8],
                              bytes[offset + 9], false};
    const auto duplicate = std::find_if(
        records.begin(), records.end(), [&](const ParsedSpriteRecord &known) {
          return same_parsed_sprite_key(known, record);
        });
    if (duplicate != records.end()) {
      duplicate->chain_ambiguous =
          duplicate->chain_ambiguous || duplicate->chain != record.chain;
      duplicate->chain = (std::min)(duplicate->chain, record.chain);
      continue;
    }
    records.push_back(record);
  }
  return records;
}

/// Whether Rust emits an occurrence for this canonical parsed record.
[[nodiscard]] constexpr bool
parsed_sprite_produces_occurrence(const ParsedSpriteRecord &record) noexcept {
  const std::int16_t x = record.screen_x();
  const std::int16_t y = record.screen_y();
  const std::int16_t width =
      static_cast<std::int16_t>(record.width_tiles() * 8U);
  const std::int16_t height =
      static_cast<std::int16_t>(record.height_tiles() * 8U);
  const bool offscreen = x <= -width || y <= -height || x >= 336 || y >= 240;
  return record.pattern() != 0 && !offscreen;
}

/// Validates the observable metadata of one ordered parsed-source pair.
[[nodiscard]] constexpr bool parsed_sprite_matches_occurrence(
    const ParsedSpriteRecord &record,
    const AytherSpriteOccurrence &occurrence) noexcept {
  return parsed_sprite_produces_occurrence(record) &&
         record.screen_x() == occurrence.screen_x &&
         record.screen_y() == occurrence.screen_y &&
         record.width_tiles() == occurrence.w_tiles &&
         record.height_tiles() == occurrence.h_tiles &&
         record.slot == occurrence.slot && record.chain == occurrence.link &&
         record.palette() == occurrence.palette &&
         record.priority() == occurrence.priority &&
         record.hflip() == (occurrence.hflip != 0) &&
         record.vflip() == (occurrence.vflip != 0);
}

/// Joins the occurrence list to the canonical parsed records by producer
/// order, then validates every paired record. Order is necessary because two
/// mid-frame records can share slot, geometry, chain and visual flags while
/// referring to different patterns. A mismatch remains unjoined; it never
/// falls back to another record with the same slot or position.
[[nodiscard]] inline std::vector<std::optional<std::size_t>>
join_parsed_sprites(std::span<const ParsedSpriteRecord> records,
                    std::span<const AytherSpriteOccurrence> occurrences) {
  std::vector<std::optional<std::size_t>> joined(occurrences.size());
  std::size_t occurrence_index = 0;
  for (std::size_t record_index = 0;
       record_index < records.size() && occurrence_index < occurrences.size();
       ++record_index) {
    if (!parsed_sprite_produces_occurrence(records[record_index]))
      continue;
    if (parsed_sprite_matches_occurrence(records[record_index],
                                         occurrences[occurrence_index]))
      joined[occurrence_index] = record_index;
    ++occurrence_index;
  }
  return joined;
}

/// The canonical parsed-SAT chain rank for each exact occurrence. A rank is
/// occurrence-scoped rather than slot-scoped because one SAT slot can be
/// parsed with different contents more than once during the same frame.
/// Unknown or mismatched occurrences remain 0xFF instead of borrowing the
/// depth of another record that happens to reuse their slot.
[[nodiscard]] inline std::vector<std::uint8_t>
parsed_sprite_chain_ranks(std::span<const ParsedSpriteRecord> records,
                          std::span<const AytherSpriteOccurrence> occurrences) {
  const auto joined = join_parsed_sprites(records, occurrences);
  std::vector<std::uint8_t> ranks(occurrences.size(), 0xFF);
  for (std::size_t index = 0; index < joined.size(); ++index)
    if (joined[index])
      ranks[index] = records[*joined[index]].chain;
  return ranks;
}

} // namespace ayther::session
