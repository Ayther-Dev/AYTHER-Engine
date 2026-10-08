// Spec 002, DI-20 (RF-10.1, RF-10.3, CV-5), on a GPU: in a raster band the
// pixels the core drew as in the previous frame keep the previous frame's
// composed HD, and only the pixels the writes changed show the core's image.
// Toma 3 frames 1572 and 1621 (the «Go to Turtle Town» box) showed a strip
// of the original background across the HD one. Without a usable previous
// frame (not k-1, or a sprite crossing the band) the band stays the core's
// image, as DI-17 decided.
#include "gpu_oracle.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

constexpr std::uint32_t kW = 320;
constexpr std::uint32_t kH = 224;
constexpr std::uint16_t kCramRed = 0x0007; // packed CRAM colour 1
constexpr std::uint16_t kFbRed = 0xF800;   // the core's image, RGB565
constexpr std::uint16_t kFbYellow = 0xFFE0;

ayther::SceneElement cell(std::int16_t x, std::int16_t y, std::int32_t sub) {
  ayther::SceneElement e{};
  e.hash = 0xB000U + static_cast<std::uint64_t>(x) * 256U + y;
  e.x = x;
  e.y = y;
  e.w = 8;
  e.h = 8;
  e.pattern = 1;
  e.layer = 0;
  e.claimed = 1;
  e.sub_kind = 2;
  e.sub = sub;
  return e;
}

AytherSpriteSub plane_sub(const std::string &asset, std::int16_t x,
                          std::int16_t y) {
  AytherSpriteSub s{};
  std::snprintf(s.asset_path, sizeof(s.asset_path), "%s", asset.c_str());
  s.screen_x = x;
  s.screen_y = y;
  s.w_tiles = 1;
  s.h_tiles = 1;
  s.palette = 0xFF;
  s.synth_pal = 0xFF;
  s.uw = 1.0F;
  s.vh = 1.0F;
  return s;
}

bool blue_at(const ayther::probe::RgbImage &img, int x, int y) {
  const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 3;
  return img.rgb[i] < 40 && img.rgb[i + 1] < 40 && img.rgb[i + 2] > 200;
}
bool red_at(const ayther::probe::RgbImage &img, int x, int y) {
  const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 3;
  return img.rgb[i] > 200 && img.rgb[i + 1] < 40 && img.rgb[i + 2] < 40;
}
bool yellow_at(const ayther::probe::RgbImage &img, int x, int y) {
  const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 3;
  return img.rgb[i] > 200 && img.rgb[i + 1] > 200 && img.rgb[i + 2] < 40;
}
} // namespace

