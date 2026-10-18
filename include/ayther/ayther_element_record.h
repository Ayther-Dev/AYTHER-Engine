#pragma once
// ---------------------------------------------------------------------------
// ayther_element_record.h — the raw record of one element of a game, and its
// reconstruction without an emulator (spec 007, Índice de elementos).
//
// An ELEMENT is one distinct observation of the VDP, keyed by (kind, hash): a
// sprite (the xxh3 of its w×h tiles, blind to palette and flips), a plane tile
// (the FNV of its 8×8 pattern mixed with its palette line; planes A and B) or a
// window tile (the same hash, on the Window plane). A frontend that keeps these
// records — the Lab's project index — can show, compose and measure the
// elements of a game with no take loaded, and after the take they came from is
// gone.
//
// Ownership: the Engine owns the FORMAT (the record stream below), the HASH
// reproducers and the RECONSTRUCTION (the CPU decoder here; the Vulkan path in
// AytherRenderer::render_elements). A frontend owns the policy: which frames
// feed the index, when it is written, what it shows.
//
// # Byte order
//
// The pattern bytes are stored in BUS order: the byte the VDP reads at tile
// offset `o`. The emulator publishes VRAM word-swapped on little-endian hosts
// (`docs/EMULATOR_EXTENSION_ABI.md`, `AYTHER_REGION_WORD_SWAPPED_LE`), so the
// bus byte `o` lives at host offset `o ^ 1`. The plane-tile hash and every
// decoder read the bus view; the sprite hash reads the HOST array
// (`docs/IDENTITY_SPECIFICATION.md`), so `sprite_pattern_hash` swaps the pairs
// back before hashing. One order on disk, both hashes reproducible.
//
// # Versions
//
// `kElementRecordSchema` versions this stream. The two identity algorithms are
// persisted too (`AytherRecording::kSpriteHashAlgo`, `kPlaneTileHashAlgo`): a
// reader that finds other values keeps the records and reports them stale.
// ---------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ayther {

/// Version of the record stream written by write_element_records().
inline constexpr uint32_t kElementRecordSchema = 1;

/// Version of the plane-tile hash: FNV-1a-64 (AYTHER seed) over the 32 bus
/// bytes of the pattern, then the palette line (0-3). Flips and plane are not
/// part of it. It pins what `plane_tile_hash` computes, the way
/// `AytherRecording::kSpriteHashAlgo` pins the sprite hash.
inline constexpr uint32_t kPlaneTileHashAlgo = 1;

/// The only byte order this revision writes: bus order (see the header note).
inline constexpr uint8_t kElementByteOrderBus = 1;

/// Kind of element. Values 16-31 are reserved for the audio records of a later
/// revision; a reader skips any kind it does not know.
enum class ElementKind : uint8_t {
  Sprite = 1,
  PlaneTile = 2,  ///< planes A and B (see ElementRecord::planes_seen)
  WindowTile = 3, ///< the Window plane
};

/// Bits of ElementRecord::flips_seen.
inline constexpr uint8_t kElementFlipsNone = 0x01;
inline constexpr uint8_t kElementFlipsH = 0x02;
inline constexpr uint8_t kElementFlipsV = 0x04;
inline constexpr uint8_t kElementFlipsHV = 0x08;
/// Bits of ElementRecord::priority_seen.
inline constexpr uint8_t kElementPriorityLow = 0x01;
inline constexpr uint8_t kElementPriorityHigh = 0x02;
/// Bits of ElementRecord::planes_seen.
inline constexpr uint8_t kElementPlaneB = 0x01;
inline constexpr uint8_t kElementPlaneA = 0x02;
inline constexpr uint8_t kElementPlaneW = 0x04;

/// One palette the element was seen with: the 16 packed CRAM words
/// (R0-2 G3-5 B6-8) of `line` at that moment. A sprite hash ignores the
/// palette, so a sprite usually carries several; a tile hash includes the LINE
/// index, so its variants differ in CONTENT (day/night).
struct ElementPaletteVariant {
  uint8_t line = 0;
  uint16_t words[16] = {};
  uint64_t line_sig = 0; ///< cram_line_signature of the line when captured
  uint32_t count = 0;    ///< frames seen with this palette
};

