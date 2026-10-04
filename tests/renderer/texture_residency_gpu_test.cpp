// Spec 002, BR-089 (RF-9.1, RF-9.2), on a GPU.
//
// A replacement hides the originals it claims only while its texture is
// resident (session/texture_residency.h). Synthetic scene, no ROM or pack: a
// two-sprite pose whose replacement is a loose PNG (pending on its first
// frames, until its asynchronous decode lands), its anchor and its member
// both claimed. On every frame each sprite is on screen — as its original
// (red) while the texture is pending, as the replacement (green) once it is
// ready — so no frame loses the element. A replacement whose asset is
// missing keeps its original for good.
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

struct Spot {
  std::int16_t x;
  std::int16_t y;
};
// Pose 0: anchor at kAnchor and member at kMember, drawn by one 32x16 PNG.
// Pose 1: one sprite at kLost whose asset does not exist.
constexpr Spot kAnchor{60, 100};
constexpr Spot kMember{76, 100};
constexpr Spot kLost{200, 100};

AytherSpriteSub make_sub(const std::string &asset, Spot at, int w) {
  AytherSpriteSub s{};
  std::snprintf(s.asset_path, sizeof(s.asset_path), "%s", asset.c_str());
  s.screen_x = at.x;
  s.screen_y = at.y;
  s.w_tiles = static_cast<std::uint8_t>(w / 8);
  s.h_tiles = kSprite / 8;
  s.w_px = static_cast<std::uint16_t>(w);
  s.h_px = kSprite;
  s.palette = 0xFF;
  s.synth_pal = 0xFF;
  s.uw = 1.0F;
  s.vh = 1.0F;
  return s;
}

// A sprite claimed by replacement `owner`; `anchor` = it draws that
// replacement in the scene pass.
ayther::SceneElement claimed_sprite(Spot at, std::uint8_t slot,
                                    std::int16_t owner, bool anchor) {
  ayther::SceneElement e{};
  e.hash = 0xB0U + slot;
  e.x = at.x;
  e.y = at.y;
  e.w = kSprite;
  e.h = kSprite;
  e.pattern = 1;
  e.layer = 3;
  e.slot = slot;
  e.chain = slot;
  e.claimed = 1;
  e.owner = owner;
  e.sub_kind = anchor ? 1 : 0;
  e.sub = anchor ? owner : -1;
  return e;
}

enum class Seen { red, green, black };

Seen seen_at(const std::uint8_t *bgra, Spot at) {
  const std::size_t i =
      ((static_cast<std::size_t>(at.y) + 8) * kW + at.x + 8) * 4;
  const std::uint8_t b = bgra[i];
  const std::uint8_t g = bgra[i + 1];
  const std::uint8_t r = bgra[i + 2];
  if (g > 200 && r < 60 && b < 60)
    return Seen::green;
  if (r > 200 && g < 60 && b < 60)
    return Seen::red;
  return Seen::black;
}

} // namespace

