// Spec 002, BR-022 (RF-7.3, RF-7.9), on a GPU.
//
// The renderer reports how it drew every sprite substitution of a frame and
// the state of its texture (contracts.md C3, DrawReport). Synthetic scene, no
// ROM, pack or recording:
//   - composed frame: the substitution anchored to a claimed scene sprite is
//     drawn in the scene pass (in_pass), the one without an anchor in the HD
//     lane (lane), the one whose anchor is hidden is not drawn (discarded);
//   - textures: a loose PNG is pending on its first frame and ready once its
//     asynchronous decode lands; a missing file is failed;
//   - hybrid frame (scene_dirty): every substitution goes to the lane,
//     including the priority-1 one deferred in front of the high plane;
//   - HD off: nothing is drawn and the report says so.
// What the report calls drawn is checked on screen, and the report is fed to
// the session's observation builder to get the status of each occurrence.
#include <ayther/ayther_layers.h>
#include <ayther/ayther_renderer.h>
#include <ayther/ayther_session.h>

#include "session/render_observation_builder.h"
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
// Substitutions 0..3, and one original sprite that nothing replaces.
constexpr std::array<Spot, 4> kSub{
    {{40, 100}, {80, 150}, {120, 100}, {260, 100}}};
constexpr Spot kOriginal{200, 100};

AytherSpriteSub make_sub(const std::string &asset, Spot at) {
  AytherSpriteSub s{};
  std::snprintf(s.asset_path, sizeof(s.asset_path), "%s", asset.c_str());
  s.screen_x = at.x;
  s.screen_y = at.y;
  s.w_tiles = kSprite / 8;
  s.h_tiles = kSprite / 8;
  s.w_px = kSprite;
  s.h_px = kSprite;
  s.palette = 0xFF;
  s.synth_pal = 0xFF;
  s.uw = 1.0F;
  s.vh = 1.0F;
  return s;
}

// The substitution a scene sprite anchors (-1 = none) and whether the author
// hid the sprite.
struct Anchor {
  std::int32_t sub = -1;
  bool hidden = false;
};

ayther::SceneElement make_sprite(Spot at, std::uint8_t slot, Anchor anchor) {
  const std::int32_t sub = anchor.sub;
  ayther::SceneElement e{};
  e.hash = 0xA0U + slot;
  e.x = at.x;
  e.y = at.y;
  e.w = kSprite;
  e.h = kSprite;
  e.pattern = 1;
  e.layer = 3;
  e.slot = slot;
  e.chain = slot;
  e.claimed = sub >= 0 ? 1 : 0;
  e.sub_kind = sub >= 0 ? 1 : 0;
  e.sub = sub;
  e.hidden = anchor.hidden ? 1 : 0;
  return e;
}

AytherSpriteOccurrence make_occurrence(Spot at, std::uint8_t slot) {
  AytherSpriteOccurrence o{};
  o.hash = 0xA0U + slot;
  o.screen_x = at.x;
  o.screen_y = at.y;
  o.w_tiles = kSprite / 8;
  o.h_tiles = kSprite / 8;
  o.slot = slot;
  return o;
}

bool green_at(const std::uint8_t *bgra, Spot at) {
  const std::size_t i =
      ((static_cast<std::size_t>(at.y) + 8) * kW + at.x + 8) * 4;
  return bgra[i + 1] > 200 && bgra[i] < 60 && bgra[i + 2] < 60;
}

std::string_view reason(const ro::OccurrenceView &o) {
  const auto *text = std::get_if<std::string_view>(&o.not_applied_reason.value);
  return text ? *text : std::string_view{};
}

bool all_rows(const ro::DrawReport &report, ro::DrawOutcome outcome) {
  bool all = !report.replacements.empty();
  for (const ro::ReplacementDraw &row : report.replacements)
    all = all && row.draw == outcome;
  return all;
}

} // namespace

