// Fondos (Componentes), 2026-10-19: a pan that ends with the camera resting on
// a fine offset stitches the partial column the screen shows
// (session/bg_capture_window.h), and a Panorama ignores the cells outside its
// rectangle (build_panorama).
//
// "Go to Turtle Town" (Golden Axe) came out 191 columns wide with 192 on
// screen: the stitcher read whole cells only and nobody saw the last column
// again. The content index did include it, so the Panorama claimed those
// tiles where its HD quad drew nothing and the backdrop showed through.
//
// The test core's pan scenario (tools/test_core) pans plane A one pixel per
// frame over a 62-column level and streams each column two cells before it is
// on screen; the byte after the tag picks where the pan rests. Every level
// cell has its own pattern, 1 + ly * 62 + (lx + 2), so a scene element says
// which level cell it is.
#include <ayther/ayther_session.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

// tools/test_core/ayther_test_core.cpp, pan scenario.
constexpr char kTag[] = "AYTHER-PAN-SCENARIO";
constexpr std::uint8_t kRestFine = 0;    // rests at x = 197: cell 24, 5 px in
constexpr std::uint8_t kRestAligned = 1; // rests at x = 200: cell 25
constexpr std::uint8_t kRestLeft = 2;    // rests at x = -3: cell -1, 5 px in
constexpr int kPanStartFrame = 8;
constexpr int kFirstCol = -2;
constexpr int kCols = 62;

constexpr int kCaptureFrom = 5; // the core's geometry settles at frame 3
constexpr int kRestedFrame = 240;
constexpr int kMovingFrame = kPanStartFrame + 100; // x = 100, moving

using Session = ayther::AytherSession;

std::unique_ptr<Session> open(std::uint8_t variant) {
  const auto rom_path = std::filesystem::temp_directory_path() /
                        ("ayther_pan_" + std::to_string(variant) + ".md");
  {
    std::vector<char> rom(0x10000, 0);
    std::copy(std::begin(kTag), std::end(kTag) - 1, rom.begin());
    rom[sizeof(kTag) - 1] = static_cast<char>(variant);
    std::ofstream out(rom_path, std::ios::binary);
    out.write(rom.data(), static_cast<std::streamsize>(rom.size()));
    if (!out)
      return nullptr;
  }
  Session::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = rom_path.string();
  config.enable_audio = false;
  config.derive_core_pack = false;
  auto created = Session::create(config);
  if (!created) {
    std::fprintf(stderr, "[FAIL] %s\n", created.error.message.c_str());
    return nullptr;
  }
  return std::move(*created);
}

/// PNG width from the IHDR chunk (bytes 16..19, big-endian).
int png_width(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  unsigned char head[24] = {};
  in.read(reinterpret_cast<char *>(head), sizeof(head));
  if (!in)
    return -1;
  return (head[16] << 24) | (head[17] << 16) | (head[18] << 8) | head[19];
}

struct Capture {
  bool ok = false;
  std::int32_t b[4] = {}; ///< min_x, min_y, max_x, max_y
  std::vector<Session::PanoramaCell> cells;
  int png_w = -1;
};

/// Steps `s` to `last_frame` (inclusive) with plane A captured from
/// kCaptureFrom on, and reads what the stitcher kept. The capture stays on.
Capture capture(Session &s, int last_frame, const char *png_name) {
  Capture c;
  for (int frame = 0; frame <= last_frame; ++frame) {
    if (frame == kCaptureFrom)
      s.bg_capture(true);
    s.step();
  }
  c.ok = s.bg_bounds(0, c.b);
  c.cells = s.bg_cells(0);
  const auto png = std::filesystem::temp_directory_path() / png_name;
  if (s.export_background_plane(0, png.string()))
    c.png_w = png_width(png);
  std::printf("  bounds x %d..%d y %d..%d, %zu cells, png %d px\n", c.b[0],
              c.b[2], c.b[1], c.b[3], c.cells.size(), c.png_w);
  return c;
}