int main() try {
  std::printf("=== texture_residency_gpu_test (spec 002 BR-089) ===\n");
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path() / "ayther_texture_residency";
  fs::create_directories(dir, ec);
  const std::string green_png = (dir / "pose.png").string();
  const std::string missing_png = (dir / "missing.png").string();
  fs::remove(missing_png, ec);
  {
    const int w = kSprite * 2;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * kSprite * 4);
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
      rgba[i + 1] = 255;
      rgba[i + 3] = 255;
    }
    if (stbi_write_png(green_png.c_str(), w, kSprite, 4, rgba.data(), w * 4) ==
        0) {
      std::fprintf(stderr, "[FAIL] cannot write %s\n", green_png.c_str());
      return 1;
    }
  }

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "[FAIL] SDL_Init\n");
    return 1;
  }
  SDL_Window *window = SDL_CreateWindow("texture_residency_gpu_test", 64, 64,
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
  // BR-092: this test is about the asynchronous mode, where a texture is
  // pending for a few frames; the synchronous default never shows it.
  renderer.set_synchronous_textures(false);
  const std::string shaders = std::string(AYTHER_SOURCE_DIR) + "/shaders/";
  if (!renderer.init(ctx, kW, kH, shaders.c_str()) ||
      !renderer.readback_init(ctx)) {
    std::fprintf(stderr, "[FAIL] renderer (shaders in %s)\n", shaders.c_str());
    return 1;
  }

  // Patterns 1..4 (the four tiles of a 16x16 sprite) = colour index 1 =
  // red; the backdrop stays black.
  std::vector<std::uint8_t> vram(0x10000, 0);
  for (int i = 0; i < 4 * 32; ++i)
    vram[32 + i] = 0x11;
  std::vector<std::uint8_t> cram(128, 0);
  cram[2] = 7;
  std::vector<std::uint16_t> fb(static_cast<std::size_t>(kW) * kH, 0);

  const std::array subs{make_sub(green_png, kAnchor, kSprite * 2),
                        make_sub(missing_png, kLost, kSprite)};
  std::array<std::uint8_t, 2> prio{};
  std::array<std::uint8_t, 2> slot{0, 2};
  // Back to front: the member (chain 1) before the anchor (chain 0).
  const std::array scene{claimed_sprite(kMember, 1, 0, false),
                         claimed_sprite(kAnchor, 0, 0, true),
                         claimed_sprite(kLost, 2, 1, true)};

  AytherLayerStack stack;
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
  fv.sprite_subs = subs.data();
  fv.sprite_sub_count = static_cast<std::uint32_t>(subs.size());
  fv.sprite_sub_prio = prio.data();
  fv.sprite_sub_slot = slot.data();
  fv.frame_index = 100;

  // 1. First frame: the texture is pending, the originals are on screen.
  const std::uint8_t *pixels =
      renderer.export_frame(ctx, fv, nullptr, true, &stack);
  check(pixels != nullptr, "the synthetic frame renders");
  if (pixels == nullptr)
    return 1;
  ro::DrawReport report = renderer.last_draw_report();
  check(report.replacements.size() == 2 &&
            report.replacements[0].texture == ro::TextureState::pending,
        "the pose texture is pending on its first frame");
  check(seen_at(pixels, kAnchor) == Seen::red &&
            seen_at(pixels, kMember) == Seen::red,
        "RF-9.1: with the texture pending, the anchor and the member keep "
        "their originals");

  // 2. Every frame until the decode lands: never a black hole.
  bool ready = false;
  bool never_lost = true;
  int frames = 1;
  for (int attempt = 0; attempt < 600 && !ready; ++attempt) {
    ++fv.frame_index;
    pixels = renderer.export_frame(ctx, fv, nullptr, true, &stack);
    if (pixels == nullptr)
      break;
    ++frames;
    report = renderer.last_draw_report();
    ready = report.replacements.size() == 2 &&
            report.replacements[0].texture == ro::TextureState::ready;
    for (const Spot at : {kAnchor, kMember, kLost})
      never_lost = never_lost && seen_at(pixels, at) != Seen::black;
    if (!ready)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::printf("  %d frame(s) until the texture was resident\n", frames);
  check(ready, "the texture becomes resident once its decode lands");
  check(never_lost, "RF-9.1: no frame loses the anchor, the member or the "
                    "sprite whose asset is missing");
  if (!ready || pixels == nullptr)
    return 1;

  // 3. Resident: the replacement hides both originals.
  check(seen_at(pixels, kAnchor) == Seen::green &&
            seen_at(pixels, kMember) == Seen::green,
        "RF-9.2: with the texture resident, the replacement covers the "
        "anchor and the member");
  check(report.replacements[1].texture == ro::TextureState::failed &&
            seen_at(pixels, kLost) == Seen::red,
        "RF-9.1: a replacement whose asset is missing keeps its original");

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
