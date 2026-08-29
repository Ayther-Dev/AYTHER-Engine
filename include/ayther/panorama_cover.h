#pragma once
// ---------------------------------------------------------------------------
// panorama_cover.h — the COVERAGE rule of a Panorama.
//
// "Is what is seen at this position the strip, or is it something else drawn on
// top of the same plane?" Once the camera has anchored, that has to be answered
// cell by cell, and `FrameView.panorama_cover` comes out of that count.
//
// IT LIVES IN A HEADER AND NOT INSIDE THE SESSION because it is a rule of the
// strip FORMAT —like `ayther_plane_tile_hash_variants`, which it leans on— and
// because the defect it fixes could not be tested without a ROM and a
// twenty-minute capture. Here it is tested with three made-up hashes.
//
// THE DEFECT (measured on Sonic 3 & Knuckles f2092). One position of the strip
// can have SEVERAL hashes: an animated cell has one per state, and a scroll
// that crossed into another zone stacks two sections of the level at the same
// position. The index keeps them all —every state has to be able to ANCHOR—
// but the PNG keeps ONE (`Cell::last` in the stitcher).
//
// Accepting any of them to verify coverage declares "anchored, 100 % coverage"
// over a strip that shows a different section of the level: the exported crop
// was Angel Island —sky, water, grass— while the frame was a cave.
//
// WHY IT IS ALMOST NEVER SEEN: the native area corrects itself, because the
// live cells the strip did not claim are drawn on top and hide the weak anchor.
// What exposes it is widescreen (EM-8.1), where the extended area has nothing
// to correct itself with — there you see exactly what the strip holds.
//
// WHAT THE FIX IS NOT: a coverage floor. Both available numbers were tried and
// neither separates the cases (Golden Axe extends WELL at 69 %; Sonic 3 & K
// extends BADLY at 100 %). A threshold tuned against two data points is a
// fragile patch dressed up as a fix.
//
// THE FIX is to align the index with the drawing: coverage is verified against
// the hash the strip KEEPS and not against any of the ones that passed through
// there. The others are not discarded — they stay in the anchoring index, where
// multiplicity helps vote on where the camera is and one extra vote is offset
// by the other thirty. What they may not do is decide WHAT IS DRAWN where
// nobody is going to correct it.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <vector>

namespace ayther {

/// The 4 readings of `h` under palette line `pal`, with `out[0]` = `h` as-is.
/// The real implementation lives in the core (exact arithmetic: the PRIME is
/// invertible mod 2⁶⁴); this is only the function type, so it can be injected
/// into the test without dragging in the core.
using PanoHashVariantsFn = void (*)(uint64_t h, uint8_t pal, uint64_t out[4]);

/// Is the cell the STRIP DRAWS at this position the observed one?
///
/// `strip` holds the hashes of the strip at that position, **with the drawn one
/// first** — the order is guaranteed by `AytherSession::bg_cells`, and for a
/// baked pack, by the TOML that came out of it.
///
/// An empty strip at that position does **not** match: there is nothing to
/// compare against, and "I do not know" is not "yes". That is the difference
/// between not covering a cell and asserting that the strip explains it.
inline bool panorama_pos_matches(const std::vector<uint64_t>& strip,
                                 uint64_t h, uint8_t pal,
                                 PanoHashVariantsFn variants) {
    if (strip.empty()) return false;
    // The DIRECT path first: with no repaletting —the normal case— this costs
    // nothing, and the extra work is paid only by cells that were going to be
    // discarded anyway.
    const uint64_t rendered_hash = strip[0];
    if (rendered_hash == h) return true;
    // The same drawing under another CRAM line produces a different hash (the
    // line enters at the end of the FNV). The four readings are exact, not
    // approximate.
    uint64_t var[4];
    variants(h, pal, var);
    for (int i = 1; i < 4; ++i)
        if (rendered_hash == var[i]) return true;
    return false;
}

}  // namespace ayther
