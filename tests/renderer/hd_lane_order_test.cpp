// Spec 002, R4 (RF-8.1, RF-8.4), on a GPU: the HD lanes in their pass.
//
// BR-097: a low-priority plane cell replaced by an HD asset, with a pri-0
// sprite over it. The VDP draws the cell (plane B, low) before the pri-0
// sprites, so the sprite stays in front of the HD background. The GPU oracle
// compares the renderer with the CPU reference compositor pixel by pixel,
// the HD asset being the same colour as the cell it replaces.
#include "gpu_oracle.h"
#include "session/plane_set_split.h"

#include <array>
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
constexpr std::uint16_t kRed = 0x0007;   // packed CRAM colour 1
constexpr std::uint16_t kGreen = 0x0038; // packed CRAM colour 2

ayther::SceneElement cell(std::int16_t x, std::int16_t y, std::uint8_t layer,
                          std::uint8_t priority, std::uint16_t pattern) {
  ayther::SceneElement e{};
  e.hash = 0xC000U + static_cast<std::uint64_t>(x) * 256U + y;
  e.x = x;
  e.y = y;
  e.w = 8;
  e.h = 8;
  e.pattern = pattern;
  e.layer = layer;
  e.priority = priority;
  return e;
}

ayther::SceneElement sprite(std::int16_t x, std::int16_t y,
                            std::uint8_t priority, std::uint8_t chain) {
  ayther::SceneElement e = cell(x, y, 3, priority, 1);
  e.slot = chain;
  e.chain = chain;
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
} // namespace