bool cells_inside(const Capture &c) {
  return std::all_of(c.cells.begin(), c.cells.end(), [&](const auto &cell) {
    return cell.lx >= c.b[0] && cell.lx <= c.b[2] && cell.ly >= c.b[1] &&
           cell.ly <= c.b[3];
  });
}

/// Claimed state of the plane A cells of level column `lx` on screen:
/// -1 none on screen, 0 none claimed, 1 some claimed, 2 all claimed.
int claimed_in_column(const Session &s, int lx) {
  std::vector<Session::SceneElement> scene;
  s.scene_inventory(scene);
  int seen = 0, claimed = 0;
  for (const Session::SceneElement &e : scene) {
    if (e.layer != 1 || e.pattern == 0) // 1 = plane A
      continue;
    const int index = e.pattern - 1;
    if (index >= kCols * 24 || index % kCols + kFirstCol != lx)
      continue;
    ++seen;
    claimed += e.claimed ? 1 : 0;
  }
  if (!seen)
    return -1;
  return claimed == 0 ? 0 : (claimed == seen ? 2 : 1);
}
} // namespace

int main() try {
  // 1. Resting on a fine offset: columns 24..55 whole and part of 56.
  {
    auto s = open(kRestFine);
    if (!s)
      return 1;
    const Capture c = capture(*s, kRestedFrame, "ayther_pan_fine.png");
    check(c.ok && c.b[0] == 0 && c.b[2] == 56,
          "a pan resting 5 px into cell 24 stitches the partial column 56");
    check(c.ok && c.b[1] == 0 && c.b[3] == 23,
          "rows 0..23, nothing past the visible lines");
    check(c.png_w == (c.b[2] - c.b[0] + 1) * 8,
          "the exported strip is as wide as the bounds");
    check(cells_inside(c), "every content cell is inside the bounds");

    // 2. The Panorama ignores cells outside its rectangle: one column
    // narrower than the capture leaves column 56 to the original tiles.
    s->define_panorama(0xA1, 0, 0, 0, 56, 24, c.cells.data(),
                       static_cast<std::uint32_t>(c.cells.size()),
                       "graphics/pan.png");
    check(s->step().panorama_valid,
          "the Panorama anchors at the resting camera");
    check(claimed_in_column(*s, 40) == 2,
          "inside the rectangle, the strip claims its cells");
    check(claimed_in_column(*s, 56) == 0,
          "outside the rectangle, nothing is claimed");
    s->define_panorama(0xA1, 0, 0, 0, 0, 0, c.cells.data(),
                       static_cast<std::uint32_t>(c.cells.size()),
                       "graphics/pan.png");
    s->step();
    check(claimed_in_column(*s, 56) == 2,
          "w = h = 0 is no rectangle: every cell is kept");
  }
  // 3. Resting on a cell boundary: columns 25..56, and the column streamed
  // ahead (57) is not on screen.
  {
    auto s = open(kRestAligned);
    if (!s)
      return 1;
    const Capture c = capture(*s, kRestedFrame, "ayther_pan_aligned.png");
    check(c.ok && c.b[0] == 0 && c.b[2] == 56,
          "a pan resting on a cell boundary reads nothing past the screen");
    check(cells_inside(c), "every content cell is inside the bounds");
  }
  // 4. Still moving when the capture is read: the incoming column is not
  // read, as before (cell 12, 4 px in: columns 12..43 whole).
  {
    auto s = open(kRestFine);
    if (!s)
      return 1;
    const Capture c = capture(*s, kMovingFrame, "ayther_pan_moving.png");
    check(c.ok && c.b[2] == 43,
          "a moving camera stitches what it did before the edge rule");
  }
  // 5. Resting 3 px left of the origin: the camera is in cell -1.
  {
    auto s = open(kRestLeft);
    if (!s)
      return 1;
    const Capture c = capture(*s, 40, "ayther_pan_left.png");
    check(c.ok && c.b[0] == -1 && c.b[2] == 31,
          "a camera at x = -3 stitches columns -1..31");
    check(c.png_w == (c.b[2] - c.b[0] + 1) * 8,
          "the exported strip is as wide as the bounds");
  }

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
