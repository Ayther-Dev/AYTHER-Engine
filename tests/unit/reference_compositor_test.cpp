// Spec 002, BR-034 (RF-8.1, RF-8.2): the CPU reference compositor composes
// synthetic scenes with the VDP rules: between sprites the first of the link
// chain wins the pixel whatever its priority, and the winning pixel's priority
// then places it below or above the planes' cells. Four cases cross chain
// order and priority.
#include "reference_compositor.h"

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

using ayther::SceneElement;
using ayther::test::compose_reference;
using ayther::test::genesis565;
using ayther::test::ReferenceScene;

constexpr int kW = 32;
constexpr int kH = 16;

// Patterns 1..4 filled with colour indices 1..4; pattern 5 is index 5 on its
// left half and transparent on its right half.
std::vector<std::uint8_t> make_vram() {
  std::vector<std::uint8_t> vram(0x10000, 0);
  for (std::uint32_t p = 1; p <= 4; ++p)
    for (std::uint32_t b = 0; b < 32; ++b)
      vram[p * 32 + b] = static_cast<std::uint8_t>((p << 4) | p);
  for (std::uint32_t row = 0; row < 8; ++row)
    for (std::uint32_t col = 0; col < 2; ++col)
      vram[(5 * 32 + row * 4 + col) ^ 1U] = 0x55;
  return vram;
}

// Palette line 0: index i has red = i (3-bit channels), so every index has
// its own RGB565 value.
std::vector<std::uint8_t> make_cram() {
  std::vector<std::uint8_t> cram(128, 0);
  for (int i = 0; i < 8; ++i)
    cram[static_cast<std::size_t>(i) * 2] = static_cast<std::uint8_t>(i);
  return cram;
}

SceneElement sprite(std::uint16_t pattern, std::uint8_t chain,
                    std::uint8_t priority) {
  SceneElement e{};
  e.x = 0;
  e.y = 0;
  e.w = 8;
  e.h = 8;
  e.pattern = pattern;
  e.layer = 3;
  e.slot = chain;
  e.chain = chain;
  e.priority = priority;
  return e;
}

SceneElement cell(std::uint8_t layer, std::uint16_t pattern,
                  std::uint8_t priority) {
  SceneElement e{};
  e.x = 0;
  e.y = 0;
  e.w = 8;
  e.h = 8;
  e.pattern = pattern;
  e.layer = layer;
  e.priority = priority;
  return e;
}

std::uint16_t colour(int index) {
  return genesis565(static_cast<std::uint16_t>(index));
}

std::uint16_t pixel(const std::vector<SceneElement> &elements, int x, int y) {
  const std::vector<std::uint8_t> vram = make_vram();
  const std::vector<std::uint8_t> cram = make_cram();
  ReferenceScene scene;
  scene.width = kW;
  scene.height = kH;
  scene.elements = elements;
  scene.vram = vram;
  scene.cram = cram;
  scene.backdrop_index = 7;
  const std::vector<std::uint16_t> image = compose_reference(scene);
  return image[static_cast<std::size_t>(y) * kW + x];
}
} // namespace

int main() try {
  check(pixel({}, 3, 3) == colour(7) && pixel({}, 20, 10) == colour(7),
        "an empty scene is the backdrop");

  // 1. Chain beats priority between sprites: the low front sprite hides the
  //    high one behind it.
  check(pixel({sprite(1, 0, 0), sprite(2, 1, 1)}, 3, 3) == colour(1),
        "RF-8.1: the first of the chain wins between sprites, even when it is "
        "low priority and the one behind is high");

  // 2. The winner's priority places it: below a high plane cell, even though
  //    the high sprite behind it would have been above the cell.
  check(pixel({cell(1, 3, 1), sprite(1, 0, 0), sprite(2, 1, 1)}, 3, 3) ==
            colour(3),
        "RF-8.2: the winning low sprite goes below a high plane cell, and the "
        "high sprite behind it does not show through");

  // 3. A high front sprite stays above a high cell and hides the low one.
  check(pixel({cell(1, 3, 1), sprite(1, 0, 1), sprite(2, 1, 0)}, 3, 3) ==
            colour(1),
        "RF-8.2: a high sprite first in the chain is above a high plane cell");

  // 4. Where the front sprite is transparent the next one in the chain wins,
  //    with its own priority: above a low cell, below a high one.
  check(pixel({cell(0, 3, 0), sprite(5, 0, 1), sprite(2, 1, 0)}, 6, 3) ==
                colour(2) &&
            pixel({cell(0, 3, 1), sprite(5, 0, 1), sprite(2, 1, 0)}, 6, 3) ==
                colour(3) &&
            pixel({cell(0, 3, 1), sprite(5, 0, 1), sprite(2, 1, 0)}, 1, 3) ==
                colour(5),
        "RF-8.1, RF-8.2: through a transparent pixel the next sprite of the "
        "chain wins, placed by its own priority");

  // Planes: A over B within a priority, and high over low.
  check(pixel({cell(1, 2, 0), cell(0, 1, 0)}, 3, 3) == colour(2) &&
            pixel({cell(1, 2, 0), cell(0, 1, 1)}, 3, 3) == colour(1),
        "plane A covers plane B at the same priority; a high B covers a low A");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
