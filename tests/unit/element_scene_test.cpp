// ---------------------------------------------------------------------------
// element_scene_test.cpp — spec 007 (RF-5.1): the synthetic frame built from
// element records, checked on the CPU (no GPU): VRAM placement in host order,
// palette-to-line assignment, depth order and the limits of one scene.
// ---------------------------------------------------------------------------
#include "ayther_element_scene.h"

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

ayther::ElementRecord sprite(uint8_t w, uint8_t h, uint8_t fill,
                             uint16_t colour) {
  ayther::ElementRecord r;
  r.kind = ayther::ElementKind::Sprite;
  r.w_tiles = w;
  r.h_tiles = h;
  r.pattern.assign(static_cast<size_t>(w) * h * 32u, fill);
  ayther::ElementPaletteVariant pv;
  for (uint16_t &word : pv.words)
    word = colour;
  r.palettes.push_back(pv);
  return r;
}
} // namespace

int main() try {
  using namespace ayther;
  ElementRecord a = sprite(2, 1, 0x11, 0x007);
  a.pattern[0] = 0x12; // bus byte 0
  a.pattern[1] = 0x34; // bus byte 1
  ElementRecord b = sprite(1, 1, 0x22, 0x1C0);
  ElementRecord t;
  t.kind = ElementKind::PlaneTile;
  t.pattern.assign(32, 0x33);
  t.palettes = a.palettes; // same palette as `a` → same line

  const ElementPlacement pl[] = {
      {&a, 0, 0, 0, 0, nullptr},
      {&b, 4, 4, 0, 1, nullptr},
      {&t, 8, 8, 0, 0, nullptr},
      {&a, 16, 0, 0, 0, nullptr}, // the same record again: one VRAM copy
  };
  ElementScene scene;
  std::string err;
  check(build_element_scene(pl, 64, 32, 0x038, scene, &err) && err.empty(),
        "a valid scene builds");
  check(scene.vram.size() == 0x10000 && scene.cram.size() == 128 &&
            scene.elements.size() == 4 && scene.framebuffer.size() == 64u * 32u,
        "buffers have the VDP sizes and one element per placement");

  const SceneElement &ea = scene.elements[0];
  check(ea.layer == 3 && ea.pattern == 1 && ea.w == 16 && ea.h == 8,
        "the first sprite is at tile 1 with its size in px");
  check(scene.vram[(1u * 32u) ^ 1u] == 0x12 &&
            scene.vram[(1u * 32u + 1u) ^ 1u] == 0x34,
        "patterns are stored in host (word-swapped) order");
  check(scene.elements[3].pattern == ea.pattern,
        "a record placed twice shares its VRAM tiles");
  check(scene.elements[1].pattern == 3 && scene.elements[1].flips == 1,
        "the next record follows the first one's tiles; flips pass through");
  check(scene.elements[2].layer == 1 && scene.elements[2].chain == 0xFF,
        "a plane tile is a plane A cell");
  check(ea.palette == scene.elements[2].palette &&
            ea.palette != scene.elements[1].palette,
        "equal palettes share a CRAM line, different ones do not");
  const size_t off =
      (static_cast<size_t>(scene.elements[1].palette) * 16u + 5u) * 2u;
  check((scene.cram[off] | (scene.cram[off + 1] << 8)) == 0x1C0,
        "the line holds the palette's words");
  check((scene.cram[0] | (scene.cram[1] << 8)) == 0x038,
        "CRAM colour 0 of line 0 is the backdrop");
  check(scene.elements[0].chain > scene.elements[1].chain &&
            scene.elements[1].chain > scene.elements[3].chain &&
            scene.elements[3].chain == 0,
        "later sprites are further front in the chain");

  const FrameView fv = scene.frame_view();
  check(fv.scene == scene.elements.data() && fv.scene_count == 4 &&
            fv.scene_vram_size == 0x10000 && fv.scene_dirty == 0 &&
            fv.fb_width == 64 && fv.fb_format == 2 &&
            fv.scene_backdrop == 0x038,
        "the FrameView points into the scene, composed and not dirty");

  // Limits.
  std::vector<ElementRecord> many;
  many.reserve(5);
  for (uint16_t i = 0; i < 5; ++i)
    many.push_back(sprite(1, 1, 0x11, static_cast<uint16_t>(i + 1)));
  std::vector<ElementPlacement> five;
  for (const ElementRecord &r : many)
    five.push_back({&r, 0, 0, 0, 0, nullptr});
  check(!build_element_scene(five, 16, 16, 0, scene, &err) && !err.empty(),
        "a fifth distinct palette is refused");
  std::vector<ElementPlacement> eighty_one(81, {&b, 0, 0, 0, 0, nullptr});
  check(!build_element_scene(eighty_one, 16, 16, 0, scene, &err),
        "more than 80 sprites are refused");
  ElementRecord big = sprite(4, 4, 0x11, 0x007);
  std::vector<ElementRecord> bigs(129, big);
  std::vector<ElementPlacement> tiles;
  for (const ElementRecord &r : bigs)
    tiles.push_back({&r, 0, 0, 0, 0, nullptr});
  // 129 × 16 tiles = 2064 > 2047 (and > 80 sprites); use plane tiles instead.
  ElementRecord pt = t;
  std::vector<ElementRecord> cells(2048, pt);
  std::vector<ElementPlacement> cell_pl;
  for (const ElementRecord &r : cells)
    cell_pl.push_back({&r, 0, 0, 0, 0, nullptr});
  check(!build_element_scene(cell_pl, 16, 16, 0, scene, &err),
        "more than 2047 pattern tiles are refused");
  ElementRecord broken = b;
  broken.pattern.resize(3);
  const ElementPlacement bad[] = {{&broken, 0, 0, 0, 0, nullptr}};
  check(!build_element_scene(bad, 16, 16, 0, scene, &err),
        "a malformed record is refused");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