int main() try {
  std::printf("=== hd_lane_order_test (spec 002 R4) ===\n");
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path() / "ayther_hd_lane_order";
  fs::create_directories(dir, ec);
  const std::string green_png = (dir / "green.png").string();
  std::uint8_t green[3];
  ayther::test::cram_rgb8(kGreen, green);
  if (!ayther::test::write_solid_png(green_png, 8, 8, green))
    return 1;

  ayther::test::GpuOracle oracle;
  if (!oracle.init(kW, kH)) {
    std::fprintf(stderr, "[FAIL] GPU oracle init\n");
    return 1;
  }

  // Pattern 1 = colour 1 (red), pattern 2 = colour 2 (green).
  std::vector<std::uint8_t> vram(0x10000, 0);
  for (int i = 0; i < 32; ++i) {
    vram[32 + i] = 0x11;
    vram[64 + i] = 0x22;
  }
  std::vector<std::uint8_t> cram(128, 0);
  cram[2] = kRed & 0xFF;
  cram[4] = kGreen & 0xFF;
  std::vector<std::uint16_t> fb(static_cast<std::size_t>(kW) * kH, 0);

  // BR-097: plane B low cells at (80,80)..(95,95) replaced by HD (green);
  // a pri-0 sprite at (84,84) over them.
  std::vector<ayther::SceneElement> scene;
  std::vector<AytherSpriteSub> subs;
  for (std::int16_t cy = 80; cy < 96; cy += 8)
    for (std::int16_t cx = 80; cx < 96; cx += 8) {
      ayther::SceneElement c = cell(cx, cy, 0, 0, 2);
      c.claimed = 1;
      c.sub_kind = 2;
      c.sub = static_cast<std::int32_t>(subs.size());
      scene.push_back(c);
      subs.push_back(plane_sub(green_png, cx, cy));
    }
  scene.push_back(sprite(84, 84, 0, 0));
  std::vector<std::uint8_t> flips(subs.size(), 0);

  ayther::FrameView fv{};
  fv.fb_width = kW;
  fv.fb_height = kH;
  fv.fb_pixels = fb.data();
  fv.fb_pitch = kW * 2;
  fv.fb_format = 2;
  fv.scene = scene.data();
  fv.scene_count = static_cast<std::uint32_t>(scene.size());
  fv.scene_vram = vram.data();
  fv.scene_vram_size = vram.size();
  fv.scene_cram = cram.data();
  fv.scene_cram_size = cram.size();
  fv.plane_tile_subs = subs.data();
  fv.plane_tile_sub_count = static_cast<std::uint32_t>(subs.size());
  fv.plane_tile_sub_hi = static_cast<std::uint32_t>(subs.size());
  fv.plane_tile_flips = flips.data();
  fv.frame_index = 1;

  // The expected image: the cells as their (green) originals.
  std::vector<ayther::SceneElement> reference = scene;
  for (ayther::SceneElement &e : reference)
    if (e.layer == 0) {
      e.claimed = 0;
      e.sub = -1;
      e.sub_kind = 0;
    }
  const ayther::probe::RgbImage want = oracle.expected(reference, vram, cram);
  const ayther::probe::RgbImage got = oracle.render(fv);
  const ayther::probe::O1Result r = ayther::probe::check_o1(got, want);
  std::printf("  differing=%zu first=(%d,%d)\n", r.differing_pixels, r.first_x,
              r.first_y);
  check(!got.rgb.empty() && r.identical,
        "RF-8.1: a pri-0 sprite stays in front of a low HD plane cell (GPU "
        "matches the CPU compositor)");

  // BR-098: a two-cell plane set on plane B — left cell pri-0, right cell
  // pri-1 — replaced by one HD asset, with a pri-0 sprite over both cells.
  // The VDP shows the sprite over the left cell and hides it behind the
  // right one; the set's replacement has to be drawn in both passes.
  {
    const std::string set_png = (dir / "set.png").string();
    if (!ayther::test::write_solid_png(set_png, 16, 8, green))
      return 1;
    const std::int16_t ox = 160;
    const std::int16_t oy = 120;
    const std::vector<ayther::session::SetCell> cells = {{0, 0, 0}, {1, 0, 1}};
    const std::vector<ayther::session::SetQuad> quads =
        ayther::session::split_set_by_priority(2, 1, cells, 1);
    std::vector<AytherSpriteSub> set_subs;
    std::uint32_t hi = 0;
    for (const std::uint8_t pri : {std::uint8_t{0}, std::uint8_t{1}}) {
      if (pri == 1)
        hi = static_cast<std::uint32_t>(set_subs.size());
      for (const ayther::session::SetQuad &q : quads) {
        if (q.priority != pri)
          continue;
        AytherSpriteSub s =
            plane_sub(set_png, static_cast<std::int16_t>(ox + q.x * 8),
                      static_cast<std::int16_t>(oy + q.y * 8));
        s.w_tiles = static_cast<std::uint8_t>(q.w);
        s.h_tiles = static_cast<std::uint8_t>(q.h);
        s.w_px = static_cast<std::uint16_t>(q.w * 8);
        s.h_px = static_cast<std::uint16_t>(q.h * 8);
        s.u0 = static_cast<float>(q.x) / 2.0F;
        s.v0 = 0.0F;
        s.uw = static_cast<float>(q.w) / 2.0F;
        s.vh = 1.0F;
        set_subs.push_back(s);
      }
    }
    // The scene: the two cells, claimed and linked to the quad over them.
    std::vector<ayther::SceneElement> set_scene;
    for (const ayther::session::SetCell &c : cells) {
      ayther::SceneElement e =
          cell(static_cast<std::int16_t>(ox + c.dx * 8), oy, 0, c.priority, 2);
      e.claimed = 1;
      e.sub_kind = 2;
      for (std::size_t q = 0; q < set_subs.size(); ++q)
        if (e.x >= set_subs[q].screen_x &&
            e.x < set_subs[q].screen_x + set_subs[q].w_px)
          e.sub = static_cast<std::int32_t>(q);
      set_scene.push_back(e);
    }
    set_scene.push_back(sprite(ox + 4, oy, 0, 0));
    std::vector<std::uint8_t> set_flips(set_subs.size(), 0);
    ayther::FrameView sv = fv;
    sv.scene = set_scene.data();
    sv.scene_count = static_cast<std::uint32_t>(set_scene.size());
    sv.plane_tile_subs = set_subs.data();
    sv.plane_tile_sub_count = static_cast<std::uint32_t>(set_subs.size());
    sv.plane_tile_sub_hi = hi;
    sv.plane_tile_flips = set_flips.data();
    std::vector<ayther::SceneElement> set_ref = set_scene;
    for (ayther::SceneElement &e : set_ref)
      if (e.layer == 0) {
        e.claimed = 0;
        e.sub = -1;
        e.sub_kind = 0;
      }
    const ayther::probe::RgbImage set_want =
        oracle.expected(set_ref, vram, cram);
    const ayther::probe::RgbImage set_got = oracle.render(sv);
    const ayther::probe::O1Result sr =
        ayther::probe::check_o1(set_got, set_want);
    std::printf("  set: quads=%zu differing=%zu first=(%d,%d)\n", quads.size(),
                sr.differing_pixels, sr.first_x, sr.first_y);
    check(!set_got.rgb.empty() && sr.identical,
          "RF-8.4: a set with pri-0 and pri-1 cells is drawn in both passes "
          "(GPU matches the CPU compositor)");
  }

  fs::remove_all(dir, ec);
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
