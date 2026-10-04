// Spec 002, BR-038 (RF-3.5): the O1 fidelity check of the render probe
// (plan §5.13). Without pack and shaders the composed image at native
// resolution must be the core's framebuffer pixel for pixel. Checked with an
// identical frame and one altered on purpose, and with the framebuffer
// formats the core can emit.
#include "../../tools/render_probe/probe_images.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

using ayther::probe::check_o1;
using ayther::probe::core_image;
using ayther::probe::readback_image;

constexpr std::uint32_t kW = 16;
constexpr std::uint32_t kH = 8;

// An RGB565 framebuffer and the BGRA8 readback a faithful renderer gives.
struct Frame {
  std::vector<std::uint16_t> fb;
  std::vector<std::uint8_t> bgra;
};

Frame make_frame() {
  Frame f;
  f.fb.resize(static_cast<std::size_t>(kW) * kH);
  f.bgra.resize(f.fb.size() * 4);
  for (std::size_t i = 0; i < f.fb.size(); ++i) {
    const unsigned r5 = (i * 3) & 0x1FU;
    const unsigned g6 = (i * 5) & 0x3FU;
    const unsigned b5 = (i * 7) & 0x1FU;
    f.fb[i] = static_cast<std::uint16_t>((r5 << 11U) | (g6 << 5U) | b5);
    f.bgra[i * 4 + 2] = static_cast<std::uint8_t>((r5 << 3U) | (r5 >> 2U));
    f.bgra[i * 4 + 1] = static_cast<std::uint8_t>((g6 << 2U) | (g6 >> 4U));
    f.bgra[i * 4 + 0] = static_cast<std::uint8_t>((b5 << 3U) | (b5 >> 2U));
    f.bgra[i * 4 + 3] = 255;
  }
  return f;
}
} // namespace

int main() try {
  Frame frame = make_frame();
  const auto core = core_image(frame.fb.data(), kW, kH, kW * 2, 2);
  const auto o1 = check_o1(readback_image(frame.bgra.data(), kW, kH), core);
  check(o1.identical && o1.differing_pixels == 0,
        "RF-3.5: an identical composed frame passes O1");

  frame.bgra[(3 * kW + 5) * 4 + 1] ^= 0x04U; // one green bit at (5, 3)
  frame.bgra[(6 * kW + 9) * 4 + 0] ^= 0x80U; // one blue bit at (9, 6)
  const auto altered =
      check_o1(readback_image(frame.bgra.data(), kW, kH), core);
  check(!altered.identical && altered.differing_pixels == 2 &&
            altered.first_x == 5 && altered.first_y == 3,
        "RF-3.5: a frame altered on purpose fails O1, with the pixels found");

  const auto smaller =
      check_o1(readback_image(frame.bgra.data(), kW, kH / 2), core);
  check(!smaller.identical, "an image of another size fails O1");

  // XRGB8888 framebuffers compare channel for channel.
  std::vector<std::uint32_t> fb32(static_cast<std::size_t>(kW) * kH);
  std::vector<std::uint8_t> bgra32(fb32.size() * 4);
  for (std::size_t i = 0; i < fb32.size(); ++i) {
    fb32[i] = static_cast<std::uint32_t>(i * 0x010203U) & 0xFFFFFFU;
    bgra32[i * 4 + 0] = static_cast<std::uint8_t>(fb32[i]);
    bgra32[i * 4 + 1] = static_cast<std::uint8_t>(fb32[i] >> 8U);
    bgra32[i * 4 + 2] = static_cast<std::uint8_t>(fb32[i] >> 16U);
  }
  check(check_o1(readback_image(bgra32.data(), kW, kH),
                 core_image(fb32.data(), kW, kH, kW * 4, 1))
            .identical,
        "an XRGB8888 framebuffer passes O1 when identical");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
