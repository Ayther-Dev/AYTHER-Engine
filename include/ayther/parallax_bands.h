#pragma once
// ---------------------------------------------------------------------------
// parallax_bands.h — the level column PER BAND (EM-8.0).
//
// Plane B carries per-band parallax: every entry of the Hscroll table has its
// own displacement, so "level column" **is not a single thing on that plane** —
// it depends on the row. With a single camera per plane, every band collapses
// onto the same columns and they stack on top of one another.
//
// MEASURED on Sonic 2 (`background_spike`, 1200 frames): plane A reconstructed
// 607 level columns and plane B only **37** —less than one screen— with 45
// bands per frame. It was not that art was missing: it was all stacked in the
// wrong place.
//
// TWO THINGS THIS FILE LEARNED THE HARD WAY
//
// 1. The rule lives here and not inside the loop in `ayther_session.cpp`. The
//    first version was buried there, where the stitcher oracle could not see it
//    —it calls the stitcher directly— so measuring it moved not a single
//    number. It was not wrong: it was not being executed.
//
// 2. Subtracting the H of two bands within the same frame is not enough. The
//    VDP field is 10 bits and it wraps, and the separation between bands GROWS
//    without bound over the course of a level (measured: 17 px against 566 px
//    over 1033 px of scroll). Each band needs its own unwrapping, just like the
//    plane camera. That is why `BandCameras` has state: a pure subtraction
//    cannot know how many times each band wrapped around.
// ---------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ayther {

/// How the Hscroll table is organised (register $0B, bits 0-1).
///
///   0 = one for the whole plane · 1 = per cell within the first tile ·
///   2 = per cell (every 8 lines) · 3 = per line
inline uint32_t hscroll_mask(uint8_t reg0b) {
    static const uint32_t kMask[4] = { 0x00, 0x07, 0xF8, 0xFF };
    return kMask[reg0b & 3];
}

/// Base of the Hscroll table in VRAM (register $0D).
inline uint32_t hscroll_base(uint8_t reg0d) {
    return (static_cast<uint32_t>(reg0d) << 10) & 0xFC00u;
}

/// Reads the H of one screen LINE for a plane. `read_u32` yields the VRAM long
/// word already un-swapped.
///
/// The VDP field is 10 bits: the game may write above that and the rest is
/// ignored, so it is masked here and not in the caller — doing it outside is
/// how a 16-bit value sneaks into a 10-bit calculation.
template <typename ReadU32>
inline int hscroll_of_line(const ReadU32& read_u32, uint32_t base, uint32_t mask,
                           uint8_t plane, int line) {
    const uint32_t idx = static_cast<uint32_t>(line < 0 ? 0 : line) & mask;
    const uint32_t hw  = read_u32(base + (idx << 2));
    return plane == 0 ? static_cast<int>(hw & 0x3FF)
                      : static_cast<int>((hw >> 16) & 0x3FF);
}

/// The ALWAYS positive remainder. `-1 % 512` is -1 in C++, and a negative
/// column here would mean reading the nametable out of bounds.
inline int wrap_px(int v, int w) {
    if (w <= 0) return 0;
    const int r = v % w;
    return r < 0 ? r + w : r;
}

/// How many distinct BANDS this plane has, looking at the `rows` rows that are
/// drawn. It is the measurement that says whether separating is worthwhile: 1
/// means the plane has no per-band parallax.
template <typename ReadU32>
inline int band_count(const ReadU32& read_u32, uint32_t base, uint32_t mask,
                      uint8_t plane, int rows) {
    if (mask == 0 || rows <= 0) return 1;
    int distinct = 1;
    int last = hscroll_of_line(read_u32, base, mask, plane, 0);
    for (int r = 1; r < rows; ++r) {
        const int h = hscroll_of_line(read_u32, base, mask, plane, r * 8);
        if (h != last) { ++distinct; last = h; }
    }
    return distinct;
}

