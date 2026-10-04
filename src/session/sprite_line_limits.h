// Spec 002, R5 (RF-9.4): which parsed sprites the VDP actually draws on some
// line. Per line, in link-chain order, the VDP draws at most `max_sprites`
// sprites and `max_pixels` sprite pixels (H40: 20 and 320; H32: 16 and 256)
// unless the core option lifts the limit (genesis_plus_gx_no_sprite_limit),
// and a sprite at raw x = 0 masks the sprites after it on that line once a
// sprite with x != 0 came before it. Pure: the scene inventory feeds it the
// parsed list of the frame.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ayther::session {

struct ParsedSprite {
  std::int16_t x_raw = 0; ///< SAT x (screen x + 128); 0 = the mask
  std::int16_t y = 0;     ///< screen y of the top line
  std::uint8_t w_tiles = 1;
  std::uint8_t h_tiles = 1;
  std::uint8_t chain = 0; ///< link-chain position (lower = drawn first)
};

struct SpriteLineLimits {
  bool enabled = true; ///< false with no_sprite_limit
  int max_sprites = 20;
  int max_pixels = 320;
};

/// The limits of a display width (256 = H32, otherwise H40).
inline SpriteLineLimits line_limits_for(std::uint32_t width, bool no_limit) {
  SpriteLineLimits l;
  l.enabled = !no_limit;
  l.max_sprites = width <= 256 ? 16 : 20;
  l.max_pixels = width <= 256 ? 256 : 320;
  return l;
}

/// Per sprite: 1 when the VDP draws it on at least one of lines [0, lines)
/// it covers, or when it covers none of them (nothing to judge); 0 when it
/// covers visible lines and is drawn on none.
inline std::vector<std::uint8_t>
drawn_on_some_line(std::span<const ParsedSprite> sprites, int lines,
                   const SpriteLineLimits &limits) {
  const std::size_t n = sprites.size();
  std::vector<std::size_t> order(n);
  for (std::size_t i = 0; i < n; ++i)
    order[i] = i;
  std::stable_sort(order.begin(), order.end(),
                   [&](std::size_t a, std::size_t b) {
                     return sprites[a].chain < sprites[b].chain;
                   });
  std::vector<std::uint8_t> visible(n, 0);
  std::vector<std::uint8_t> drawn(n, 0);
  for (int line = 0; line < lines; ++line) {
    int count = 0;
    int pixels = 0;
    bool seen_nonzero = false;
    bool masking = false;
    for (const std::size_t i : order) {
      const ParsedSprite &s = sprites[i];
      if (line < s.y || line >= s.y + s.h_tiles * 8)
        continue;
      // Every sprite of the line counts toward the limits, the mask included.
      ++count;
      const int width = s.w_tiles * 8;
      const bool over = limits.enabled && (count > limits.max_sprites ||
                                           pixels >= limits.max_pixels);
      pixels += width;
      if (s.x_raw == 0) {
        if (seen_nonzero)
          masking = true;
        continue; // the mask itself draws nothing
      }
      seen_nonzero = true;
      visible[i] = 1;
      if (!masking && !over)
        drawn[i] = 1;
    }
  }
  std::vector<std::uint8_t> out(n, 1);
  for (std::size_t i = 0; i < n; ++i)
    if (visible[i] && !drawn[i])
      out[i] = 0;
  return out;
}

} // namespace ayther::session
