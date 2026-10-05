// Spec 002, DI-18 (F-1), on a GPU: the renderer reports every layer of its
// stack in the draw report (render observation contract 1.1), and for an
// overlay (a pack Acetato) whether its gate was open and its sheet drawn. An
// overlay gated on a Cuadro draws while the Cuadro is present and not
// otherwise.
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
namespace ro = ayther::engine::render_observation;

constexpr std::uint64_t kCuadro = 0x4451C3DCE705076DULL;

const ro::LayerView *overlay_row(const ro::DrawReport &report) {
  for (const ro::LayerView &layer : report.layers)
    if (layer.overlay)
      return &layer;
  return nullptr;
}

bool white_at(const ayther::probe::RgbImage &img, int x, int y) {
  const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 3;
  return img.rgb[i] > 200 && img.rgb[i + 1] > 200 && img.rgb[i + 2] > 200;
}
} // namespace

int main() try {
  std::printf("=== overlay_layer_report_test (spec 002 DI-18) ===\n");
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path() / "ayther_overlay_report";
  fs::create_directories(dir, ec);
  const std::string white_png = (dir / "white.png").string();
  const std::uint8_t white[3] = {255, 255, 255};
  if (!ayther::test::write_solid_png(white_png, 16, 16, white))
    return 1;

  ayther::test::GpuOracle oracle;
  if (!oracle.init(320, 224)) {
    std::fprintf(stderr, "[FAIL] GPU oracle init\n");
    return 1;
  }
  // The overlay: a white sheet tiled over the frame, at stack position 1,
  // gated by presence on the Cuadro.
  AytherLayerStack &stack = oracle.stack();
  const std::uint32_t id = stack.insert_custom("Nubes", 1);
  AytherLayerContent content{};
  std::snprintf(content.asset, sizeof(content.asset), "%s", white_png.c_str());
  content.img_w = 16;
  content.img_h = 16;
  content.tile_mode = 1;
  content.opacity = 1.0F;
  content.gate_presence = 1;
  (void)content.add_screen(kCuadro);
  check(id != 0 && stack.set_content(id, content), "the overlay is stacked");

  std::vector<std::uint16_t> fb(320U * 224U, 0);
  ayther::FrameView fv{};
  fv.fb_width = 320;
  fv.fb_height = 224;
  fv.fb_pixels = fb.data();
  fv.fb_pitch = 640;
  fv.fb_format = 2;
  fv.frame_index = 3;

  // The Cuadro is present: the gate opens and the sheet is drawn. The first
  // render loads the sheet; the second draws it.
  fv.screen_presence_ids[0] = kCuadro;
  fv.screen_presence_count = 1;
  (void)oracle.render(fv);
  ayther::probe::RgbImage open = oracle.render(fv);
  ro::DrawReport report = oracle.renderer().last_draw_report();
  const ro::LayerView *row = overlay_row(report);
  std::printf("  layers=%zu\n", report.layers.size());
  check(report.layers.size() == stack.layers().size(),
        "the draw report lists every layer of the stack");
  check(row != nullptr && row->name == "Nubes" && row->stack_index == 1 &&
            row->gated && row->gate_open && row->drawn,
        "DI-18: with its Cuadro present the overlay's gate is open and it is "
        "drawn");
  check(!open.rgb.empty() && white_at(open, 160, 112),
        "the overlay's sheet is on screen");

  // The Cuadro is gone: the gate closes.
  fv.screen_presence_count = 0;
  fv.screen_presence_ids[0] = 0;
  const ayther::probe::RgbImage closed = oracle.render(fv);
  report = oracle.renderer().last_draw_report();
  row = overlay_row(report);
  check(row != nullptr && row->gated && !row->gate_open && !row->drawn,
        "DI-18: without its Cuadro the gate is closed and nothing is drawn");
  check(!closed.rgb.empty() && !white_at(closed, 160, 112),
        "the overlay's sheet is not on screen");

  fs::remove_all(dir, ec);
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
