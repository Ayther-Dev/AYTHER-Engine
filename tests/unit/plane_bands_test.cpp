// Spec 002, BR-111 and BR-113 (RF-10.1, RF-8.2), CPU part: planes A and B
// placed per scroll band (session/plane_bands.h) compose exactly what the
// VDP draws pixel by pixel under 2-cell column vscroll, per-line hscroll and
// both together (O1 on synthetic scenes: the reference compositor over the
// banded cells against an independent per-pixel VDP reference).
#include "scroll_scene.h"
#include "vdp_plane_reference.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

using ayther::test::ScrollScene;

std::size_t differing(const ScrollScene &s) {
  const std::vector<ayther::SceneElement> elements = s.elements();
  ayther::test::ReferenceScene scene;
  scene.width = s.width;
  scene.height = s.height;
  scene.elements = elements;
  scene.vram = s.vram;
  scene.cram = s.cram;
  const std::vector<std::uint16_t> got = ayther::test::compose_reference(scene);
  const std::vector<std::uint16_t> want = ayther::test::compose_vdp_planes(
      {s.vram, s.regs, s.vsram, s.cram, s.width, s.height});
  std::size_t diff = 0;
  for (std::size_t i = 0; i < got.size() && i < want.size(); ++i)
    diff += got[i] != want[i] ? 1 : 0;
  std::printf("  cells=%zu differing=%zu\n", elements.size(), diff);
  return diff;
}
} // namespace

int main() try {
  const ScrollScene column(ScrollScene::Mode::column_vscroll);
  check(ayther::session::needs_bands(column.input()),
        "column vscroll that varies needs bands");
  check(differing(column) == 0,
        "RF-10.1: with 2-cell column vscroll the banded planes match the VDP "
        "pixel by pixel");

  const ScrollScene lines(ScrollScene::Mode::line_hscroll);
  check(ayther::session::needs_bands(lines.input()),
        "per-line hscroll that varies needs bands");
  check(differing(lines) == 0,
        "RF-10.1: with per-line hscroll the banded planes match the VDP pixel "
        "by pixel");

  const ScrollScene both(ScrollScene::Mode::both);
  check(differing(both) == 0,
        "RF-10.1: with both at once the banded planes match the VDP");

  // A uniform scroll needs no bands.
  ScrollScene flat(ScrollScene::Mode::line_hscroll);
  for (std::size_t i = 0xFC00; i < 0x10000; ++i)
    flat.vram[i] = 0;
  check(!ayther::session::needs_bands(flat.input()),
        "a uniform scroll needs no bands");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
