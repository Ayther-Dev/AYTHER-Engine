// Spec 002, DI-17 (RF-10.1, RF-10.3, RF-7.9), on a GPU: a frame with raster
// writes in mid-screen loses HD only in the bands of lines those writes
// touch. There the core's image is shown — the originals, never an original
// and its replacement together; elsewhere the frame is composed with its
// replacements. A replacement whole inside a band is reported discarded
// (assigned_not_applied, frame_not_composable); one outside is drawn.
#include "gpu_oracle.h"
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

ayther::SceneElement sprite(std::int16_t y, std::uint8_t slot,
                            std::int32_t sub) {
  ayther::SceneElement e{};
  e.hash = 0xF1U + slot;
  e.x = 100;
  e.y = y;
  e.w = 16;
  e.h = 16;
  e.pattern = 1;
  e.layer = 3;
  e.slot = slot;
  e.chain = slot;
  e.claimed = 1;
  e.owner = static_cast<std::int16_t>(sub);
  e.sub_kind = 1;
  e.sub = sub;
  return e;
}

AytherSpriteSub replacement(const std::string &png, std::int16_t y) {
  AytherSpriteSub sub{};
  std::snprintf(sub.asset_path, sizeof(sub.asset_path), "%s", png.c_str());
  sub.screen_x = 100;
  sub.screen_y = y;
  sub.w_tiles = 2;
  sub.h_tiles = 2;
  sub.w_px = 16;
  sub.h_px = 16;
  sub.palette = 0xFF;
  sub.synth_pal = 0xFF;
  sub.uw = 1.0F;
  sub.vh = 1.0F;
  return sub;
}

bool rows_equal(const ayther::probe::RgbImage &a,
                const ayther::probe::RgbImage &b, int y0, int y1) {
  for (int y = y0; y < y1; ++y)
    for (int x = 0; x < static_cast<int>(a.width); ++x)
      for (int c = 0; c < 3; ++c) {
        const std::size_t i =
            (static_cast<std::size_t>(y) * a.width + x) * 3 + c;
        if (a.rgb[i] != b.rgb[i])
          return false;
      }
  return true;
}

bool green_at(const ayther::probe::RgbImage &img, int x, int y) {
  const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 3;
  return img.rgb[i] < 40 && img.rgb[i + 1] > 200 && img.rgb[i + 2] < 40;
}
} // namespace

