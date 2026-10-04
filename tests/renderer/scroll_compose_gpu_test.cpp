// Spec 002, BR-111 and BR-113 (RF-10.1, RF-8.2), on a GPU: the renderer
// composes planes A and B placed per scroll band (each cell clipped to its
// band) and matches, pixel by pixel, an independent per-pixel VDP reference
// under 2-cell column vscroll, per-line hscroll and both (O1 on synthetic
// scenes without a pack).
#include "gpu_oracle.h"
#include "scroll_scene.h"
#include "vdp_plane_reference.h"

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
} // namespace

int main() try {
  std::printf("=== scroll_compose_gpu_test (spec 002 R8) ===\n");
  ayther::test::GpuOracle oracle;
  if (!oracle.init(320, 224)) {
    std::fprintf(stderr, "[FAIL] GPU oracle init\n");
    return 1;
  }
  std::uint32_t frame = 1;
  const auto run = [&](const ScrollScene &s, const char *message) {
    const std::vector<ayther::SceneElement> scene = s.elements();
    std::vector<std::uint16_t> fb(320U * 224U, 0);
    ayther::FrameView fv{};
    fv.fb_width = 320;
    fv.fb_height = 224;
    fv.fb_pixels = fb.data();
    fv.fb_pitch = 320 * 2;
    fv.fb_format = 2;
    fv.scene = scene.data();
    fv.scene_count = static_cast<std::uint32_t>(scene.size());
    fv.scene_vram = s.vram.data();
    fv.scene_vram_size = s.vram.size();
    fv.scene_cram = s.cram.data();
    fv.scene_cram_size = s.cram.size();
    fv.frame_index = frame++;
    const std::vector<std::uint16_t> want565 = ayther::test::compose_vdp_planes(
        {s.vram, s.regs, s.vsram, s.cram, s.width, s.height});
    const ayther::probe::RgbImage want =
        ayther::probe::core_image(want565.data(), 320, 224, 640, 2);
    const ayther::probe::RgbImage got = oracle.render(fv);
    const ayther::probe::O1Result r = ayther::probe::check_o1(got, want);
    std::printf("  cells=%zu differing=%zu first=(%d,%d)\n", scene.size(),
                r.differing_pixels, r.first_x, r.first_y);
    check(!got.rgb.empty() && !scene.empty() && r.identical, message);
  };
  run(ScrollScene(ScrollScene::Mode::column_vscroll),
      "RF-10.1: column vscroll composed exactly (GPU = VDP reference)");
  run(ScrollScene(ScrollScene::Mode::line_hscroll),
      "RF-10.1: per-line hscroll composed exactly (GPU = VDP reference)");
  run(ScrollScene(ScrollScene::Mode::both),
      "RF-10.1: both composed exactly (GPU = VDP reference)");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
