// Spec 007 (RF-5.1, RNF-3), on a GPU.
//
// Element records are composed with no emulator by the Vulkan renderer
// (build_element_scene + AytherRenderer::export_elements) and by the CPU
// compositor (compose_elements_rgba). Both must give the same image, pixel by
// pixel: the CPU path is the oracle of the GPU one. Synthetic records, no ROM.
#include <ayther/ayther_element_record.h>
#include <ayther/ayther_element_scene.h>
#include <ayther/ayther_renderer.h>
#include <ayther/ayther_session.h>

#include "vulkan_test_context.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace {

int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

constexpr std::uint32_t kW = 64;
constexpr std::uint32_t kH = 48;

/// A record whose pixels are all different: index = (x + 3y) % 15 + 1.
ayther::ElementRecord gradient_sprite(std::uint8_t w, std::uint8_t h) {
  ayther::ElementRecord r;
  r.kind = ayther::ElementKind::Sprite;
  r.w_tiles = w;
  r.h_tiles = h;
  r.pattern.assign(static_cast<std::size_t>(w) * h * 32U, 0);
  for (std::uint32_t t = 0; t < static_cast<std::uint32_t>(w) * h; ++t)
    for (std::uint32_t row = 0; row < 8; ++row)
      for (std::uint32_t col = 0; col < 8; ++col) {
        const std::uint32_t idx = ((col + 3U * row + t) % 15U) + 1U;
        std::uint8_t &byte = r.pattern[t * 32U + row * 4U + col / 2U];
        byte = static_cast<std::uint8_t>((col & 1U)
                                             ? ((byte & 0xF0U) | idx)
                                             : ((idx << 4U) | (byte & 0x0FU)));
      }
  // Leave a transparent hole so the backdrop shows through.
  r.pattern[0] = 0x00;
  ayther::ElementPaletteVariant pv;
  for (std::uint16_t i = 0; i < 16; ++i)
    pv.words[i] = static_cast<std::uint16_t>(
        (i & 7U) | (((i * 3U) & 7U) << 3U) | (((7U - (i & 7U)) & 7U) << 6U));
  r.palettes.push_back(pv);
  return r;
}

} // namespace

int main() try {
  std::printf("=== element_scene_gpu_test (spec 007 RF-5.1) ===\n");
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "[FAIL] SDL_Init\n");
    return 1;
  }
  SDL_Window *window = SDL_CreateWindow("element_scene_gpu_test", 64, 64,
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

  const ayther::ElementRecord a = gradient_sprite(2, 2);
  const ayther::ElementRecord b = gradient_sprite(3, 1);
  const ayther::ElementPlacement placements[] = {
      {&a, 4, 6, 0, 0, nullptr},
      {&b, 14, 12, 0, 1, nullptr}, // hflip, overlapping `a`, on top
      {&a, 40, 20, 0, 2, nullptr}, // vflip
      {&b, -6, 40, 0, 3, nullptr}, // hv, clipped at the left edge
  };
  constexpr std::uint16_t kBackdrop = 0x0A3;

  ayther::ElementScene scene;
  std::string err;
  check(ayther::build_element_scene(placements, kW, kH, kBackdrop, scene, &err),
        "the scene builds");
  ayther::ElementImage cpu;
  check(
      ayther::compose_elements_rgba(placements, kW, kH, kBackdrop, false, cpu),
      "the CPU composes the same placements");

  const std::uint8_t *bgra = renderer.export_elements(ctx, scene);
  check(bgra != nullptr, "the renderer exports the element scene");
  if (bgra != nullptr) {
    std::uint32_t diffs = 0;
    for (std::uint32_t i = 0; i < kW * kH; ++i) {
      const std::uint8_t *g = bgra + static_cast<std::size_t>(i) * 4U;
      const std::uint8_t *c =
          cpu.rgba.data() + static_cast<std::size_t>(i) * 4U;
      if (g[2] != c[0] || g[1] != c[1] || g[0] != c[2])
        ++diffs;
    }
    std::printf("  differing pixels: %u of %u\n", diffs, kW * kH);
    for (const std::uint32_t i : {0U, 63U, 6U * kW + 5U, 13U * kW + 16U}) {
      if (diffs == 0)
        break;
      const std::uint8_t *g = bgra + static_cast<std::size_t>(i) * 4U;
      const std::uint8_t *c =
          cpu.rgba.data() + static_cast<std::size_t>(i) * 4U;
      std::printf("  px %u: gpu bgra %u %u %u %u | cpu rgba %u %u %u %u\n", i,
                  g[0], g[1], g[2], g[3], c[0], c[1], c[2], c[3]);
    }
    check(diffs == 0, "GPU and CPU agree pixel by pixel (RNF-3)");
  }

  ayther::ElementScene wrong;
  check(ayther::build_element_scene(placements, kW + 8, kH, kBackdrop, wrong) &&
            renderer.export_elements(ctx, wrong) == nullptr,
        "a scene of another size than the canvas is refused");

  renderer.readback_shutdown(ctx);
  renderer.shutdown(ctx);
  ctx.shutdown();
  SDL_DestroyWindow(window);
  SDL_Quit();
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