int main() try {
  std::printf("=== raster_band_test (spec 002 DI-17) ===\n");
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path() / "ayther_raster_band";
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
  // The core's image: two red sprites over black, one in the band [0, 112)
  // the raster writes touch, one below it.
  std::vector<std::uint16_t> fb(320U * 224U, 0);
  for (const int top : {40, 160})
    for (int y = top; y < top + 16; ++y)
      for (int x = 100; x < 116; ++x)
        fb[static_cast<std::size_t>(y) * 320 + x] = 0xF800;
  std::vector<std::uint8_t> vram(0x10000, 0);
  for (int i = 0; i < 4 * 32; ++i)
    vram[32 + i] = 0x11;
  std::vector<std::uint8_t> cram(128, 0);
  cram[2] = 0x07;

  const std::array<ayther::SceneElement, 2> scene{sprite(40, 0, 0),
                                                  sprite(160, 1, 1)};
  const std::array<AytherSpriteSub, 2> subs{replacement(green_png, 40),
                                            replacement(green_png, 160)};
  std::array<std::uint8_t, 2> prio{0, 0};
  std::array<std::uint8_t, 2> slot{0, 1};
  std::array<std::uint8_t, 2> flip{0, 0};

  ayther::FrameView fv{};
  fv.fb_width = 320;
  fv.fb_height = 224;
  fv.fb_pixels = fb.data();
  fv.fb_pitch = 640;
  fv.fb_format = 2;
  fv.scene = scene.data();
  fv.scene_count = 2;
  fv.scene_vram = vram.data();
  fv.scene_vram_size = vram.size();
  fv.scene_cram = cram.data();
  fv.scene_cram_size = cram.size();
  fv.scene_dirty = 0; // the session localized the raster writes
  fv.raster_reasons = 1U << 6;
  fv.raster_band_count = 1;
  fv.raster_bands[0][0] = 0;
  fv.raster_bands[0][1] = 112;
  fv.sprite_subs = subs.data();
  fv.sprite_sub_count = 2;
  fv.sprite_sub_prio = prio.data();
  fv.sprite_sub_slot = slot.data();
  fv.sprite_sub_flips = flip.data();
  fv.frame_index = 9;

  const ayther::probe::RgbImage got = oracle.render(fv);
  const ayther::probe::RgbImage core =
      ayther::probe::core_image(fb.data(), 320, 224, 640, 2);
  check(!got.rgb.empty() && rows_equal(got, core, 0, 112),
        "RF-10.1: in the band the frame is the core's image (the original, "
        "not its replacement)");
  check(!got.rgb.empty() && green_at(got, 108, 168),
        "DI-17: below the band the replacement is drawn");
  check(!got.rgb.empty() && !rows_equal(got, core, 112, 224),
        "DI-17: below the band the frame is composed, not the core's image");

  const ro::DrawReport report = oracle.renderer().last_draw_report();
  check(report.replacements.size() == 2 &&
            report.replacements[0].draw == ro::DrawOutcome::discarded &&
            report.replacements[1].draw != ro::DrawOutcome::discarded,
        "the replacement inside the band is discarded, the other is drawn");

  std::array<AytherSpriteOccurrence, 2> occ{};
  for (int i = 0; i < 2; ++i) {
    occ[i].hash = 0xF1U + i;
    occ[i].screen_x = 100;
    occ[i].screen_y = i == 0 ? 40 : 160;
    occ[i].w_tiles = 2;
    occ[i].h_tiles = 2;
    occ[i].slot = static_cast<std::uint8_t>(i);
  }
  const std::array<std::uint32_t, 2> owner{0, 1};
  const std::array<std::uint8_t, 2> claimed{1, 1};
  ayther::session::RenderObservationBuilder builder;
  ayther::session::RenderObservationInput in;
  in.emulation_frame = fv.frame_index;
  in.frame_known = true;
  in.composability = ro::Composability::raster_split;
  in.occurrences = occ;
  in.claimed = claimed;
  in.subs = subs;
  in.pose_sub_count = 2;
  in.pose_owner = owner;
  in.draw = &report;
  const ro::RenderFrameView &view = builder.build(in);
  const auto *reason = view.occurrences.size() == 2
                           ? std::get_if<std::string_view>(
                                 &view.occurrences[0].not_applied_reason.value)
                           : nullptr;
  check(view.occurrences.size() == 2 &&
            view.occurrences[0].status ==
                ro::OccurrenceStatus::assigned_not_applied &&
            reason != nullptr && *reason == "frame_not_composable" &&
            view.occurrences[1].status == ro::OccurrenceStatus::replaced,
        "RF-7.9: observed per replacement — inside the band not applied "
        "(frame_not_composable), outside replaced");

  // A replacement that straddles a band's edge: drawing its HD outside the
  // band and its original inside would show the element as both at once.
  // It is not applied: its original is drawn whole, in and out of the band.
  {
    std::vector<std::uint16_t> fb2(320U * 224U, 0);
    for (int y = 104; y < 120; ++y)
      for (int x = 200; x < 216; ++x)
        fb2[static_cast<std::size_t>(y) * 320 + x] = 0xF800;
    ayther::SceneElement edge = sprite(104, 0, 0);
    edge.x = 200;
    AytherSpriteSub edge_sub = replacement(green_png, 104);
    edge_sub.screen_x = 200;
    std::uint8_t p2 = 0;
    std::uint8_t s2 = 0;
    std::uint8_t f2 = 0;
    ayther::FrameView fv2 = fv;
    fv2.fb_pixels = fb2.data();
    fv2.scene = &edge;
    fv2.scene_count = 1;
    fv2.sprite_subs = &edge_sub;
    fv2.sprite_sub_count = 1;
    fv2.sprite_sub_prio = &p2;
    fv2.sprite_sub_slot = &s2;
    fv2.sprite_sub_flips = &f2;
    const ayther::probe::RgbImage got2 = oracle.render(fv2);
    const ayther::probe::RgbImage core2 =
        ayther::probe::core_image(fb2.data(), 320, 224, 640, 2);
    const ro::DrawReport report2 = oracle.renderer().last_draw_report();
    const auto red_at = [&](int x, int y) {
      const std::size_t i = (static_cast<std::size_t>(y) * got2.width + x) * 3;
      return got2.rgb[i] > 150 && got2.rgb[i + 1] < 40 && got2.rgb[i + 2] < 40;
    };
    check(!got2.rgb.empty() && rows_equal(got2, core2, 0, 112) &&
              red_at(208, 116) && !green_at(got2, 208, 116),
          "RF-10.1: a replacement across a band's edge is not drawn; its "
          "original shows whole");
    check(report2.replacements.size() == 1 &&
              report2.replacements[0].draw == ro::DrawOutcome::discarded,
          "the replacement across the band's edge is discarded");
  }

  fs::remove_all(dir, ec);
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