/// A run of frames of one take in which the element was observed.
struct ElementProvenance {
  uint64_t take_id = 0; ///< the take's embedded id (never 0 in a record)
  uint32_t first_frame = 0;
  uint32_t last_frame = 0; ///< inclusive
  uint32_t count = 0;      ///< frames in the run with the element on screen
};

/// The raw record of one element (see the header note).
struct ElementRecord {
  ElementKind kind = ElementKind::Sprite;
  uint64_t hash = 0;
  uint8_t w_tiles = 1; ///< tiles: always 1
  uint8_t h_tiles = 1;
  /// Tiles: the palette line mixed into the hash. Sprites: 0 (not hashed).
  uint8_t palette_line = 0;
  uint8_t planes_seen = 0;   ///< tiles: kElementPlane* bits
  uint8_t flips_seen = 0;    ///< kElementFlips* bits
  uint8_t priority_seen = 0; ///< kElementPriority* bits
  /// Pattern bytes in BUS order: `w_tiles * h_tiles * 32`, tiles contiguous
  /// from the base tile in VRAM order (column-major for a sprite: tile t is
  /// column t / h_tiles, row t % h_tiles).
  std::vector<uint8_t> pattern;
  std::vector<ElementPaletteVariant> palettes;
  std::vector<ElementProvenance> provenance;
  uint64_t first_seen_unix = 0;
  uint64_t last_seen_unix = 0;

  [[nodiscard]] size_t expected_pattern_bytes() const noexcept {
    return static_cast<size_t>(w_tiles) * h_tiles * 32u;
  }
};

/// The stream header plus its records.
struct ElementRecordSet {
  uint32_t schema = kElementRecordSchema;
  uint32_t sprite_hash_algo = 0; ///< AytherRecording::kSpriteHashAlgo at write
  uint32_t plane_tile_hash_algo = kPlaneTileHashAlgo;
  uint8_t byte_order = kElementByteOrderBus;
  uint64_t generation = 0; ///< the writer's monotonic counter
  uint64_t created_unix = 0;
  std::vector<ElementRecord> records;
};

/// Limits of the stream (spec 007, L-1..L-5). The writer refuses a set past
/// them; the reader refuses a body past `max_body_bytes`.
struct ElementRecordLimits {
  uint32_t max_records = 262144;
  uint64_t max_stream_bytes = 256ull << 20;
  uint8_t max_palettes = 16;
  uint8_t max_provenance = 64;
  uint32_t max_body_bytes = 65536;
};

/// What read_element_records() found besides the records.
struct ElementRecordReadReport {
  uint32_t skipped_unknown_kind = 0;
  bool crc_ok = false;
  std::string error; ///< empty on success
};

/// Serializes `set` (contract C1 of spec 007). false with `error` set when a
/// record is malformed (pattern size, tile counts) or a limit is exceeded;
/// nothing is written then.
bool write_element_records(const ElementRecordSet &set,
                           std::vector<uint8_t> &out,
                           const ElementRecordLimits &limits = {},
                           std::string *error = nullptr);

/// Parses a stream written by write_element_records(). Unknown kinds are
/// skipped and counted. false on bad magic, newer schema, unknown byte order,
/// truncation, a body over the limit, or a CRC mismatch; `out` is then left
/// with whatever parsed, and the report says why.
bool read_element_records(std::span<const uint8_t> bytes, ElementRecordSet &out,
                          ElementRecordReadReport *report = nullptr,
                          const ElementRecordLimits &limits = {});

// -- Identities -------------------------------------------------------------

/// The plane-tile hash of 32 pattern bytes in BUS order under palette line
/// `pal` — the same value AytherSession computes for a PlaneTileOccurrence
/// (algorithm kPlaneTileHashAlgo).
uint64_t plane_tile_hash(const uint8_t *bus_bytes32, uint8_t pal) noexcept;

