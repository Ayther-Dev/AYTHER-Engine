#pragma once
// ---------------------------------------------------------------------------
// cram_palette.h — the Mega Drive CRAM, read (EM-9.4).
//
// The four VDP palette lines: 64 nine-bit colours that decide what colour every
// index of every tile is seen as. Everything else in the pipeline —the plane
// tile hash, the per-palette variant signature, the tint— leans on this, and
// until now the conversion lived loose in three different places inside
// `ayther_session.cpp`.
//
// # The format: PACKED, not the bus one
//
// The CRAM the fork publishes comes PACKED —R in bits 0-2, G in 3-5, B in 6-8—
// and **not** in the Genesis bus format, which leaves gaps (R=1-3, G=5-7,
// B=9-11). Confusing them yields colours that look plausible: everything comes
// out at half intensity and hue-shifted, which is worse than coming out plainly
// wrong — nobody looks at it twice.
//
// Verified against the game: white = 0x1FF, blue = 0x1E3 → R3 G4 B7.
//
// # From 3 bits to 8: it is NOT `x << 5`
//
// A 3-bit component taken to 8 with a shift never reaches 255: maximum white
// would give 224 and the whole image would look washed out. The correct
// expansion repeats the bit pattern, which is what makes 7 → 255 and 0 → 0 with
// the intermediate values spread evenly.
// ---------------------------------------------------------------------------
#include <cstdint>

namespace ayther {

/// A 3-bit component to 8, repeating the pattern: `x*255/7`, which for these
/// eight values is exact and needs no division at run time.
inline constexpr uint8_t cram_c8(uint8_t x3) {
    const uint8_t v = static_cast<uint8_t>(x3 & 7);
    return static_cast<uint8_t>((v << 5) | (v << 2) | (v >> 1));
}

/// One CRAM colour, in RGB with 8 bits per channel.
struct CramColor { uint8_t r, g, b; };

/// Colour `index` (0-63) of the packed CRAM. Out of range or with no data it
/// returns black — a background colour is a legitimate result for "I do not
/// know", and returning magenta would turn every out-of-range read into a
/// visual false positive.
inline CramColor cram_color(const uint8_t* cram, size_t size, uint32_t index) {
    const size_t e = static_cast<size_t>(index) * 2;
    if (!cram || index >= 64 || e + 1 >= size) return { 0, 0, 0 };
    const uint16_t v = static_cast<uint16_t>(cram[e] | (cram[e + 1] << 8));
    return { cram_c8(static_cast<uint8_t>(v & 7)),
             cram_c8(static_cast<uint8_t>((v >> 3) & 7)),
             cram_c8(static_cast<uint8_t>((v >> 6) & 7)) };
}

/// Colour `entry` (0-15) of line `line` (0-3). It is the way a palette is
/// thought about —"index 3 of line 1"— and it saves every consumer from doing
/// the multiplication itself.
inline CramColor cram_color_at(const uint8_t* cram, size_t size,
                               uint8_t line, uint8_t entry) {
    return cram_color(cram, size, (line & 3u) * 16u + (entry & 15u));
}

/// The signature of a palette LINE: FNV-1a of its 16 words exactly as they sit
/// in CRAM ("variant by palette content").
///
/// It serves what a viewer needs and a visual comparison cannot provide: saying
/// whether two moments of the game have the SAME palette. A day/night cycle
/// changes this signature even when the on-screen change is a single shade.
inline uint64_t cram_line_signature(const uint8_t* cram, size_t size, uint8_t line) {
    uint64_t h = 0x1465'0FB0'739D'0383ull;   // the AYTHER seed (pack-identities §0)
    for (uint32_t e = 0; e < 16; ++e) {
        const size_t off = (static_cast<size_t>(line & 3u) * 16u + e) * 2u;
        const uint8_t lo = (cram && off     < size) ? cram[off]     : 0;
        const uint8_t hi = (cram && off + 1 < size) ? cram[off + 1] : 0;
        h = (h ^ lo) * 0x1000'0001'B3ull;
        h = (h ^ hi) * 0x1000'0001'B3ull;
    }
    return h;
}

}  // namespace ayther
