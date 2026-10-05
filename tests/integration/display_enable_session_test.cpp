// Spec 002 (A, Toma 3 frame 2): the display-enable bit (VDP register 1,
// bit 6). With the display off the VDP shows the backdrop only, so the frame
// is the core's image and no HD lane may be drawn over it, even when VRAM
// still holds the planes a Panorama or a Cuadro recognizes. A one-shot
// register write in mid-screen (the display turned off or on, a register
// that stays changed) leaves the lines drawn before it with the value the
// previous frame ended with: they are a raster band.
//
// The test core's display scenario (tools/test_core) keeps its planes stable
// and switches register 1 the way Golden Axe does at the start of Toma 3:
// off at line 100 of frame 20, on at line 60 of frame 30, and register 7
// written once at line 120 of frame 40.
#include <ayther/ayther_session.h>

#include "session/frame_composability.h"

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

// tools/test_core/ayther_test_core.cpp, display scenario.
constexpr char kTag[] = "AYTHER-DISPLAY-SCENARIO";
constexpr int kOffFrame = 20;
constexpr int kOnFrame = 30;
constexpr int kOneShotFrame = 40;
constexpr std::uint16_t kOnLine = 60;
constexpr std::uint16_t kOneShotLine = 120;

constexpr std::uint64_t kPanorama = 0xA1;
constexpr std::uint64_t kCuadro = 0xC1;
constexpr int kDefineAt = 10;

std::unique_ptr<ayther::AytherSession> open(const std::string &rom) {
  ayther::AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = rom;
  config.enable_audio = false;
  config.derive_core_pack = false;
  auto created = ayther::AytherSession::create(config);
  if (!created) {
    std::fprintf(stderr, "[FAIL] %s\n", created.error.message.c_str());
    return nullptr;
  }
  return std::move(*created);
}

constexpr std::uint8_t kNotComposed =
    ayther::session::kDirtyRaster | ayther::session::kDirtyDisplayOff;

void report(int frame, const ayther::FrameView &fv) {
  std::printf("  frame %d: dirty=%u reasons=%u bands=%u", frame,
              static_cast<unsigned>(fv.scene_dirty), fv.raster_reasons,
              static_cast<unsigned>(fv.raster_band_count));
  for (std::uint32_t k = 0; k < fv.raster_band_count && k < 16; ++k)
    std::printf(" [%u,%u)", fv.raster_bands[k][0], fv.raster_bands[k][1]);
  std::printf(" panorama=%d screen_subs=%u\n", fv.panorama_valid ? 1 : 0,
              fv.screen_sub_count);
}
} // namespace

int main() try {
  const auto rom_path =
      std::filesystem::temp_directory_path() / "ayther_display_enable.md";
  {
    std::vector<char> rom(0x10000, 0);
    std::copy(std::begin(kTag), std::end(kTag) - 1, rom.begin());
    std::ofstream out(rom_path, std::ios::binary);
    out.write(rom.data(), static_cast<std::streamsize>(rom.size()));
    if (!out)
      return 1;
  }
  auto s = open(rom_path.string());
  if (!s)
    return 1;

  const ayther::FrameView *fv = nullptr;
  int frame = 0;
  const auto step_to = [&](int target) {
    for (; frame <= target; ++frame)
      fv = &s->step();
  };

  // A Panorama and a Cuadro (with an asset) of plane A, from the frame.
  step_to(kDefineAt);
  std::vector<ayther::AytherSession::PanoramaCell> strip;
  std::vector<ayther::AytherSession::ScreenCell> cells;
  for (std::uint32_t i = 0; fv && fv->plane_cells && i < fv->plane_cell_count;
       ++i) {
    const ayther::PlaneCellHit &c = fv->plane_cells[i];
    if (c.plane != 0 || c.screen_x < 0 || c.screen_y < 0 || c.hash == 0)
      continue;
    strip.push_back({c.hash, c.screen_x / 8, c.screen_y / 8});
    cells.push_back({c.hash, 0, static_cast<std::uint8_t>(c.screen_x / 8),
                     static_cast<std::uint8_t>(c.screen_y / 8)});
  }
  std::printf("  plane A: %zu cells at frame %d\n", strip.size(), kDefineAt);
  check(strip.size() >= 64, "the scenario's plane A has cells to recognize");
  s->define_panorama(kPanorama, 0, 0, 0, 32, 24, strip.data(),
                     static_cast<std::uint32_t>(strip.size()),
                     "graphics/panorama.png");
  s->define_screen(kCuadro, 0x01, cells.data(),
                   static_cast<std::uint32_t>(cells.size()), 0.9F, 0.5F,
                   "graphics/cuadro.png");

  // Control: with the display on, the planes are recognized.
  step_to(kOffFrame - 1);
  report(kOffFrame - 1, *fv);
  check(fv->scene_dirty == 0 && fv->panorama_valid,
        "control: with the display on the frame composes and the Panorama "
        "anchors");
  check(fv->screen_sub_count > 0,
        "control: with the display on the Cuadro is drawn");

  // The display is turned off at line 100 and stays off: the frame is the
  // core's image, and nothing is recognized over it.
  step_to(kOffFrame);
  report(kOffFrame, *fv);
  check((fv->scene_dirty & kNotComposed) != 0,
        "A: a frame that turns the display off in mid-screen is not "
        "composed (the core's image)");
  check(!fv->panorama_valid && fv->panorama_sub_count == 0,
        "A: no Panorama over a display turned off");
  check(fv->screen_sub_count == 0, "A: no Cuadro over a display turned off");

  // Off for the whole frame, without any raster write.
  step_to(kOffFrame + 5);
  report(kOffFrame + 5, *fv);
  check(fv->raster_reasons == 0 &&
            (fv->scene_dirty & ayther::session::kDirtyDisplayOff) != 0,
        "A: a frame with the display off is not composed");
  check(!fv->panorama_valid && fv->screen_sub_count == 0 &&
            fv->plane_tile_sub_count == 0,
        "A: no HD lane while the display is off");

  // Turned on at line 60: the lines above show the backdrop only.
  step_to(kOnFrame);
  report(kOnFrame, *fv);
  check((fv->scene_dirty & kNotComposed) == 0 && fv->raster_band_count == 1 &&
            fv->raster_bands[0][0] == 0 && fv->raster_bands[0][1] == kOnLine,
        "A: the lines drawn before the display turns on are a band");

  // Register 7 written once at line 120: the lines above were drawn with
  // the previous value.
  step_to(kOneShotFrame);
  report(kOneShotFrame, *fv);
  check((fv->scene_dirty & kNotComposed) == 0 && fv->raster_band_count == 1 &&
            fv->raster_bands[0][0] == 0 &&
            fv->raster_bands[0][1] == kOneShotLine,
        "A: a one-shot register write bands the lines drawn before it");
  step_to(kOneShotFrame + 1);
  check(fv->scene_dirty == 0 && fv->panorama_valid,
        "after the write the frame composes again");

  s.reset();
  std::error_code ignored;
  std::filesystem::remove(rom_path, ignored);
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
