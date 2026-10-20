// Fondos (Componentes): which cells of a plane the background stitcher reads
// in one frame. The screen shows 40 (H40) or 32 (H32) columns and 28 rows
// starting at the camera's cell; when the camera rests on a fine offset
// (cam_x % 8 != 0) it also shows part of the next column, and with
// cam_y % 8 != 0 part of the next row. The stitcher read the whole-cell window
// only, so a pan that ended on a fine offset never stitched its last column:
// "Go to Turtle Town" (Golden Axe) came out 191 columns wide while the screen
// showed 192, and its HD strip stopped short of the screen edge (2026-10-19).
// Pure: the session gives it the camera and the geometry.
#pragma once

#include <cstdint>

namespace ayther::session {

/// The cell that holds pixel `px`: floor(px / 8). C++ division truncates
/// toward zero, and a camera 3 px left of the origin (-3) is in cell -1, not 0.
constexpr std::int64_t floor_div8(std::int64_t px) {
  return px >= 0 ? px / 8 : -((-px + 7) / 8);
}

/// Pixels of the camera's cell left of the camera: 0..7.
constexpr int fine8(std::int64_t px) {
  return static_cast<int>(px - floor_div8(px) * 8);
}

struct CaptureEdgeInput {
  std::int64_t cam_x = 0, cam_y = 0; ///< plane camera, level px
  bool cell_static = false;          ///< same cell as the previous frame
  bool pixel_static = false;         ///< same pixel as the previous frame
  int nt_cols = 64, nt_rows = 32;    ///< nametable, cells
  int scr_cols = 40, scr_rows = 28;  ///< screen, whole cells
};

/// The partial edges to read: the column `cam_col + scr_cols`, the row
/// `cam_row + scr_rows`, and the corner when both.
struct CaptureEdges {
  bool column = false;
  bool row = false;
};

/// Only a resting camera has its partial edges read. A moving camera streams
/// its incoming column a cell or two before it is on screen (the stitcher
/// already skips the last columns while it moves); at rest, what the screen
/// shows is what the nametable holds. Resting in the same cell is not enough:
/// a camera moving a pixel per frame stays in its cell for 8 frames. The
/// nametable must also be larger than the screen on that axis; otherwise the
/// edge wraps onto the first visible column and would stitch it again one
/// screen further on.
constexpr CaptureEdges capture_edges(const CaptureEdgeInput &in) {
  CaptureEdges e;
  if (!in.cell_static || !in.pixel_static)
    return e;
  e.column = fine8(in.cam_x) != 0 && in.nt_cols > in.scr_cols;
  e.row = fine8(in.cam_y) != 0 && in.nt_rows > in.scr_rows;
  return e;
}

} // namespace ayther::session
