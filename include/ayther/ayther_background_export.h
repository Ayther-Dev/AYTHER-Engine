#pragma once
// ---------------------------------------------------------------------------
// ayther_background_export.h — Fondos: reconstruct a plane's level strip into a
// **per-layer PNG** the artist repaints in HD (componentes-plan §3, the
// first-order deliverable).
//
// ## What this is
//
// `BackgroundStitcher` (core/src/background.rs) accumulates a scrolling plane's
// visible cells into a full level strip in level space over a `.arp` take — no
// single frame holds a level, since the nametable wraps. This exporter turns
// that accumulated strip into a dense RGBA image and writes it as a PNG whose
// name carries the **index** (plane + level origin + a layout hash), so when the
// artist brings the repainted HD back it maps unambiguously to its plane and
// position: `bg_A_t000x000_<hash>.png`.
//
// The pixels come from decoding each cell's pattern (via a `TileDecodeFn` — the
// engine passes AytherSession::decode_plane_tile). Cells the stitcher flagged as
// **animated** are still written (as their representative frame) and reported in
// `LayerImage::animated`, so the authoring UI can mark them.
//
// ## STATUS — contract/skeleton (env-blocked)
//
// The core stitcher + FFI are done and validated (tools/background_spike: 167-tile
// strip, 0 conflicts). This header + ayther_background_export.cpp are the
// engine-side consumer, ready to wire once the engine builds (needs vcpkg for
// stb_image_write). ayther_background_export.cpp is NOT yet in
// the root CMakeLists.txt — it type-checks standalone (see its header comment).
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ayther_core_ffi.h"   // AytherBgStitcher
#include "ayther_result.h"     // ayther::Result / Error

namespace ayther {

// A reconstructed background layer: the dense pixel image plus the index that
// names its PNG (so re-imported HD maps back to plane/position).
struct LayerImage {
    uint8_t  plane       = 0;    ///< 0 = Plano A · 1 = Plano B · 2 = Window
    int32_t  origin_x    = 0;    ///< top-left level tile (min_x) — filename index
    int32_t  origin_y    = 0;    ///< min_y
    uint32_t width_px    = 0;
    uint32_t height_px   = 0;
    uint32_t cell_count  = 0;    ///< non-empty cells filled in
    uint32_t animated    = 0;    ///< cells the stitcher flagged as animated
    uint64_t layout_hash = 0;    ///< hash of the code grid (filename + re-import key)
    std::vector<uint8_t> rgba;   ///< width*height*4, row-major; alpha 0 = hole
};

// Decode one plane tile (pattern + palette + flips) into an 8×8 **RGBA** buffer
// (256 B, stride 8 px). The engine adapts AytherSession::decode_plane_tile
// (which returns BGRA) by swapping R↔B, or uses an RGBA variant.
using TileDecodeFn =
    std::function<void(uint16_t pattern, uint8_t pal, bool hflip, bool vflip, uint8_t* out_rgba)>;

class BackgroundExporter {
public:
    // Build the dense image of `plane` from the stitched cells, decoding each
    // one's pattern via `decode`. Empty image (0×0) if the plane has no cells.
    static LayerImage build(const AytherBgStitcher* stitcher, uint8_t plane,
                            const TileDecodeFn& decode);

    // Canonical filename encoding the index:  bg_<A|B|W>_t<ox>x<oy>_<hash16>.png
    // (ox/oy = signed level-tile origin; hash16 = layout_hash in hex).
    static std::string filename(const LayerImage& img);

    // Write `img` as a PNG (RGBA) via stb_image_write. Error on an empty image
    // or an I/O failure.
    static Result<void> write_png(const LayerImage& img, const std::string& path);
};

}  // namespace ayther
