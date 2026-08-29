#pragma once
// ---------------------------------------------------------------------------
// pano_bands.h — the camera of a Panorama, VOTED PER BAND.
//
// THE PROBLEM. The Panorama models a rigid strip with ONE camera: every visible
// cell votes `cam_px = lx*8 - screen_x` and the mode wins. When the plane has
// line-scroll —bands that move at different rates within the SAME VDP layer—
// no single position explains them all: the cells of the fast band vote against
// those of the background. At best the mode wins and the minority band ends up
// misplaced; at worst the vote splits and the anchor never settles.
//
// THAT THE CASE EXISTS is measured, not assumed (2026-08-24,
// hscroll_bands_probe):
//
//   Golden Axe   3 takes, 40,854 frames   reg $B mode 0   0 bands
//   Ecco         1,800 frames             reg $B mode 0   0 bands
//   Aladdin      1,800 frames             reg $B mode 0   0 bands
//   Sonic 3 & K  1,800 frames             per-line table in 1,766
//                                         plane A: 1 band · plane B: 37
//
// Golden Axe is NOT the corpus for this feature —its title-screen clouds were
// resolved as two parallaxed Acetates—; Sonic 3 & Knuckles is.
//
// THE SHAPE OF THE SOLUTION. With 37 bands, declaring one drift per strip
// (direction 2 of the issue) is not enough: that would be 37 speeds the author
// would have to maintain by hand. The vote is PER BAND, which is what the
// hardware does.
//
// This file is only the VOTE: it groups and decides, it does not read VRAM,
// does not touch Vulkan and does not know what a Panorama is. Like
// `widescreen.h`, it can be measured without a GPU and without a ROM — which is
// how the banding bug in EM-8.0 was found.
// ---------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <vector>

#include "parallax_bands.h"   // hscroll_of_line: the same read as EM-8.0

