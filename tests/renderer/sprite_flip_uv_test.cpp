// Spec 002, BR-090 (RF-9.2), on a GPU.
//
// A flipped replacement is drawn by flipping its texture coordinates, not by
// decoding a flipped copy of the asset: once the asset is resident, the
// same replacement drawn with any flip is on screen in that very frame, with
// no new decode. Its pixels match the flip done on the CPU. Synthetic scene,
// no ROM or pack: a 16x16 asset with four coloured quadrants.
#include <ayther/ayther_layers.h>
#include <ayther/ayther_renderer.h>
#include <ayther/ayther_session.h>

#include "vulkan_test_context.h"

#include <SDL3/SDL.h>
#include <stb_image_write.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace ro = ayther::engine::render_observation;

int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

constexpr std::uint32_t kW = 320;
constexpr std::uint32_t kH = 224;
constexpr int kSprite = 16;
constexpr std::int16_t kX = 100;
constexpr std::int16_t kY = 80;

struct Rgb {
  std::uint8_t r, g, b;
};
// Quadrants of the asset: top-left, top-right, bottom-left, bottom-right.
constexpr std::array<Rgb, 4> kQuad{
    {{0, 255, 0}, {0, 0, 255}, {255, 255, 255}, {255, 255, 0}}};

/// The asset pixel at (x, y) after `flip` (bit0 h, bit1 v), on the CPU.
Rgb cpu_flipped(int x, int y, std::uint8_t flip) {
  if ((flip & 1U) != 0)
    x = kSprite - 1 - x;
  if ((flip & 2U) != 0)
    y = kSprite - 1 - y;
  return kQuad[(y >= kSprite / 2 ? 2 : 0) + (x >= kSprite / 2 ? 1 : 0)];
}

Rgb screen_at(const std::uint8_t *bgra, int x, int y) {
  const std::size_t i = (static_cast<std::size_t>(kY + y) * kW +
                         static_cast<std::size_t>(kX + x)) *
                        4;
  return {bgra[i + 2], bgra[i + 1], bgra[i]};
}

/// Every pixel of the replacement equals the CPU flip of the asset, within
/// the rounding of the blend (±8 per channel; the quadrant colours differ by
/// 255 in at least one channel).
bool matches_cpu_flip(const std::uint8_t *bgra, std::uint8_t flip) {
  const auto near = [](std::uint8_t a, std::uint8_t b) {
    return (a > b ? a - b : b - a) <= 8;
  };
  for (int y = 0; y < kSprite; ++y)
    for (int x = 0; x < kSprite; ++x) {
      const Rgb got = screen_at(bgra, x, y);
      const Rgb want = cpu_flipped(x, y, flip);
      if (!near(got.r, want.r) || !near(got.g, want.g) || !near(got.b, want.b))
        return false;
    }
  return true;
}

} // namespace