// ---------------------------------------------------------------------------
/// The ABSOLUTE camera of each band of a plane, unwrapped frame by frame.
///
/// The VDP only says where each band is WITHIN the plane (0..width-1). To
/// reconstruct a level strip the absolute position is needed, and that requires
/// memory: the wrap a band made between two frames is inferred from the jump
/// being small. The criterion is the usual one —a jump of more than half a
/// plane reads as a wrap— and that is why it must be fed EVERY frame: skipping
/// frames turns a fast scroll into an invented wrap.
class BandCameras {
public:
    /// `rows` = rows of cells on screen · `width_px` = plane width.
    /// Reconfiguring (another level, another plane size) resets the start: the
    /// bands are seeded again on the next frame.
    void configure(int rows, int width_px) {
        if (rows == rows_ && width_px == width_) return;
        rows_ = rows > 0 ? rows : 0;
        width_ = width_px > 0 ? width_px : 512;
        st_.assign(static_cast<size_t>(rows_), State{});
    }

    void reset() { st_.assign(st_.size(), State{}); }

    int rows() const { return rows_; }

    /// Consumes one frame: reads the Hscroll table and updates the `rows`
    /// bands.
    ///
    /// The VDP H is how far the plane moved TOWARDS the right, so the camera is
    /// its negative — the same sign the rest of the motor uses.
    template <typename ReadU32>
    void observe(const ReadU32& read_u32, uint32_t base, uint32_t mask,
                 uint8_t plane) {
        if (rows_ <= 0) return;
        // The topmost band is the origin, and the rest are SEEDED relative to
        // it. Seeding each one at its own wrapped value would place a band 64 px
        // behind at column +56 instead of at -8: the VDP says where it is within
        // the plane, not on which side.
        const int h0 = hscroll_of_line(read_u32, base, mask, plane, 0);
        const int w0 = wrap_px(-h0, width_);
        push(0, w0, w0, 0);
        const int64_t ref = st_[0].abs;
        for (int r = 1; r < rows_; ++r) {
            const int h = hscroll_of_line(read_u32, base, mask, plane, r * 8);
            push(r, wrap_px(-h, width_), w0, ref);
        }
    }

    /// The level column of the band that row `row` belongs to.
    /// Out of range it returns 0, which is the old single camera: the
    /// degradation is "as it was", never a jump.
    int column(int row) const {
        if (row < 0 || row >= rows_) return 0;
        const int64_t a = st_[static_cast<size_t>(row)].abs;
        return static_cast<int>(a >= 0 ? a / 8 : -((-a + 7) / 8));
    }

    int64_t absolute_px(int row) const {
        if (row < 0 || row >= rows_) return 0;
        return st_[static_cast<size_t>(row)].abs;
    }

    /// How far this band is from band 0, in columns. It is what gets added to
    /// a plane camera already unwrapped elsewhere.
    int offset_from_top(int row) const { return column(row) - column(0); }

private:
    struct State { bool seeded = false; int last = 0; int64_t abs = 0; };

    /// The representative closest to zero. Half a plane: below it is scroll,
    /// above it is a wrap.
    ///
    /// It is a BET, and the only one that can be made looking at a wrapped
    /// value: two bands separated by more than half a plane in the same frame
    /// are indistinguishable from two separated by whatever is left on the
    /// other side. At the start of a level the bands are all together, so the
    /// bet is made when it is safe and after that it only accumulates.
    int shortest(int d) const {
        if (d >  width_ / 2) d -= width_;
        if (d < -width_ / 2) d += width_;
        return d;
    }

    void push(int row, int wrapped, int ref_wrapped, int64_t ref_abs) {
        State& s = st_[static_cast<size_t>(row)];
        if (!s.seeded) {
            s.seeded = true;
            s.last   = wrapped;
            s.abs    = ref_abs + shortest(wrapped - ref_wrapped);
            return;
        }
        s.abs += shortest(wrapped - s.last);
        s.last = wrapped;
    }

    std::vector<State> st_;
    int rows_  = 0;
    int width_ = 512;
};

}  // namespace ayther