namespace ayther {

/// One vote: a visible cell says where the camera would be if it had its way.
struct PanoVote {
    int32_t  screen_y;   ///< where it is seen (px) — decides which band it belongs to
    int32_t  cam_x;      ///< lx*8 - screen_x
    int32_t  cam_y;      ///< ly*8 - screen_y
};

/// The camera that won within a band of lines.
struct BandCam {
    int32_t  y0 = 0, y1 = 0;   ///< line range [y0, y1) of the band
    int32_t  cam_x = 0, cam_y = 0;
    uint32_t votes = 0;        ///< how many votes the winner got
    uint32_t total = 0;        ///< how many the band cast in total
    /// With no votes the band has NO camera: `total == 0` is not the same as
    /// "the camera is (0,0)". Confusing them draws the strip at the origin,
    /// which is a visible defect and hard to attribute.
    bool decided() const { return total != 0; }
    /// How solid the decision was: 1.0 = every cell agreed. Below ~0.5 the band
    /// is mixing two scrolls and is worth inspecting rather than trusting.
    float confidence() const {
        return total ? float(votes) / float(total) : 0.0f;
    }
};

/// Votes one camera per band.
///
/// `bands` holds the line cuts, in ascending order and non-overlapping: band i
/// covers [bands[i], bands[i+1]). They come from the VDP Hscroll table — the
/// contiguous runs of lines sharing the same scroll value, which is what
/// `hscroll_bands_probe` already counts and what `parallax_bands.h` models for
/// the EM-8.0 camera.
///
/// A `bands` with a single cut (or empty) returns ONE band covering everything,
/// and then this behaves EXACTLY like today's vote — which is what makes the
/// change safe for the 40,854 frames of Golden Axe measured without a single
/// band.
///
/// Tie-breaking is DETERMINISTIC, and that is why it lives here and not in the
/// caller: with a bare `>` the winner of a tie depends on the iteration order
/// of a hash, and that makes two identical runs differ (the trap that already
/// cost us in the Panorama vote). The one with the most votes wins; on a tie,
/// the smaller `cam_x`, and on a tie of that, the smaller `cam_y`.
inline std::vector<BandCam> pano_vote_by_band(const PanoVote* votes, size_t n,
                                              const int32_t* bands, size_t nbands,
                                              int32_t screen_h) {
    std::vector<BandCam> out;
    // No usable cuts: a single band covering the screen (the old model).
    if (!bands || nbands < 2) {
        out.push_back(BandCam{0, screen_h, 0, 0, 0, 0});
    } else {
        out.reserve(nbands - 1);
        for (size_t i = 0; i + 1 < nbands; ++i)
            out.push_back(BandCam{bands[i], bands[i + 1], 0, 0, 0, 0});
    }
    if (!votes || n == 0) return out;

    // One tally per band. There are few of them (37 in the worst measured
    // case) and few votes per band, so a linear vector beats a hash: fewer
    // allocations and, above all, a STABLE traversal order for tie-breaking.
    struct Cnt { int32_t cx, cy; uint32_t k; };
    std::vector<std::vector<Cnt>> tally(out.size());

    for (size_t v = 0; v < n; ++v) {
        const PanoVote& pv = votes[v];
        size_t b = out.size();   // outside every band = discarded
        for (size_t i = 0; i < out.size(); ++i)
            if (pv.screen_y >= out[i].y0 && pv.screen_y < out[i].y1) { b = i; break; }
        if (b == out.size()) continue;
        ++out[b].total;
        bool found = false;
        for (Cnt& c : tally[b])
            if (c.cx == pv.cam_x && c.cy == pv.cam_y) { ++c.k; found = true; break; }
        if (!found) tally[b].push_back(Cnt{pv.cam_x, pv.cam_y, 1});
    }

    for (size_t i = 0; i < out.size(); ++i) {
        const Cnt* best = nullptr;
        for (const Cnt& c : tally[i]) {
            if (!best || c.k > best->k ||
                (c.k == best->k && (c.cx < best->cx ||
                                    (c.cx == best->cx && c.cy < best->cy))))
                best = &c;
        }
        if (best) { out[i].cam_x = best->cx; out[i].cam_y = best->cy; out[i].votes = best->k; }
    }
    return out;
}

/// The band CUTS of a plane, read from the VDP Hscroll table.
///
/// Returns the boundaries in screen LINES for `pano_vote_by_band`: band i
/// covers [out[i], out[i+1]). It always starts at 0 and ends at `rows * 8`, so
/// a plane without parallax returns exactly {0, height} — a single band, and
/// the vote behaves as it always did.
///
/// `band_count()` (parallax_bands.h) answers HOW MANY there are; this answers
/// WHERE they are, which is what the vote needs. They share the table read and
/// the sampling every 8 lines: the VDP resolves scroll per cell or per line
/// depending on reg $0B, and sampling finer adds no bands — it adds noise.
///
/// `mask == 0` (reg $0B mode 0, whole-plane scroll) returns a single band
/// without reading anything: that is 100 % of the measured corpus except Sonic
/// 3 & Knuckles.
template <typename ReadU32>
inline std::vector<int32_t> pano_band_edges(const ReadU32& read_u32,
                                            uint32_t base, uint32_t mask,
                                            uint8_t plane, int rows) {
    std::vector<int32_t> out;
    const int32_t h = static_cast<int32_t>(rows > 0 ? rows * 8 : 0);
    out.push_back(0);
    if (mask == 0 || rows <= 0) { out.push_back(h); return out; }
    int last = hscroll_of_line(read_u32, base, mask, plane, 0);
    for (int r = 1; r < rows; ++r) {
        const int v = hscroll_of_line(read_u32, base, mask, plane, r * 8);
        if (v != last) { out.push_back(static_cast<int32_t>(r * 8)); last = v; }
    }
    out.push_back(h);
    return out;
}

}  // namespace ayther
