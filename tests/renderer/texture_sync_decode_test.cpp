// Spec 002, BR-092 (RF-10.2, RNF-1), on a GPU.
//
// Deterministic residency: a frame that needs a texture that is not resident
// waits for its decode (the renderer decodes it synchronously before
// composing), so the replacement is on screen on its very first frame and
// two renderers draw that frame identically. A missing asset is failed and
// keeps its original. Same synthetic scene as texture_residency_gpu_test.
#include <ayther/ayther_layers.h>
#include <ayther/ayther_renderer.h>
#include <ayther/ayther_session.h>

#include "vulkan_test_context.h"

#include <SDL3/SDL.h>
#include <stb_image_write.h>

#include <algorithm>
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
  std::printf("=== texture_sync_decode_test (spec 002 BR-092) ===\n");
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path() / "ayther_texture_sync_decode";
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
  SDL_Window *window = SDL_CreateWindow("texture_sync_decode_test", 64, 64,
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

  // 1. The first frame that needs the texture: it is decoded before the
  //    frame is composed.
  const std::uint8_t *pixels =
      renderer.export_frame(ctx, fv, nullptr, true, &stack);
  check(pixels != nullptr, "the synthetic frame renders");
  if (pixels == nullptr)
    return 1;
  const std::vector<std::uint8_t> first(
      pixels, pixels + static_cast<std::size_t>(kW) * kH * 4);
  const ro::DrawReport report = renderer.last_draw_report();
  check(report.replacements.size() == 2 &&
            report.replacements[0].texture == ro::TextureState::ready,
        "RF-10.2: the frame waits for the decode: the texture is resident on "
        "its first frame");
  check(seen_at(pixels, kAnchor) == Seen::green &&
            seen_at(pixels, kMember) == Seen::green,
        "RNF-1: the replacement is on screen on its first frame");
  check(report.replacements.size() == 2 &&
            report.replacements[1].texture == ro::TextureState::failed &&
            seen_at(pixels, kLost) == Seen::red,
        "RF-10.2: a missing asset is failed at once and keeps its original");

  // 2. A second renderer, cold, draws the same first frame identically.
  {
    ayther::AytherRenderer other;
    if (!other.init(ctx, kW, kH, shaders.c_str()) ||
        !other.readback_init(ctx)) {
      std::fprintf(stderr, "[FAIL] second renderer\n");
      return 1;
    }
    const std::uint8_t *again =
        other.export_frame(ctx, fv, nullptr, true, &stack);
    check(again != nullptr && std::equal(first.begin(), first.end(), again),
          "RNF-1: two cold renderers compose the first frame identically");
    vkDeviceWaitIdle(ctx.device());
    other.readback_shutdown(ctx);
    other.shutdown(ctx);
  }

  // 2b. The other textured replacements of a frame — a panorama strip, a
  //     screen (Picture) and an entity — are resident on their first frame
  //     too (frame 4788 of Golden Axe lost its whole HD background, a
  //     panorama, while its texture decoded).
  {
    const std::string pano_png = (dir / "pano.png").string();
    const std::string screen_png = (dir / "screen.png").string();
    const std::string entity_png = (dir / "entity.png").string();
    std::vector<std::uint8_t> px(16 * 16 * 4, 255);
    for (const std::string &path : {pano_png, screen_png, entity_png})
      if (stbi_write_png(path.c_str(), 16, 16, 4, px.data(), 16 * 4) == 0)
        return 1;
    const std::array extra{make_sub(pano_png, kLost, kSprite),
                           make_sub(screen_png, kLost, kSprite),
                           make_sub(entity_png, kLost, kSprite)};
    ayther::FrameView more = fv;
    more.panorama_subs = &extra[0];
    more.panorama_sub_count = 1;
    more.screen_subs = &extra[1];
    more.screen_sub_count = 1;
    more.entity_subs = &extra[2];
    more.entity_sub_count = 1;
    (void)renderer.export_frame(ctx, more, nullptr, true, &stack);
    using TS = ayther::AytherRenderer::TextureState;
    check(renderer.sprite_texture_state(pano_png) == TS::ready &&
              renderer.sprite_texture_state(screen_png) == TS::ready &&
              renderer.sprite_texture_state(entity_png) == TS::ready,
          "RNF-1: panorama, screen and entity textures are resident on their "
          "first frame");
  }

  // 3. The asynchronous mode (authoring) still exists: pending first.
  {
    ayther::AytherRenderer async;
    async.set_synchronous_textures(false);
    if (!async.init(ctx, kW, kH, shaders.c_str()) ||
        !async.readback_init(ctx)) {
      std::fprintf(stderr, "[FAIL] asynchronous renderer\n");
      return 1;
    }
    (void)async.export_frame(ctx, fv, nullptr, true, &stack);
    const ro::DrawReport cold = async.last_draw_report();
    check(cold.replacements.size() == 2 &&
              cold.replacements[0].texture == ro::TextureState::pending,
          "with synchronous textures off, the first frame is pending");
    vkDeviceWaitIdle(ctx.device());
    async.readback_shutdown(ctx);
    async.shutdown(ctx);
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