/// The sprite hash of `n` pattern bytes in BUS order (`n` a multiple of 32:
/// the w×h tiles in VRAM order) — the same value the sprite detector computes
/// (AytherRecording::kSpriteHashAlgo). Swaps the pairs back to host order and
/// hashes through the core (`ayther_sprite_pattern_hash`). 0 when `n` is 0 or
/// not a multiple of 32.
uint64_t sprite_pattern_hash(const uint8_t *bus_bytes, size_t n) noexcept;

// -- Reconstruction (CPU) ---------------------------------------------------

/// One packed CRAM word (R0-2 G3-5 B6-8) as the game shows it: the core's
/// framebuffer level (a 3-bit 7 is 239, not 255), byte-identical to the
/// Vulkan scene path, so CPU and GPU reconstructions agree (spec 007, RNF-3).
void element_color_rgb(uint16_t packed, uint8_t *rgb3) noexcept;

/// Decodes one 8×8 tile (32 bus bytes, 4 bpp, high nibble first) with the 16
/// packed CRAM words of a line into 8×8 RGBA8 (row-major, 256 bytes). Index 0
/// is transparent (alpha 0) when `index0_transparent`, else the line's colour
/// 0. Pixel layout as the VDP; colours as element_color_rgb (the framebuffer
/// level — AytherSession::decode_plane_tile_rgba expands to the full 0-255).
void decode_tile_rgba(const uint8_t *bus_bytes32, const uint16_t *cram_line16,
                      bool hflip, bool vflip, bool index0_transparent,
                      uint8_t *out_rgba256) noexcept;

/// Which tile of a sprite's pattern (VRAM order) lands on screen tile
/// (`cx`, `cy`) under the given flips: the VDP mirrors the tile grid as well as
/// each tile's pixels.
constexpr uint32_t sprite_tile_index(uint8_t w_tiles, uint8_t h_tiles,
                                     uint32_t cx, uint32_t cy, bool hflip,
                                     bool vflip) noexcept {
  const uint32_t sx = hflip ? (w_tiles - 1u - cx) : cx;
  const uint32_t sy = vflip ? (h_tiles - 1u - cy) : cy;
  return sx * h_tiles + sy;
}

/// An RGBA8 image, row-major, `w * h * 4` bytes.
struct ElementImage {
  uint32_t w = 0;
  uint32_t h = 0;
  std::vector<uint8_t> rgba;
};

/// How to draw a record.
struct ElementRenderOptions {
  uint8_t palette_variant = 0; ///< index into ElementRecord::palettes
  uint8_t flips = 0;           ///< bit0 hflip, bit1 vflip
  bool index0_transparent = true;
  /// Overrides the variant: 16 packed CRAM words. nullptr = the variant; with
  /// no variants either, a grey ramp (index / 2 per channel, as a CRAM word)
  /// so the shape is still seen.
  const uint16_t *cram_line = nullptr;
};

/// Renders one record at native size (w_tiles*8 × h_tiles*8). false when the
/// record's pattern size does not match its tiles.
bool render_element_rgba(const ElementRecord &record,
                         const ElementRenderOptions &opts, ElementImage &out);

/// One record placed on a canvas (a pose member, a set cell).
struct ElementPlacement {
  const ElementRecord *record = nullptr;
  int32_t x = 0; ///< canvas px, may be negative (clipped)
  int32_t y = 0;
  uint8_t palette_variant = 0;
  uint8_t flips = 0;
  const uint16_t *cram_line = nullptr; ///< per-placement override
};

/// Composes placements in order (later on top) on a `w × h` canvas filled with
/// `backdrop_packed` (a packed CRAM colour) or left transparent. Index 0 of
/// every element is transparent. false when any record is malformed. The
/// Vulkan path (build_element_scene) draws tiles beneath sprites; list tiles
/// first for the two to agree.
bool compose_elements_rgba(std::span<const ElementPlacement> placements,
                           uint32_t w, uint32_t h, uint16_t backdrop_packed,
                           bool transparent_backdrop, ElementImage &out);

} // namespace ayther
