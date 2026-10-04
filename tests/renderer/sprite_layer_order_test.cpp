// Spec 002, R3 (BR-100 to BR-102; RF-8.1, RF-8.2, RF-8.4), on a GPU.
//
// The VDP sprite layer: between sprites the first of the link chain wins the
// pixel whatever its priority, and the winner enters below or above the
// planes by its own priority bit (B0, A0, sprites pri-0, B1, A1, sprites
// pri-1). The four crossed cases of BR-034, with original sprites and with
// an HD replacement in front, each compared pixel by pixel with the CPU
// reference compositor (GPU oracle).
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
constexpr std::int16_t kX = 120;
constexpr std::int16_t kY = 96;

ayther::SceneElement element(std::uint8_t layer, std::uint16_t pattern,
                             std::uint8_t priority, std::uint8_t chain) {
  ayther::SceneElement e{};
  e.hash = 0xD000U + layer * 64U + chain;
  e.x = kX;
  e.y = kY;
  e.w = 8;
  e.h = 8;
  e.pattern = pattern;
  e.layer = layer;
  e.priority = priority;
  if (layer == 3) {
    e.slot = chain;
    e.chain = chain;
  }
  return e;
}
} // namespace

int main() try {
  std::printf("=== sprite_layer_order_test (spec 002 R3) ===\n");
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path() / "ayther_sprite_layer_order";
  fs::create_directories(dir, ec);

  ayther::test::GpuOracle oracle;
  if (!oracle.init(kW, kH)) {
    std::fprintf(stderr, "[FAIL] GPU oracle init\n");
    return 1;
  }

  // Patterns 1..3 = colour indices 1..3 (red, green, blue); pattern 5 =
  // colour 1 on its left half and transparent on its right half.
  std::vector<std::uint8_t> vram(0x10000, 0);
  for (std::uint32_t p = 1; p <= 3; ++p)
    for (std::uint32_t b = 0; b < 32; ++b)
      vram[p * 32 + b] = static_cast<std::uint8_t>((p << 4) | p);
  for (std::uint32_t row = 0; row < 8; ++row)
    for (std::uint32_t col = 0; col < 2; ++col)
      vram[(5 * 32 + row * 4 + col) ^ 1U] = 0x11;
  std::vector<std::uint8_t> cram(128, 0);
  const std::uint16_t colours[4] = {0x0000, 0x0007, 0x0038, 0x01C0};
  for (int i = 1; i < 4; ++i) {
    cram[static_cast<std::size_t>(i) * 2] =
        static_cast<std::uint8_t>(colours[i] & 0xFF);
    cram[static_cast<std::size_t>(i) * 2 + 1] =
        static_cast<std::uint8_t>(colours[i] >> 8);
  }
  std::vector<std::uint16_t> fb(static_cast<std::size_t>(kW) * kH, 0);

  // An HD replacement of the red sprite: a red 8x8 PNG.
  const std::string red_png = (dir / "red.png").string();
  std::uint8_t red[3];
  ayther::test::cram_rgb8(colours[1], red);
  if (!ayther::test::write_solid_png(red_png, 8, 8, red))
    return 1;

  std::uint32_t frame = 1;
  const auto run = [&](const std::vector<ayther::SceneElement> &scene,
                       bool hd_front, const char *message) {
    // hd_front: the element with chain 0 is replaced by red.png.
    std::vector<ayther::SceneElement> drawn = scene;
    AytherSpriteSub sub{};
    std::uint8_t slot = 0;
    std::uint8_t prio = 0;
    std::uint8_t flip = 0;
    ayther::FrameView fv{};
    if (hd_front)
      for (ayther::SceneElement &e : drawn)
        if (e.layer == 3 && e.chain == 0) {
          e.claimed = 1;
          e.owner = 0;
          e.sub_kind = 1;
          e.sub = 0;
          std::snprintf(sub.asset_path, sizeof(sub.asset_path), "%s",
                        red_png.c_str());
          sub.screen_x = kX;
          sub.screen_y = kY;
          sub.w_tiles = 1;
          sub.h_tiles = 1;
          sub.w_px = 8;
          sub.h_px = 8;
          sub.palette = 0xFF;
          sub.synth_pal = 0xFF;
          sub.uw = 1.0F;
          sub.vh = 1.0F;
          prio = e.priority;
          fv.sprite_subs = &sub;
          fv.sprite_sub_count = 1;
          fv.sprite_sub_prio = &prio;
          fv.sprite_sub_slot = &slot;
          fv.sprite_sub_flips = &flip;
        }
    fv.fb_width = kW;
    fv.fb_height = kH;
    fv.fb_pixels = fb.data();
    fv.fb_pitch = kW * 2;
    fv.fb_format = 2;
    fv.scene = drawn.data();
    fv.scene_count = static_cast<std::uint32_t>(drawn.size());
    fv.scene_vram = vram.data();
    fv.scene_vram_size = vram.size();
    fv.scene_cram = cram.data();
    fv.scene_cram_size = cram.size();
    fv.frame_index = frame++;
    const ayther::probe::RgbImage want = oracle.expected(scene, vram, cram);
    const ayther::probe::RgbImage got = oracle.render(fv);
    const ayther::probe::O1Result r = ayther::probe::check_o1(got, want);
    std::printf("  differing=%zu first=(%d,%d)\n", r.differing_pixels,
                r.first_x, r.first_y);
    check(!got.rgb.empty() && r.identical, message);
  };

  // 1. A low sprite first in the chain over a high one: the low one wins.
  const std::vector<ayther::SceneElement> case1 = {element(3, 1, 0, 0),
                                                   element(3, 3, 1, 1)};
  run(case1, false,
      "RF-8.1: the first of the chain wins even when it is low priority");
  run(case1, true,
      "RF-8.4: an HD replacement first in the chain wins the same way");
  // 2. The low winner enters below a high plane cell; the high sprite behind
  //    it does not show through.
  const std::vector<ayther::SceneElement> case2 = {
      element(1, 2, 1, 0), element(3, 1, 0, 0), element(3, 3, 1, 1)};
  run(case2, false,
      "RF-8.2: the low winner goes below a high plane cell, and the high "
      "sprite behind it stays hidden");
  run(case2, true, "RF-8.4: the same with the low winner replaced by HD");
  // 3. A high sprite first in the chain over a low one, with a high cell.
  const std::vector<ayther::SceneElement> case3 = {
      element(1, 2, 1, 0), element(3, 1, 1, 0), element(3, 3, 0, 1)};
  run(case3, false, "RF-8.2: a high sprite first in the chain is above it all");
  run(case3, true, "RF-8.4: the same with the high winner replaced by HD");
  // 4. Through the transparent half of the front sprite, the next one wins
  //    with its own priority (here below a high cell of plane B).
  const std::vector<ayther::SceneElement> case4 = {
      element(0, 2, 1, 0), element(3, 5, 1, 0), element(3, 3, 0, 1)};
  run(case4, false,
      "RF-8.1, RF-8.2: through a transparent pixel the next sprite wins with "
      "its own priority");

  fs::remove_all(dir, ec);
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
