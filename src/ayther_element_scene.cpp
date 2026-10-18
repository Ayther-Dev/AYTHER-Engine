// ---------------------------------------------------------------------------
// ayther_element_scene.cpp — the synthetic frame of element records. See
// ayther_element_scene.h.
// ---------------------------------------------------------------------------
#include "ayther_element_scene.h"

#include <array>
#include <cstring>
#include <map>
#include <unordered_map>

namespace ayther {
namespace {

constexpr uint32_t kVramBytes = 0x10000;
constexpr uint32_t kMaxTiles = kVramBytes / 32u; // 2048, tile 0 left empty
constexpr uint32_t kMaxSprites = 80;

std::array<uint16_t, 16> grey_ramp() {
  std::array<uint16_t, 16> g{};
  for (uint16_t i = 0; i < 16; ++i) {
    const uint16_t c = static_cast<uint16_t>(i >> 1); // 0..7
    g[i] = static_cast<uint16_t>(c | (c << 3) | (c << 6));
  }
  return g;
}

const uint16_t *palette_of(const ElementPlacement &p) {
  if (p.cram_line)
    return p.cram_line;
  const ElementRecord &r = *p.record;
  if (p.palette_variant < r.palettes.size())
    return r.palettes[p.palette_variant].words;
  if (!r.palettes.empty())
    return r.palettes.front().words;
  return nullptr;
}

} // namespace

FrameView ElementScene::frame_view() const {
  FrameView fv{};
  fv.fb_pixels = framebuffer.data();
  fv.fb_width = width;
  fv.fb_height = height;
  fv.fb_pitch = width * 2u;
  fv.fb_format = 2; // RETRO_PIXEL_FORMAT_RGB565
  fv.scene = elements.data();
  fv.scene_count = static_cast<uint32_t>(elements.size());
  fv.scene_vram = vram.data();
  fv.scene_vram_size = vram.size();
  fv.scene_cram = cram.data();
  fv.scene_cram_size = cram.size();
  fv.scene_backdrop = backdrop_packed;
  fv.scene_dirty = 0;
  return fv;
}

bool build_element_scene(std::span<const ElementPlacement> placements,
                         uint32_t width, uint32_t height,
                         uint16_t backdrop_packed, ElementScene &out,
                         std::string *error) {
  auto fail = [&](const char *what) {
    if (error)
      *error = what;
    return false;
  };
  if (width == 0 || height == 0 || width > 4096 || height > 4096)
    return fail("canvas size out of range");
  out = ElementScene{};
  out.width = width;
  out.height = height;
  out.backdrop_packed = backdrop_packed;
  out.vram.assign(kVramBytes, 0);
  out.cram.assign(128, 0);
  // The base image is the backdrop in RGB565 (the scene path clears to the
  // backdrop anyway; this only matters if the renderer falls back).
  {
    const uint32_t r = (backdrop_packed & 7u) * 31u / 7u;
    const uint32_t g = ((backdrop_packed >> 3) & 7u) * 63u / 7u;
    const uint32_t b = ((backdrop_packed >> 6) & 7u) * 31u / 7u;
    out.framebuffer.assign(static_cast<size_t>(width) * height,
                           static_cast<uint16_t>((r << 11) | (g << 5) | b));
  }
  // Colour 0 of line 0 is the backdrop, as on the VDP.
  out.cram[0] = static_cast<uint8_t>(backdrop_packed);
  out.cram[1] = static_cast<uint8_t>(backdrop_packed >> 8);

  // Palettes → CRAM lines, by content.
  std::map<std::array<uint16_t, 16>, uint8_t> lines;
  auto line_for = [&](const uint16_t *words, uint8_t &line) {
    std::array<uint16_t, 16> key = grey_ramp();
    if (words)
      std::memcpy(key.data(), words, sizeof(uint16_t) * 16);
    key[0] = backdrop_packed; // index 0 is transparent; keep line 0 coherent
    const auto it = lines.find(key);
    if (it != lines.end()) {
      line = it->second;
      return true;
    }
    if (lines.size() >= 4)
      return false;
    line = static_cast<uint8_t>(lines.size());
    lines.emplace(key, line);
    for (uint32_t i = 1; i < 16; ++i) {
      const size_t off = (static_cast<size_t>(line) * 16u + i) * 2u;
      out.cram[off] = static_cast<uint8_t>(key[i]);
      out.cram[off + 1] = static_cast<uint8_t>(key[i] >> 8);
    }
    return true;
  };

  // Patterns → VRAM tiles, once per record.
  std::unordered_map<const ElementRecord *, uint16_t> base_of;
  uint32_t next_tile = 1;
  uint32_t sprites = 0;
  for (const ElementPlacement &p : placements) {
    if (!p.record)
      return fail("placement without a record");
    if (p.record->kind == ElementKind::Sprite)
      ++sprites;
  }
  if (sprites > kMaxSprites)
    return fail("more than 80 sprites in one scene");

  uint32_t sprite_rank = 0;
  for (const ElementPlacement &p : placements) {
    const ElementRecord &r = *p.record;
    if (r.pattern.size() != r.expected_pattern_bytes() || r.w_tiles == 0 ||
        r.h_tiles == 0)
      return fail("malformed record");
    uint16_t base = 0;
    if (const auto it = base_of.find(&r); it != base_of.end()) {
      base = it->second;
    } else {
      const uint32_t tiles = static_cast<uint32_t>(r.w_tiles) * r.h_tiles;
      if (next_tile + tiles > kMaxTiles)
        return fail("more than 2047 pattern tiles in one scene");
      base = static_cast<uint16_t>(next_tile);
      const size_t at = static_cast<size_t>(base) * 32u;
      for (size_t o = 0; o < r.pattern.size(); ++o)
        out.vram[(at + o) ^ 1u] = r.pattern[o]; // bus → host order
      next_tile += tiles;
      base_of.emplace(&r, base);
    }
    uint8_t line = 0;
    if (!line_for(palette_of(p), line))
      return fail("more than four distinct palettes in one scene");
    SceneElement e{};
    e.hash = r.hash;
    e.x = static_cast<int16_t>(p.x);
    e.y = static_cast<int16_t>(p.y);
    e.w = static_cast<uint8_t>(r.w_tiles * 8u);
    e.h = static_cast<uint8_t>(r.h_tiles * 8u);
    e.pattern = base;
    e.palette = line;
    e.flips = static_cast<uint8_t>(p.flips & 3u);
    e.priority = 0;
    if (r.kind == ElementKind::Sprite) {
      e.layer = 3;
      e.slot = static_cast<uint8_t>(sprite_rank);
      // Later placements on top: the front of the link chain is rank 0.
      e.chain = static_cast<uint8_t>(sprites - 1u - sprite_rank);
      ++sprite_rank;
    } else {
      e.layer = 1; // plane A cell, beneath the sprites
      e.slot = 0xFF;
      e.chain = 0xFF;
    }
    out.elements.push_back(e);
  }
  return true;
}

} // namespace ayther
