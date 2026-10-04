// Spec 002, R3 (RF-8.1, RF-8.2): the VDP sprite layer. Between sprites the
// first of the link chain wins the pixel whatever its priority; the winning
// pixel then enters below or above the planes by ITS priority bit. Pure.
//
// The renderer draws sprites in two passes (pri-0 after the low planes, pri-1
// after the high ones). It reproduces the rule with a depth buffer: every
// sprite pixel carries the depth of its chain position (chain_depth); the
// pri-0 pass keeps the frontmost depth, and a pri-1 pixel is drawn only where
// no sprite in front of it (lower chain) left a pixel. `two_pass_depth`
// models exactly that, so a test can check it against `vdp_winner`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace ayther::session {

/// One opaque sprite pixel at a screen position.
struct SpriteFragment {
  std::uint8_t chain = 0;    ///< link-chain position (lower = in front)
  std::uint8_t priority = 0; ///< VDP priority bit
};

/// What ends up on screen at a pixel from the sprite layer.
struct SpritePixel {
  std::size_t fragment = 0;  ///< index of the winning fragment
  std::uint8_t priority = 0; ///< level it enters at (0 = below high planes)
};

/// Depth of a chain position: strictly increasing in (0, 1), so the depth
/// buffer's clear value 1.0 is behind every sprite.
constexpr float chain_depth(std::uint8_t chain) noexcept {
  return (static_cast<float>(chain) + 1.0F) / 257.0F;
}

/// The VDP rule: the first of the chain wins; it enters by its priority.
inline std::optional<SpritePixel>
vdp_winner(std::span<const SpriteFragment> fragments) {
  std::optional<SpritePixel> best;
  for (std::size_t i = 0; i < fragments.size(); ++i)
    if (!best || fragments[i].chain < fragments[best->fragment].chain)
      best = SpritePixel{i, fragments[i].priority};
  return best;
}

/// The renderer's two passes over the same pixel, in any draw order:
/// pass 0 draws the pri-0 fragments with depth test LESS and depth write;
/// pass 1 draws the pri-1 fragments with depth test LESS and depth write
/// over the depth pass 0 left. Returns the fragment whose colour remains
/// from the sprite layer, and the pass that drew it.
inline std::optional<SpritePixel>
two_pass_depth(std::span<const SpriteFragment> fragments) {
  float depth = 1.0F; // the clear value: behind every sprite
  std::optional<SpritePixel> colour;
  for (std::uint8_t pass = 0; pass < 2; ++pass)
    for (std::size_t i = 0; i < fragments.size(); ++i) {
      if (fragments[i].priority != pass)
        continue;
      const float z = chain_depth(fragments[i].chain);
      if (z < depth) { // depth test LESS, then depth write
        depth = z;
        colour = SpritePixel{i, pass};
      }
    }
  return colour;
}

} // namespace ayther::session