int main() try {
  std::printf("=== sprite_flip_uv_test (spec 002 BR-090) ===\n");
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path() / "ayther_sprite_flip_uv";
  fs::create_directories(dir, ec);
  const std::string png = (dir / "quadrants.png").string();
  {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kSprite) * kSprite *
                                   4);
    for (int y = 0; y < kSprite; ++y)
      for (int x = 0; x < kSprite; ++x) {
        const Rgb c = cpu_flipped(x, y, 0);
        const std::size_t i = (static_cast<std::size_t>(y) * kSprite + x) * 4;
        rgba[i] = c.r;
        rgba[i + 1] = c.g;
        rgba[i + 2] = c.b;
        rgba[i + 3] = 255;
      }
    if (stbi_write_png(png.c_str(), kSprite, kSprite, 4, rgba.data(),
                       kSprite * 4) == 0) {
      std::fprintf(stderr, "[FAIL] cannot write %s\n", png.c_str());
      return 1;
    }
  }

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "[FAIL] SDL_Init\n");
    return 1;
  }
  SDL_Window *window = SDL_CreateWindow("sprite_flip_uv_test", 64, 64,
                                        SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
  if (window == nullptr) {
    std::fprintf(stderr, "[FAIL] SDL_CreateWindow\n");
    return 1;
  }
  VulkanTestContext ctx;
  if (!ctx.init(window)) {
    std::fprintf(stderr, "[FAIL] VulkanTestContext::init\n");
    return 1;
  }
  ayther::AytherRenderer renderer;
  const std::string shaders = std::string(AYTHER_SOURCE_DIR) + "/shaders/";
  if (!renderer.init(ctx, kW, kH, shaders.c_str()) ||
      !renderer.readback_init(ctx)) {
    std::fprintf(stderr, "[FAIL] renderer (shaders in %s)\n", shaders.c_str());
    return 1;
  }

  std::vector<std::uint8_t> vram(0x10000, 0);
  std::vector<std::uint8_t> cram(128, 0);
  std::vector<std::uint16_t> fb(static_cast<std::size_t>(kW) * kH, 0);

  AytherSpriteSub sub{};
  std::snprintf(sub.asset_path, sizeof(sub.asset_path), "%s", png.c_str());
  sub.screen_x = kX;
  sub.screen_y = kY;
  sub.w_tiles = kSprite / 8;
  sub.h_tiles = kSprite / 8;
  sub.w_px = kSprite;
  sub.h_px = kSprite;
  sub.palette = 0xFF;
  sub.synth_pal = 0xFF;
  sub.uw = 1.0F;
  sub.vh = 1.0F;
  ayther::SceneElement anchor{};
  anchor.hash = 0xC0;
  anchor.x = kX;
  anchor.y = kY;
  anchor.w = kSprite;
  anchor.h = kSprite;
  anchor.pattern = 1;
  anchor.layer = 3;
  anchor.slot = 0;
  anchor.chain = 0;
  anchor.claimed = 1;
  anchor.owner = 0;
  anchor.sub_kind = 1;
  anchor.sub = 0;
  std::uint8_t flip = 0;
  std::uint8_t prio = 0;
  std::uint8_t slot = 0;

  AytherLayerStack stack;
  ayther::FrameView fv{};
  fv.fb_width = kW;
  fv.fb_height = kH;
  fv.fb_pixels = fb.data();
  fv.fb_pitch = kW * 2;
  fv.fb_format = 2;
  fv.scene = &anchor;
  fv.scene_count = 1;
  fv.scene_vram = vram.data();
  fv.scene_vram_size = vram.size();
  fv.scene_cram = cram.data();
  fv.scene_cram_size = cram.size();
  fv.sprite_subs = &sub;
  fv.sprite_sub_count = 1;
  fv.sprite_sub_prio = &prio;
  fv.sprite_sub_slot = &slot;
  fv.sprite_sub_flips = &flip;
  fv.frame_index = 10;

  // 1. Unflipped, until the asset is resident.
  const std::uint8_t *pixels = nullptr;
  bool ready = false;
  for (int attempt = 0; attempt < 600 && !ready; ++attempt) {
    ++fv.frame_index;
    pixels = renderer.export_frame(ctx, fv, nullptr, true, &stack);
    if (pixels == nullptr)
      break;
    const ro::DrawReport report = renderer.last_draw_report();
    ready = report.replacements.size() == 1 &&
            report.replacements[0].texture == ro::TextureState::ready;
    if (!ready)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(ready && pixels != nullptr, "the asset becomes resident");
  if (!ready || pixels == nullptr)
    return 1;
  check(matches_cpu_flip(pixels, 0), "unflipped, the asset is drawn as is");

  // 2. Each flip, on the very next frame: no new decode, same texture.
  for (const std::uint8_t f :
       {std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{3}, std::uint8_t{0}}) {
    flip = f;
    ++fv.frame_index;
    pixels = renderer.export_frame(ctx, fv, nullptr, true, &stack);
    const ro::DrawReport report = renderer.last_draw_report();
    const bool resident =
        pixels != nullptr && report.replacements.size() == 1 &&
        report.replacements[0].texture == ro::TextureState::ready;
    char message[160];
    std::snprintf(message, sizeof(message),
                  "RF-9.2: flip %u reuses the resident texture in the same "
                  "frame",
                  f);
    check(resident, message);
    std::snprintf(message, sizeof(message),
                  "RF-9.2: flip %u matches the flip done on the CPU, pixel by "
                  "pixel",
                  f);
    check(resident && matches_cpu_flip(pixels, f), message);
  }

  vkDeviceWaitIdle(ctx.device());
  renderer.readback_shutdown(ctx);
  renderer.shutdown(ctx);
  ctx.shutdown();
  SDL_DestroyWindow(window);
  SDL_Quit();
  fs::remove_all(dir, ec);

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
