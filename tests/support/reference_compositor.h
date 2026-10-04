// CPU reference compositor for spec 002 tests (BR-034; RF-8.1, RF-8.2),
// recovered from tools/scene_inventory_smoke. It composes a scene inventory
// (SceneElement list) with the VDP rules and the core's exact RGB565 palette,
// so a GPU composition can be compared with it pixel by pixel.
#pragma once

#include <ayther/ayther_session.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ayther::test {

/// The core's exact RGB565 for a packed CRAM colour (3-bit channels, normal
/// level ×2, expanded like the core's MAKE_PIXEL).
inline std::uint16_t genesis565(std::uint16_t packed) {
  const unsigned r4 = ((packed >> 0U) & 7U) << 1U;
  const unsigned g4 = ((packed >> 3U) & 7U) << 1U;
  const unsigned b4 = ((packed >> 6U) & 7U) << 1U;
  const unsigned r5 = (r4 << 1U) | (r4 >> 3U);
  const unsigned g6 = (g4 << 2U) | (g4 >> 2U);
  const unsigned b5 = (b4 << 1U) | (b4 >> 3U);
  return static_cast<std::uint16_t>((r5 << 11U) | (g6 << 5U) | b5);
}

struct ReferenceScene {
  int width = 0;
  int height = 0;
  std::span<const SceneElement> elements;
  std::span<const std::uint8_t> vram; // the VDP's VRAM, bus view (64 KiB)
  std::span<const std::uint8_t> cram; // 64 packed colours, little-endian
  std::uint8_t backdrop_index = 0;    // VDP register 7 & 0x3F
  bool left_blank = false;            // VDP register 0 bit 5
};

/// Colour index 0..15 of a tile pixel (bus view of VRAM: byte offset ^ 1).
inline std::uint8_t tile_pixel(std::span<const std::uint8_t> vram,
                               std::uint32_t pattern, int px, int py) {
  const std::uint32_t off =
      (pattern * 32U + static_cast<std::uint32_t>(py) * 4U +
       static_cast<std::uint32_t>(px >> 1)) ^
      1U;
  const std::uint8_t b = off < vram.size() ? vram[off] : 0;
  return (px & 1) != 0 ? static_cast<std::uint8_t>(b & 0x0FU)
                       : static_cast<std::uint8_t>(b >> 4U);
}

/// RGB565 image of `scene`, row-major. The VDP rules:
///   1. sprites into a first-wins buffer in GLOBAL link-chain order (lower
///      chain = further front); priority does not take part between sprites,
///      it only decides at which level the winning pixel enters;
///   2. back to front: backdrop, B low, A low, Window low, sprite pixels low,
///      B high, A high, Window high, sprite pixels high (index 0 lets the
///      layer behind show);
///   3. left-column blanking when register 0 bit 5 is set.
inline std::vector<std::uint16_t>
compose_reference(const ReferenceScene &scene) {
  const int w = scene.width;
  const int h = scene.height;
  const auto size = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  std::array<std::uint16_t, 64> palette{};
  for (std::size_t i = 0; i < palette.size() && i * 2 + 1 < scene.cram.size();
       ++i)
    palette[i] = genesis565(static_cast<std::uint16_t>(
        scene.cram[i * 2] | (scene.cram[i * 2 + 1] << 8U)));
  const std::uint16_t backdrop = palette[scene.backdrop_index & 0x3FU];

  // 1. Sprites, front of the chain first; the first opaque pixel wins.
  std::vector<const SceneElement *> sprites;
  for (const SceneElement &e : scene.elements)
    if (e.layer == 3)
      sprites.push_back(&e);
  std::stable_sort(sprites.begin(), sprites.end(),
                   [](const SceneElement *a, const SceneElement *b) {
                     return a->chain < b->chain;
                   });
  std::vector<std::uint8_t> sprite_index(size, 0);
  std::vector<std::uint8_t> sprite_priority(size, 0);
  for (const SceneElement *e : sprites) {
    const int tiles_high = e->h / 8;
    for (int py = 0; py < e->h; ++py) {
      const int sy = e->y + py;
      if (sy < 0 || sy >= h)
        continue;
      const int ly = (e->flips & 2U) != 0 ? e->h - 1 - py : py;
      for (int px = 0; px < e->w; ++px) {
        const int sx = e->x + px;
        if (sx < 0 || sx >= w)
          continue;
        const std::size_t at = static_cast<std::size_t>(sy) * w + sx;
        if (sprite_index[at] != 0)
          continue;
        const int lx = (e->flips & 1U) != 0 ? e->w - 1 - px : px;
        // Sprite tiles are column-major: base + column * height + row.
        const std::uint32_t tile =
            (static_cast<std::uint32_t>(e->pattern & 0x7FFU) +
             static_cast<std::uint32_t>(lx / 8) * tiles_high +
             static_cast<std::uint32_t>(ly / 8)) &
            0x7FFU;
        const std::uint8_t index = tile_pixel(scene.vram, tile, lx & 7, ly & 7);
        if (index == 0)
          continue;
        sprite_index[at] =
            static_cast<std::uint8_t>(((e->palette & 3U) << 4U) | index);
        sprite_priority[at] = e->priority;
      }
    }
  }

  // 2. Back to front.
  std::vector<std::uint16_t> image(size, backdrop);
  const auto paint_cells = [&](std::uint8_t layer, std::uint8_t priority) {
    for (const SceneElement &e : scene.elements) {
      if (e.layer != layer || e.priority != priority)
        continue;
      const bool clipped = e.clip_x1 > e.clip_x0; // spec 002 R8: scroll band
      for (int py = 0; py < 8; ++py) {
        const int sy = e.y + py;
        if (sy < 0 || sy >= h ||
            (clipped && (sy < e.clip_y0 || sy >= e.clip_y1)))
          continue;
        const int ly = (e.flips & 2U) != 0 ? 7 - py : py;
        for (int px = 0; px < 8; ++px) {
          const int sx = e.x + px;
          if (sx < 0 || sx >= w ||
              (clipped && (sx < e.clip_x0 || sx >= e.clip_x1)))
            continue;
          const int lx = (e.flips & 1U) != 0 ? 7 - px : px;
          const std::uint8_t index = tile_pixel(scene.vram, e.pattern, lx, ly);
          if (index != 0)
            image[static_cast<std::size_t>(sy) * w + sx] =
                palette[((e.palette & 3U) << 4U) | index];
        }
      }
    }
  };
  const auto paint_sprites = [&](std::uint8_t priority) {
    for (std::size_t i = 0; i < size; ++i)
      if (sprite_index[i] != 0 && sprite_priority[i] == priority)
        image[i] = palette[sprite_index[i]];
  };
  for (std::uint8_t priority = 0; priority < 2; ++priority) {
    paint_cells(0, priority); // plane B
    paint_cells(1, priority); // plane A
    paint_cells(2, priority); // window
    paint_sprites(priority);
  }

  // 3. Left-column blanking.
  if (scene.left_blank)
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < 8 && x < w; ++x)
        image[static_cast<std::size_t>(y) * w + x] = backdrop;
  return image;
}

} // namespace ayther::test
