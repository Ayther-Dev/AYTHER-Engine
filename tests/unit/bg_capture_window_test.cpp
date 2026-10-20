// Fondos (Componentes), 2026-10-19: the background stitcher reads the partial
// column and row the screen shows when the camera rests on a fine offset
// (session/bg_capture_window.h), and only then: not while the camera moves,
// not even by a pixel inside its cell, and not when the nametable is no larger
// than the screen (the edge would wrap onto the first visible column). A
// negative camera's cell rounds down.
#include "session/bg_capture_window.h"

#include <cstdint>
#include <cstdio>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

namespace s = ayther::session;

static_assert(s::floor_div8(-3) == -1, "a camera left of the origin");
static_assert(s::fine8(-3) == 5, "its fine offset is positive");

s::CaptureEdgeInput resting(std::int64_t x, std::int64_t y) {
  s::CaptureEdgeInput in;
  in.cam_x = x;
  in.cam_y = y;
  in.cell_static = true;
  in.pixel_static = true;
  return in;
}
} // namespace

int main() {
  check(s::floor_div8(0) == 0 && s::floor_div8(7) == 0 && s::floor_div8(8) == 1,
        "floor_div8: non-negative pixels divide as usual");
  check(s::floor_div8(-1) == -1 && s::floor_div8(-8) == -1 &&
            s::floor_div8(-9) == -2,
        "floor_div8: negative pixels round down, not toward zero");
  check(s::fine8(0) == 0 && s::fine8(13) == 5 && s::fine8(16) == 0 &&
            s::fine8(-8) == 0 && s::fine8(-9) == 7,
        "fine8: the offset inside the cell is 0..7");

  // Golden Axe, "Go to Turtle Town": the pan ends 4 px into column 151, so
  // the screen shows columns 151..190 whole and part of column 191.
  {
    const s::CaptureEdges e = s::capture_edges(resting(1212, 0));
    check(e.column && !e.row,
          "resting on a fine x offset: the partial column is read");
  }
  {
    const s::CaptureEdges e = s::capture_edges(resting(1216, 0));
    check(!e.column && !e.row,
          "resting on a cell boundary: nothing past the window");
  }
  {
    s::CaptureEdgeInput in = resting(1212, 0);
    in.pixel_static = false;
    const s::CaptureEdges e = s::capture_edges(in);
    check(!e.column && !e.row,
          "moving a pixel inside its cell: the incoming column is not read");
  }
  {
    s::CaptureEdgeInput in = resting(1212, 3);
    in.cell_static = false;
    in.pixel_static = false;
    const s::CaptureEdges e = s::capture_edges(in);
    check(!e.column && !e.row, "a camera that changed cells reads no edge");
  }
  {
    s::CaptureEdgeInput in = resting(1212, 0);
    in.nt_cols = 32;
    in.scr_cols = 32;
    check(!s::capture_edges(in).column,
          "H32 over a 32-column nametable: the edge would wrap, not read");
    in.nt_cols = 64;
    check(s::capture_edges(in).column,
          "H32 over a 64-column nametable: the edge is a new column");
  }
  {
    const s::CaptureEdges row = s::capture_edges(resting(1216, 3));
    check(!row.column && row.row,
          "resting on a fine y offset: the partial row is read");
    const s::CaptureEdges both = s::capture_edges(resting(1212, 3));
    check(both.column && both.row,
          "fine on both axes: column, row and their corner");
    s::CaptureEdgeInput in = resting(1216, 3);
    in.nt_rows = 28;
    check(!s::capture_edges(in).row,
          "a nametable no taller than the screen: the row would wrap");
  }
  {
    const s::CaptureEdges e = s::capture_edges(resting(-3, 0));
    check(s::floor_div8(-3) == -1 && e.column,
          "a camera at -3 starts at column -1 and shows part of column 31/39");
  }

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
