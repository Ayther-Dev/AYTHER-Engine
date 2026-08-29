#pragma once
// ---------------------------------------------------------------------------
// widescreen.h — which level cells fill the extended area (EM-8.1).
//
// The extra width is NOT drawn from what sits in the live nametable. That has
// been measured and it does not add up: on the side you are heading towards the
// game streams 1-2 cells ahead, and 16:9 over 224 px asks for 5 per side (7 if
// the displayed 4:3 aspect is preserved). Plane A —the gameplay— simply has no
// lateral art.
//
//   side source           stale cells    without art
//   live nametable            182            185
//   level strip                 0            185
//
// "Stale" means art from ANOTHER section of the level: the nametable wraps every
// 512 px, and reading past it returns a band that does not belong. The stitcher
// strip —what each position showed while it was on screen— supplies the real
// level.
//
// This file is only the PLAN: which level position goes into each cell of the
// extended area. It does not draw, does not read VRAM and does not touch
// Vulkan, so it can be measured without a GPU and without a ROM — which is how
// the banding bug in EM-8.0 was found.
//
// And the row matters: on plane B each parallax band resolves its own column
// (see `parallax_bands.h`). An extended area that asked for a single column per
// side would leave gaps exactly where parallax separates the bands.
// ---------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>

namespace ayther {

/// How many 8 px cells must be added PER SIDE to reach `target_w_px`.
///
/// It rounds UP: drawing extra and cropping leaves a clean edge, while falling
/// short leaves a black strip against the frame that reads as a render bug.
/// Asymmetry is not an option — an asymmetric widening would flip every time
/// the player changes direction, because the surplus side is the trailing one
/// (the trail already travelled).
inline int widescreen_cols_per_side(int emu_w_px, int target_w_px) {
    if (target_w_px <= emu_w_px) return 0;
    const int per_side = (target_w_px - emu_w_px + 1) / 2;
    return (per_side + 7) / 8;
}

/// The screen width an aspect ratio `num:den` asks for over `emu_h_px`.
///
/// `par_num/par_den` is the PIXEL aspect ratio. With square pixels, 16:9 over
/// 224 px gives 398; preserving the displayed 4:3 aspect —which is what the
/// player sees on a CRT— gives 426. That is 5 cells per side against 7, and the
/// difference is visible: it is the decision the pack declares.
inline int widescreen_target_width(int emu_h_px, int num, int den,
                                   int par_num = 1, int par_den = 1) {
    if (emu_h_px <= 0 || den <= 0 || num <= 0 || par_num <= 0 || par_den <= 0)
        return 0;
    // visual_width = height · num/den  →  width_in_px = visual_width · par_den/par_num
    const int64_t w = (int64_t)emu_h_px * num * par_den;
    const int64_t d = (int64_t)den * par_num;
    return (int)((w + d / 2) / d);
}

/// One cell of the extended area: where it goes on screen and where it comes
/// from.
struct ExtCell {
    int dst_col;    ///< screen column, NEGATIVE on the left and ≥ emu_cols on the right
    int dst_row;    ///< screen row, 0..rows-1
    int level_col;  ///< level column to look up in the strip
    int level_row;  ///< level row
};

/// Builds the extended-area plan for ONE plane.
///
/// `band_offset(row)` returns how far the band of that row is from the topmost
/// band (`BandCameras::offset_from_top`). On a plane without per-band parallax
/// it is 0 and every row shares a column, which is the case for plane A.
///
/// `emit(ExtCell)` receives each cell. ALL of them are emitted, whether or not
/// the strip has art: whoever draws decides what to do with a gap, and that
/// decision —draw, freeze or leave the backdrop— does not belong to this file.
/// Returning only the ones with art would hide how much coverage is missing,
/// which is precisely the number to watch (measured: 185 gaps, all vertical).
template <typename BandOffset, typename Emit>
inline void widescreen_plan(int cols_per_side, int emu_cols, int rows,
                            int cam_col, int cam_row,
                            const BandOffset& band_offset, const Emit& emit) {
    if (cols_per_side <= 0 || emu_cols <= 0 || rows <= 0) return;
    for (int r = 0; r < rows; ++r) {
        const int lv_row = cam_row + r;
        const int dx     = band_offset(r);
        for (int c = 1; c <= cols_per_side; ++c) {
            // Left: the columns BEFORE the screen.
            emit(ExtCell{ -c, r, cam_col + dx - c, lv_row });
            // Right: the ones after it. `emu_cols + c - 1` and not `+ c`, or
            // the first new column would trample the last one on screen.
            emit(ExtCell{ emu_cols + c - 1, r, cam_col + dx + emu_cols + c - 1, lv_row });
        }
    }
}

/// How many cells of the plan have art in the strip, out of the total.
///
/// It is the measurement that decides whether the extended area can be shown:
/// with a short take the strip comes out WORSE than the nametable (measured:
/// 259 gaps against 240), not because of the method but for lack of coverage.
/// The pack needs a take that covers the level, and this is what says so before
/// it shows up on screen.
struct Coverage {
    int total = 0;
    int filled = 0;
    /// With no cells requested, coverage is COMPLETE, not zero: there is
    /// nothing to fill. Returning 0 would make "no widening" read as "art was
    /// missing".
    bool complete() const { return filled == total; }
    int  missing() const { return total - filled; }
};

template <typename BandOffset, typename Lookup>
inline Coverage widescreen_coverage(int cols_per_side, int emu_cols, int rows,
                                    int cam_col, int cam_row,
                                    const BandOffset& band_offset,
                                    const Lookup& has_cell) {
    Coverage cv;
    widescreen_plan(cols_per_side, emu_cols, rows, cam_col, cam_row, band_offset,
                    [&](const ExtCell& e) {
                        ++cv.total;
                        if (has_cell(e.level_col, e.level_row)) ++cv.filled;
                    });
    return cv;
}

}  // namespace ayther
