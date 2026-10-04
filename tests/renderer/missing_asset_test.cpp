// Spec 002, BR-093 (RF-2.11, RF-9.3), on a GPU.
//
// An assigned asset that cannot be read: a pose whose PNG is corrupt. The
// preparation reports it as a missing asset; the frame keeps the originals
// with the replacement's texture failed; the observation says why
// (assigned_not_applied, texture_failed); and the probe's replay QA rule
// stops on it (probe_missing.h).
#include <ayther/ayther_layers.h>
#include <ayther/ayther_renderer.h>
#include <ayther/ayther_session.h>

#include "../../tools/render_probe/probe_missing.h"
#include "session/render_observation_builder.h"
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
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
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

AytherSpriteOccurrence make_occurrence(Spot at, std::uint8_t slot) {
  AytherSpriteOccurrence o{};
  o.hash = 0xB0U + slot;
  o.screen_x = at.x;
  o.screen_y = at.y;
  o.w_tiles = kSprite / 8;
  o.h_tiles = kSprite / 8;
  o.slot = slot;
  return o;
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
  std::printf("=== missing_asset_test (spec 002 BR-093) ===\n");
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path() / "ayther_missing_asset";
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

  // Corrupt the pose asset: a PNG signature followed by garbage.
  {
    std::ofstream file(green_png, std::ios::binary | std::ios::trunc);
    const unsigned char bad[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A,
                                 0x1A, 0x0A, 0xDE, 0xAD, 0xBE, 0xEF};
    file.write(reinterpret_cast<const char *>(bad), sizeof(bad));
    if (!file)
      return 1;
  }

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "[FAIL] SDL_Init\n");
    return 1;
  }
  SDL_Window *window = SDL_CreateWindow("missing_asset_test", 64, 64,
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

  // 1. The preparation reports the unreadable asset.
  {
    const ayther::AytherRenderer::PrewarmReport warm =
        renderer.prewarm_textures(ctx, {}, {green_png});
    check(warm.failed == 1 && warm.missing_assets.size() == 1 &&
              warm.missing_assets[0] == green_png,
          "RF-2.11: the preparation reports the corrupt asset as missing");
  }

  // 2. The frame keeps the originals; the replacement's texture failed.
  const std::uint8_t *pixels =
      renderer.export_frame(ctx, fv, nullptr, true, &stack);
  check(pixels != nullptr, "the synthetic frame renders");
  if (pixels == nullptr)
    return 1;
  const ro::DrawReport report = renderer.last_draw_report();
  check(report.replacements.size() == 2 &&
            report.replacements[0].texture == ro::TextureState::failed,
        "the corrupt asset's texture is failed");
  check(seen_at(pixels, kAnchor) == Seen::red &&
            seen_at(pixels, kMember) == Seen::red,
        "RF-9.3: the anchor and the member keep their originals");

  // 3. The observation says why.
  const std::array occurrences{make_occurrence(kMember, 1),
                               make_occurrence(kAnchor, 0),
                               make_occurrence(kLost, 2)};
  const std::array<std::uint32_t, 3> owner{0, 0, 1};
  ayther::session::RenderObservationBuilder builder;
  ayther::session::RenderObservationInput in;
  in.emulation_frame = fv.frame_index;
  in.frame_known = true;
  in.occurrences = occurrences;
  in.subs = subs;
  in.pose_sub_count = 2;
  in.pose_owner = owner;
  in.draw = &report;
  const ro::RenderFrameView &view = builder.build(in);
  const auto *reason = std::get_if<std::string_view>(
      &view.occurrences[1].not_applied_reason.value);
  check(view.occurrences[1].status ==
                ro::OccurrenceStatus::assigned_not_applied &&
            reason != nullptr && *reason == "texture_failed",
        "RF-2.11: observed, assigned_not_applied (texture_failed), not "
        "original_unassigned");

  // 4. Replay QA stops on it.
  const auto missing = ayther::probe::first_missing_asset(report, subs);
  check(missing.has_value() && missing->replacement == 0 &&
            missing->asset == green_png,
        "RF-2.11: replay QA stops with the missing asset");

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