int main() try {
  std::printf("=== render_draw_report_test (spec 002 BR-022) ===\n");
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path() / "ayther_render_draw_report";
  fs::create_directories(dir, ec);
  const std::string green_png = (dir / "green.png").string();
  const std::string missing_png = (dir / "missing.png").string();
  fs::remove(missing_png, ec);
  {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kSprite) * kSprite *
                                   4);
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
      rgba[i + 1] = 255;
      rgba[i + 3] = 255;
    }
    if (stbi_write_png(green_png.c_str(), kSprite, kSprite, 4, rgba.data(),
                       kSprite * 4) == 0) {
      std::fprintf(stderr, "[FAIL] cannot write %s\n", green_png.c_str());
      return 1;
    }
  }

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "[FAIL] SDL_Init\n");
    return 1;
  }
  SDL_Window *window = SDL_CreateWindow("render_draw_report_test", 64, 64,
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
  // Spec 002 (BR-092): the pending state only exists in the asynchronous
  // mode; the synchronous default decodes before the first frame.
  renderer.set_synchronous_textures(false);
  const std::string shaders = std::string(AYTHER_SOURCE_DIR) + "/shaders/";
  if (!renderer.init(ctx, kW, kH, shaders.c_str()) ||
      !renderer.readback_init(ctx)) {
    std::fprintf(stderr, "[FAIL] renderer (shaders in %s)\n", shaders.c_str());
    return 1;
  }

  check(renderer.last_draw_report().replacements.empty(),
        "before the first render there is nothing to report");

  // Pattern 1 = colour index 1 = red; the backdrop stays black.
  std::vector<std::uint8_t> vram(0x10000, 0);
  for (int i = 0; i < 32; ++i)
    vram[32 + i] = 0x11;
  std::vector<std::uint8_t> cram(128, 0);
  cram[2] = 7;
  std::vector<std::uint16_t> fb(static_cast<std::size_t>(kW) * kH, 0);

  const std::array subs{
      make_sub(green_png, kSub[0]), make_sub(green_png, kSub[1]),
      make_sub(missing_png, kSub[2]), make_sub(green_png, kSub[3])};
  std::array<std::uint8_t, 4> prio{};
  // Sub 0 and sub 2 are anchored to claimed sprites, sub 3 to a hidden one,
  // sub 1 has no anchor.
  const std::array scene{make_sprite(kSub[0], 0, Anchor{0, false}),
                         make_sprite(kSub[2], 1, Anchor{2, false}),
                         make_sprite(kOriginal, 2, Anchor{-1, false}),
                         make_sprite(kSub[3], 3, Anchor{3, true})};

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
  fv.frame_index = 100;

  const auto render = [&](bool hd_on) {
    return renderer.export_frame(ctx, fv, nullptr, hd_on, &stack);
  };

  // The session's builder, fed with the report: occurrences 0..3 are the
  // sprites of subs 0, 2 and 3 and the original one.
  const std::array occurrences{
      make_occurrence(kSub[0], 0), make_occurrence(kSub[2], 1),
      make_occurrence(kOriginal, 2), make_occurrence(kSub[3], 3)};
  const std::array<std::uint8_t, 4> hidden{0, 0, 0, 1};
  ayther::session::RenderObservationBuilder builder;
  const auto observe =
      [&](const ro::DrawReport &report) -> const ro::RenderFrameView & {
    ayther::session::RenderObservationInput in;
    in.emulation_frame = fv.frame_index;
    in.frame_known = true;
    in.occurrences = occurrences;
    in.hidden = hidden;
    in.subs = subs;
    in.draw = &report;
    return builder.build(in);
  };

  // 1. Composed frame, first render: the textures are still decoding.
  const std::uint8_t *pixels = render(true);
  check(pixels != nullptr, "the synthetic frame renders");
  ro::DrawReport report = renderer.last_draw_report();
  check(report.emulation_frame == 100 && report.hd_enabled &&
            report.replacements.size() == subs.size(),
        "one row per substitution, for the frame drawn, HD on");
  if (report.replacements.size() != subs.size())
    return 1;
  check(report.replacements[0].draw == ro::DrawOutcome::in_pass,
        "RF-7.3: the substitution anchored to a claimed sprite is drawn in "
        "the scene pass (in_pass)");
  check(report.replacements[1].draw == ro::DrawOutcome::lane,
        "RF-7.3: the substitution without an anchor is drawn in the lane");
  check(report.replacements[3].draw == ro::DrawOutcome::discarded,
        "RF-7.9: the substitution whose anchor is hidden is not drawn "
        "(discarded)");
  check(report.replacements[0].texture == ro::TextureState::pending &&
            report.replacements[1].texture == ro::TextureState::pending,
        "RF-7.9: a texture still decoding is pending on its first frame");
  check(report.replacements[2].draw == ro::DrawOutcome::in_pass &&
            report.replacements[2].texture == ro::TextureState::failed,
        "RF-7.9: a missing asset is failed");
  {
    const ro::RenderFrameView &view = observe(report);
    check(view.occurrences[0].status ==
                  ro::OccurrenceStatus::assigned_not_applied &&
              reason(view.occurrences[0]) == "texture_pending",
          "RF-7.9: observed, a pending texture is not applied "
          "(texture_pending)");
  }

  // 2. Same scene until the decode lands.
  bool ready = false;
  for (int attempt = 0; attempt < 600 && !ready; ++attempt) {
    ++fv.frame_index;
    pixels = render(true);
    if (pixels == nullptr)
      break;
    report = renderer.last_draw_report();
    ready = report.replacements.size() == subs.size() &&
            report.replacements[0].texture == ro::TextureState::ready;
    if (!ready)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(ready, "RF-7.9: the texture is ready once its decode lands");
  if (!ready || pixels == nullptr)
    return 1;
  check(report.emulation_frame == fv.frame_index,
        "the report names the frame it describes");
  check(report.replacements[1].texture == ro::TextureState::ready &&
            report.replacements[2].texture == ro::TextureState::failed,
        "RF-7.9: ready and failed textures side by side");
  check(report.replacements[0].draw == ro::DrawOutcome::in_pass &&
            report.replacements[1].draw == ro::DrawOutcome::lane &&
            report.replacements[3].draw == ro::DrawOutcome::discarded,
        "the draw outcomes do not depend on the texture state");
  check(green_at(pixels, kSub[0]) && green_at(pixels, kSub[1]),
        "what the report calls drawn (in_pass, lane) is on screen");
  check(!green_at(pixels, kSub[3]),
        "what the report calls discarded is not on screen");
  {
    const ro::RenderFrameView &view = observe(report);
    check(view.occurrences[0].status == ro::OccurrenceStatus::replaced &&
              view.replacements[0].draw == ro::DrawOutcome::in_pass,
          "RF-7.3: observed, the in_pass replacement replaces its sprite");
    check(view.occurrences[1].status ==
                  ro::OccurrenceStatus::assigned_not_applied &&
              reason(view.occurrences[1]) == "texture_failed",
          "RF-7.9: observed, a failed texture is not applied "
          "(texture_failed)");
    check(view.occurrences[2].status ==
                  ro::OccurrenceStatus::original_unassigned &&
              view.occurrences[3].status ==
                  ro::OccurrenceStatus::hidden_by_author,
          "observed, the original sprite and the hidden one keep their "
          "status");
  }

  // 3. Hybrid frame: no scene pass, every substitution goes to the lane; the
  //    priority-1 one is deferred in front of the high plane. Spec 002 (R8):
  //    the hybrid that keeps HD is the authoring dim (bit 2); a raster frame
  //    (bit 1) now shows the originals only (raster_frame_test).
  fv.scene_dirty = 2;
  prio[1] = 1;
  ++fv.frame_index;
  pixels = render(true);
  report = renderer.last_draw_report();
  check(pixels != nullptr && all_rows(report, ro::DrawOutcome::lane),
        "RF-7.3: in a hybrid frame every substitution is drawn in the lane, "
        "the priority-1 one included");
  check(pixels != nullptr && green_at(pixels, kSub[1]),
        "the deferred priority-1 substitution is on screen");
  fv.scene_dirty = 0;
  prio[1] = 0;

  // 4. HD off: nothing is drawn, and the observation says why.
  ++fv.frame_index;
  pixels = render(false);
  report = renderer.last_draw_report();
  check(pixels != nullptr && !report.hd_enabled &&
            all_rows(report, ro::DrawOutcome::discarded),
        "RF-7.9: with HD off no substitution is drawn");
  {
    const ro::RenderFrameView &view = observe(report);
    check(view.occurrences[0].status ==
                  ro::OccurrenceStatus::assigned_not_applied &&
              reason(view.occurrences[0]) == "hd_off",
          "RF-7.9: observed, the reason is hd_off");
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
