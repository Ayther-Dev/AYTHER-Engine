// GPU oracle for spec 002 phase 5 (R4, R3, R2): renders a synthetic FrameView
// with the Vulkan renderer and compares it pixel by pixel with the CPU
// reference compositor (reference_compositor.h), the same comparison as the
// probe's O1. HD replacements are solid colours whose RGB is exactly what the
// compositor gives an original of the same CRAM colour, so the expected image
// is the reference composition of the scene with each replacement drawn as
// that original.
#pragma once

#include <ayther/ayther_layers.h>
#include <ayther/ayther_renderer.h>
#include <ayther/ayther_session.h>

#include "../../tools/render_probe/probe_images.h"
#include "reference_compositor.h"
#include "vulkan_test_context.h"

#include <SDL3/SDL.h>
#include <stb_image_write.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ayther::test {

/// RGB8 the renderer shows for a packed CRAM colour (RGB565 of the core,
/// expanded as the probe's core_image does).
inline void cram_rgb8(std::uint16_t packed, std::uint8_t rgb[3]) {
  const std::uint16_t p565 = genesis565(packed);
  const probe::RgbImage one = probe::core_image(&p565, 1, 1, 2, 2);
  rgb[0] = one.rgb[0];
  rgb[1] = one.rgb[1];
  rgb[2] = one.rgb[2];
}

/// Writes a w×h PNG of one opaque RGB colour.
inline bool write_solid_png(const std::string &path, int w, int h,
                            const std::uint8_t rgb[3]) {
  std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
  for (std::size_t i = 0; i < px.size(); i += 4) {
    px[i] = rgb[0];
    px[i + 1] = rgb[1];
    px[i + 2] = rgb[2];
    px[i + 3] = 255;
  }
  return stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4) != 0;
}

class GpuOracle {
public:
  bool init(std::uint32_t w, std::uint32_t h) {
    w_ = w;
    h_ = h;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
      std::fprintf(stderr, "[FAIL] SDL_Init: %s\n", SDL_GetError());
      return false;
    }
    window_ = SDL_CreateWindow("gpu_oracle", 64, 64,
                               SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (window_ == nullptr) {
      std::fprintf(stderr, "[FAIL] SDL_CreateWindow: %s\n", SDL_GetError());
      return false;
    }
    if (!ctx_.init(window_))
      return false;
    const std::string shaders = std::string(AYTHER_SOURCE_DIR) + "/shaders/";
    return renderer_.init(ctx_, w, h, shaders.c_str()) &&
           renderer_.readback_init(ctx_);
  }

  ~GpuOracle() {
    if (window_ != nullptr) {
      vkDeviceWaitIdle(ctx_.device());
      renderer_.readback_shutdown(ctx_);
      renderer_.shutdown(ctx_);
      ctx_.shutdown();
      SDL_DestroyWindow(window_);
      SDL_Quit();
    }
  }
  GpuOracle() = default;
  GpuOracle(const GpuOracle &) = delete;
  GpuOracle &operator=(const GpuOracle &) = delete;

  ayther::AytherRenderer &renderer() { return renderer_; }

  /// The renderer's image of `fv` (HD on, default layer stack).
  probe::RgbImage render(const ayther::FrameView &fv) {
    const std::uint8_t *px =
        renderer_.export_frame(ctx_, fv, nullptr, true, &stack_);
    if (px == nullptr)
      return {};
    return probe::readback_image(px, w_, h_);
  }

  /// The reference composition of `elements` as RGB8.
  probe::RgbImage expected(std::span<const SceneElement> elements,
                           std::span<const std::uint8_t> vram,
                           std::span<const std::uint8_t> cram) const {
    ReferenceScene scene;
    scene.width = static_cast<int>(w_);
    scene.height = static_cast<int>(h_);
    scene.elements = elements;
    scene.vram = vram;
    scene.cram = cram;
    const std::vector<std::uint16_t> image = compose_reference(scene);
    return probe::core_image(image.data(), w_, h_, w_ * 2, 2);
  }

private:
  std::uint32_t w_ = 0, h_ = 0;
  SDL_Window *window_ = nullptr;
  VulkanTestContext ctx_;
  ayther::AytherRenderer renderer_;
  AytherLayerStack stack_;
};

} // namespace ayther::test
