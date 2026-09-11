#include <ayther/ayther_layers.h>
#include <ayther/ayther_renderer.h>
#include <ayther/ayther_session.h>

#include "vulkan_test_context.h"

#include <SDL3/SDL.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace {
constexpr std::uint32_t kWidth = 320;
constexpr std::uint32_t kHeight = 224;
int failures = 0;

void check(bool condition, const char *message) {
    if (!condition)
        ++failures;
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

int run_test() {
    const std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window{
        SDL_CreateWindow("Layer focus test", 64, 64, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN),
        SDL_DestroyWindow};
    if (!window)
        return 1;
    VulkanTestContext context;
    if (!context.init(window.get()))
        return 1;
    ayther::AytherRenderer renderer;
    const std::string shader_directory = std::string{AYTHER_SOURCE_DIR} + "/shaders";
    if (!renderer.init(context, kWidth, kHeight, shader_directory.c_str()) ||
        !renderer.readback_init(context)) {
        renderer.shutdown(context);
        return 1;
    }

    std::vector<std::uint8_t> vram(65536, 0);
    std::fill(vram.begin() + 32, vram.begin() + 64, std::uint8_t{0x11});
    std::array<std::uint8_t, 128> cram{};
    cram[2] = 7; // Packed CRAM: palette index 1 is saturated red.
    std::array<ayther::SceneElement, 4> scene{};
    for (std::size_t layer = 0; layer < scene.size(); ++layer) {
        auto &element = scene[layer];
        element.x = static_cast<std::int16_t>(32 + layer * 48);
        element.y = 80;
        element.w = element.h = 8;
        element.pattern = 1;
        element.layer = static_cast<std::uint8_t>(layer);
        element.hash = 100 + layer;
    }
    std::vector<std::uint16_t> framebuffer(static_cast<std::size_t>(kWidth) * kHeight, 0);
    ayther::FrameView frame{};
    frame.fb_width = kWidth;
    frame.fb_height = kHeight;
    frame.fb_pixels = framebuffer.data();
    frame.fb_pitch = kWidth * 2;
    frame.fb_format = 2;
    frame.scene = scene.data();
    frame.scene_count = static_cast<std::uint32_t>(scene.size());
    frame.scene_vram = vram.data();
    frame.scene_vram_size = vram.size();
    frame.scene_cram = cram.data();
    frame.scene_cram_size = cram.size();
    AytherLayerStack layers;
    std::array<float, 4> baseline_red{};
    for (int focused_layer = -1; focused_layer < 4; ++focused_layer) {
        renderer.set_focus_layer(focused_layer);
        const auto *pixels = renderer.export_frame(context, frame, nullptr, true, &layers);
        check(pixels != nullptr, "the composed frame renders");
        if (!pixels)
            continue;
        for (std::size_t layer = 0; layer < scene.size(); ++layer) {
            const auto pixel_index = (84 * kWidth + 36 + layer * 48) * 4;
            const bool dimmed = focused_layer >= 0 && focused_layer != static_cast<int>(layer);
            const float brightness = focused_layer == 3 ? 0.25f : 0.5f;
            const float actual_red = pixels[pixel_index + 2];
            if (focused_layer == -1) {
                baseline_red[layer] = actual_red;
                check(actual_red > 200.0f, "each control cell contributes visible red pixels");
            }
            // Palette conversion owns the source intensity. Focus scales that
            // measured intensity, independently of the palette's voltage table.
            const float expected_red =
                dimmed ? baseline_red[layer] * brightness * (191.0f / 255.0f) : baseline_red[layer];
            if (std::fabs(actual_red - expected_red) > 3.0f) {
                std::printf("focus=%d layer=%zu red=%.0f expected=%.2f\n", focused_layer, layer,
                            actual_red, expected_red);
            }
            check(std::fabs(actual_red - expected_red) <= 3.0f && pixels[pixel_index] < 3 &&
                      pixels[pixel_index + 1] < 3,
                  "focus preserves its own layer and applies the correct brightness and opacity "
                  "elsewhere");
        }
    }
    renderer.shutdown(context);
    return failures == 0 ? 0 : 1;
}
} // namespace

int main() try {
    if (!SDL_Init(SDL_INIT_VIDEO))
        return 1;
    const int result = run_test();
    SDL_Quit();
    return result;
} catch (const std::exception &error) {
    std::fprintf(stderr, "[FAIL] Unexpected exception: %s\n", error.what());
    SDL_Quit();
    return 1;
} catch (...) {
    std::fprintf(stderr, "[FAIL] Unexpected non-standard exception\n");
    SDL_Quit();
    return 1;
}