int main() try {
  std::printf("=== band_carry_gpu_test (spec 002 DI-20) ===\n");
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path() / "ayther_band_carry";
  fs::create_directories(dir, ec);
  const std::string blue_png = (dir / "blue.png").string();
  const std::uint8_t blue[3] = {0, 0, 255};
  if (!ayther::test::write_solid_png(blue_png, 8, 8, blue))
    return 1;

  ayther::test::GpuOracle oracle;
  if (!oracle.init(kW, kH)) {
    std::fprintf(stderr, "[FAIL] GPU oracle init\n");
    return 1;
  }

  std::vector<std::uint8_t> vram(0x10000, 0);
  for (int i = 0; i < 32; ++i)
    vram[32 + i] = 0x11;
  std::vector<std::uint8_t> cram(128, 0);
  cram[2] = kCramRed & 0xFF;

  // Plane B cells (80..95, 40..55), red in the core, replaced by blue HD.
  std::vector<ayther::SceneElement> scene;
  std::vector<AytherSpriteSub> subs;
  std::vector<std::uint16_t> fb(static_cast<std::size_t>(kW) * kH, 0);
  for (std::int16_t cy = 40; cy < 56; cy += 8)
    for (std::int16_t cx = 80; cx < 96; cx += 8) {
      scene.push_back(cell(cx, cy, static_cast<std::int32_t>(subs.size())));
      subs.push_back(plane_sub(blue_png, cx, cy));
      for (int y = cy; y < cy + 8; ++y)
        for (int x = cx; x < cx + 8; ++x)
          fb[static_cast<std::size_t>(y) * kW + x] = kFbRed;
    }
  std::vector<std::uint8_t> flips(subs.size(), 0);

  ayther::FrameView previous{};
  previous.fb_width = kW;
  previous.fb_height = kH;
  previous.fb_pixels = fb.data();
  previous.fb_pitch = kW * 2;
  previous.fb_format = 2;
  previous.scene = scene.data();
  previous.scene_count = static_cast<std::uint32_t>(scene.size());
  previous.scene_vram = vram.data();
  previous.scene_vram_size = vram.size();
  previous.scene_cram = cram.data();
  previous.scene_cram_size = cram.size();
  previous.plane_tile_subs = subs.data();
  previous.plane_tile_sub_count = static_cast<std::uint32_t>(subs.size());
  previous.plane_tile_sub_hi = static_cast<std::uint32_t>(subs.size());
  previous.plane_tile_flips = flips.data();
  previous.frame_index = 9;

  const ayther::probe::RgbImage before = oracle.render(previous);
  check(!before.rgb.empty() && blue_at(before, 88, 48),
        "the previous frame composes the plane cells in HD");

  // Frame k: the game draws a box edge on line 50 in mid-screen; the session
  // localizes the band [32, 64).
  std::vector<std::uint16_t> fb2 = fb;
  for (int x = 200; x < 220; ++x)
    fb2[static_cast<std::size_t>(50) * kW + x] = kFbYellow;
  ayther::FrameView current = previous;
  current.fb_pixels = fb2.data();
  current.frame_index = 10;
  // A palette write: DI-22 composes pattern-only bands in HD, so the
  // carry of DI-20 is exercised with another raster reason.
  current.raster_reasons = 1U << 1;
  current.raster_band_count = 1;
  current.raster_bands[0][0] = 32;
  current.raster_bands[0][1] = 64;

  const ayther::probe::RgbImage got = oracle.render(current);
  check(!got.rgb.empty() && blue_at(got, 88, 48) && blue_at(got, 84, 42),
        "DI-20 (RF-10.3): unchanged band pixels keep the previous frame's HD");
  check(!got.rgb.empty() && yellow_at(got, 210, 50),
        "DI-20 (RF-10.1): the pixels the write changed show the core's image");

  // Not frame k-1 (a seek): nothing to carry, the band is the core's image.
  {
    (void)oracle.render(previous);
    ayther::FrameView jump = current;
    jump.frame_index = 12;
    const ayther::probe::RgbImage after_seek = oracle.render(jump);
    check(!after_seek.rgb.empty() && red_at(after_seek, 88, 48),
          "DI-17: after a seek the band is the core's image");
  }

  // A sprite crossing the band: the previous HD may hold it elsewhere, so
  // the band is the core's image.
  {
    (void)oracle.render(previous);
    std::vector<ayther::SceneElement> with_sprite = scene;
    ayther::SceneElement s{};
    s.hash = 0x5151;
    s.x = 150;
    s.y = 56;
    s.w = 16;
    s.h = 16;
    s.pattern = 1;
    s.layer = 3;
    with_sprite.push_back(s);
    ayther::FrameView crossed = current;
    crossed.scene = with_sprite.data();
    crossed.scene_count = static_cast<std::uint32_t>(with_sprite.size());
    const ayther::probe::RgbImage got_sprite = oracle.render(crossed);
    check(!got_sprite.rgb.empty() && red_at(got_sprite, 88, 48),
          "DI-17: a sprite crossing the band keeps the core's image");
  }

  // DI-22 (CV-5, Toma 3 frame 1645): with HD substitutions, a band caused
  // only by pattern writes (text, tiles) is composed in HD from the frame's
  // final state; the core's original box never shows inside the HD one.
  {
    (void)oracle.render(previous);
    ayther::FrameView patterns = current;
    patterns.frame_index = 10;
    patterns.raster_reasons = 1U << 6;
    const ayther::probe::RgbImage composed = oracle.render(patterns);
    check(!composed.rgb.empty() && blue_at(composed, 88, 48) &&
              !yellow_at(composed, 210, 50),
          "DI-22: a pattern-only band with HD is composed from the final "
          "state");
    // Without any HD the band stays the core's image: O1 is unchanged.
    ayther::FrameView plain = patterns;
    plain.plane_tile_subs = nullptr;
    plain.plane_tile_sub_count = 0;
    plain.plane_tile_sub_hi = 0;
    for (ayther::SceneElement &e : scene) {
      e.claimed = 0;
      e.sub = -1;
      e.sub_kind = 0;
    }
    plain.frame_index = 30;
    const ayther::probe::RgbImage original = oracle.render(plain);
    check(!original.rgb.empty() && yellow_at(original, 210, 50),
          "DI-22 (O1): without HD a pattern band is the core's image");
  }

  fs::remove_all(dir, ec);
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
