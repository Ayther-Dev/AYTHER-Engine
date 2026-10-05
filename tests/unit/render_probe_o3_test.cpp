// Spec 002, DI-17 (RF-10.1, RF-10.3): the O3 continuity check of the render
// probe. From one frame to the next, a block of the composed image may only
// change where the core's image changes too: HD that appears or disappears
// over a still original (a frame that drops its HD, a band) is a
// discontinuity not explained by the game.
#include "../../tools/render_probe/probe_o3.h"

#include <cstdint>
#include <cstdio>
#include <exception>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

using ayther::probe::RgbImage;

RgbImage solid(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
  RgbImage img{64, 32, {}};
  img.rgb.resize(64U * 32U * 3U);
  for (std::size_t i = 0; i < img.rgb.size(); i += 3) {
    img.rgb[i] = r;
    img.rgb[i + 1] = g;
    img.rgb[i + 2] = b;
  }
  return img;
}

void paint(RgbImage &img, int x0, int y0, int x1, int y1, std::uint8_t r,
           std::uint8_t g, std::uint8_t b) {
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 3;
      img.rgb[i] = r;
      img.rgb[i + 1] = g;
      img.rgb[i + 2] = b;
    }
}
} // namespace

int main() try {
  const RgbImage core = solid(10, 10, 10);
  const RgbImage hd = solid(200, 120, 40);

  {
    const ayther::probe::O3Result r =
        ayther::probe::check_o3(hd, hd, core, core);
    check(r.unexplained_blocks == 0, "a still frame is continuous");
  }
  {
    // The whole frame falls back to the core's image for a frame.
    const ayther::probe::O3Result r =
        ayther::probe::check_o3(hd, core, core, core);
    std::printf("  dropout: blocks=%u rows=[%d,%d)\n", r.unexplained_blocks,
                r.y0, r.y1);
    check(r.unexplained_blocks == 32 && r.y0 == 0 && r.y1 == 32,
          "HD that disappears over a still original is a discontinuity");
  }
  {
    // Only a band of 8 lines drops its HD.
    RgbImage banded = hd;
    paint(banded, 0, 8, 64, 16, 10, 10, 10);
    const ayther::probe::O3Result r =
        ayther::probe::check_o3(hd, banded, core, core);
    check(r.unexplained_blocks == 8 && r.y0 == 8 && r.y1 == 16,
          "a band that drops its HD is reported with its lines");
  }
  {
    // The game changes the same region: the composed change is explained.
    RgbImage core2 = core;
    paint(core2, 16, 0, 32, 16, 250, 250, 250);
    RgbImage hd2 = hd;
    paint(hd2, 16, 0, 32, 16, 0, 0, 255);
    const ayther::probe::O3Result r =
        ayther::probe::check_o3(hd, hd2, core, core2);
    check(r.unexplained_blocks == 0,
          "a composed change where the core changes is explained");
  }
  {
    // The camera scrolls: a textured HD sky moves a pixel over a flat
    // original that does not visibly change. Motion is not a discontinuity.
    RgbImage sky = solid(0, 0, 0);
    for (std::uint32_t y = 0; y < sky.height; ++y)
      for (std::uint32_t x = 0; x < sky.width; ++x) {
        const std::size_t i = (static_cast<std::size_t>(y) * sky.width + x) * 3;
        sky.rgb[i] = static_cast<std::uint8_t>(((x / 3) % 2) ? 220 : 20);
        sky.rgb[i + 1] = static_cast<std::uint8_t>(((y / 5) % 2) ? 200 : 40);
      }
    RgbImage moved = sky;
    for (std::uint32_t y = 0; y < sky.height; ++y)
      for (std::uint32_t x = 0; x + 1 < sky.width; ++x)
        for (int c = 0; c < 3; ++c)
          moved.rgb[(static_cast<std::size_t>(y) * sky.width + x) * 3 + c] =
              sky.rgb[(static_cast<std::size_t>(y) * sky.width + x + 1) * 3 +
                      c];
    const ayther::probe::O3Result r =
        ayther::probe::check_o3(sky, moved, core, core);
    std::printf("  scroll: blocks=%u\n", r.unexplained_blocks);
    check(r.unexplained_blocks == 0,
          "HD that scrolls with the camera is continuous");
  }
  {
    // A tiny change (noise, an anti-aliased edge) is below the threshold.
    RgbImage hd2 = hd;
    paint(hd2, 0, 0, 2, 2, 210, 120, 40);
    check(ayther::probe::check_o3(hd, hd2, core, core).unexplained_blocks == 0,
          "a change below the threshold is not reported");
  }
  {
    RgbImage small{8, 8, {}};
    small.rgb.resize(8 * 8 * 3, 0);
    check(ayther::probe::check_o3(hd, small, core, core).unexplained_blocks ==
              0,
          "images of different sizes are not compared");
  }
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
