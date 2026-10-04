// Spec 002, BR-114 (RF-10.1, RF-10.3, RF-7.9), on a GPU: a frame that cannot
// be composed element by element (raster writes in mid-screen) is presented
// with the originals only — the core's image, no asset — so the original and
// its replacement are never seen together; the observation says why
// (assigned_not_applied, frame_not_composable).
#include "gpu_oracle.h"
#include "session/frame_composability.h"
#include "session/render_observation_builder.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}
namespace ro = ayther::engine::render_observation;
} // namespace

int main() try {
  std::printf("=== raster_frame_test (spec 002 BR-114) ===\n");
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path() / "ayther_raster_frame";
  fs::create_directories(dir, ec);
  const std::string green_png = (dir / "green.png").string();
  const std::uint8_t green[3] = {0, 255, 0};
  if (!ayther::test::write_solid_png(green_png, 16, 16, green))
    return 1;

  ayther::test::GpuOracle oracle;
  if (!oracle.init(320, 224)) {
    std::fprintf(stderr, "[FAIL] GPU oracle init\n");
    return 1;
  }
  // The core's image: a red 16x16 sprite at (100, 80) over black.
  std::vector<std::uint16_t> fb(320U * 224U, 0);
  for (int y = 80; y < 96; ++y)
    for (int x = 100; x < 116; ++x)
      fb[static_cast<std::size_t>(y) * 320 + x] = 0xF800;
  std::vector<std::uint8_t> vram(0x10000, 0);
  for (int i = 0; i < 4 * 32; ++i)
    vram[32 + i] = 0x11;
  std::vector<std::uint8_t> cram(128, 0);
  cram[2] = 0x07;

  ayther::SceneElement anchor{};
  anchor.hash = 0xF1;
  anchor.x = 100;
  anchor.y = 80;
  anchor.w = 16;
  anchor.h = 16;
  anchor.pattern = 1;
  anchor.layer = 3;
  anchor.slot = 0;
  anchor.chain = 0;
  anchor.claimed = 1;
  anchor.owner = 0;
  anchor.sub_kind = 1;
  anchor.sub = 0;
  AytherSpriteSub sub{};
  std::snprintf(sub.asset_path, sizeof(sub.asset_path), "%s",
                green_png.c_str());
  sub.screen_x = 100;
  sub.screen_y = 80;
  sub.w_tiles = 2;
  sub.h_tiles = 2;
  sub.w_px = 16;
  sub.h_px = 16;
  sub.palette = 0xFF;
  sub.synth_pal = 0xFF;
  sub.uw = 1.0F;
  sub.vh = 1.0F;
  std::uint8_t prio = 0;
  std::uint8_t slot = 0;
  std::uint8_t flip = 0;

  ayther::FrameView fv{};
  fv.fb_width = 320;
  fv.fb_height = 224;
  fv.fb_pixels = fb.data();
  fv.fb_pitch = 640;
  fv.fb_format = 2;
  fv.scene = &anchor;
  fv.scene_count = 1;
  fv.scene_vram = vram.data();
  fv.scene_vram_size = vram.size();
  fv.scene_cram = cram.data();
  fv.scene_cram_size = cram.size();
  fv.scene_dirty = ayther::session::kDirtyRaster;
  fv.sprite_subs = &sub;
  fv.sprite_sub_count = 1;
  fv.sprite_sub_prio = &prio;
  fv.sprite_sub_slot = &slot;
  fv.sprite_sub_flips = &flip;
  fv.frame_index = 7;

  const ayther::probe::RgbImage got = oracle.render(fv);
  const ayther::probe::RgbImage core =
      ayther::probe::core_image(fb.data(), 320, 224, 640, 2);
  const ayther::probe::O1Result r = ayther::probe::check_o1(got, core);
  std::printf("  differing=%zu first=(%d,%d)\n", r.differing_pixels, r.first_x,
              r.first_y);
  check(!got.rgb.empty() && r.identical,
        "RF-10.3: a raster frame shows the originals only (the core's image, "
        "no asset)");
  const ro::DrawReport report = oracle.renderer().last_draw_report();
  check(report.replacements.size() == 1 &&
            report.replacements[0].draw == ro::DrawOutcome::discarded,
        "RF-10.1: no replacement is drawn in a raster frame");

  AytherSpriteOccurrence occ{};
  occ.hash = 0xF1;
  occ.screen_x = 100;
  occ.screen_y = 80;
  occ.w_tiles = 2;
  occ.h_tiles = 2;
  const std::array<std::uint32_t, 1> owner{0};
  const std::array<std::uint8_t, 1> claimed{1};
  ayther::session::RenderObservationBuilder builder;
  ayther::session::RenderObservationInput in;
  in.emulation_frame = fv.frame_index;
  in.frame_known = true;
  in.composability = ro::Composability::raster_split;
  in.occurrences = {&occ, 1};
  in.claimed = claimed;
  in.subs = {&sub, 1};
  in.pose_sub_count = 1;
  in.pose_owner = owner;
  in.draw = &report;
  const ro::RenderFrameView &view = builder.build(in);
  const auto *reason = std::get_if<std::string_view>(
      &view.occurrences[0].not_applied_reason.value);
  check(view.composability == ro::Composability::raster_split &&
            view.occurrences[0].status ==
                ro::OccurrenceStatus::assigned_not_applied &&
            reason != nullptr && *reason == "frame_not_composable",
        "RF-7.9: observed, assigned_not_applied (frame_not_composable) with "
        "the reason raster_split");

  fs::remove_all(dir, ec);
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
