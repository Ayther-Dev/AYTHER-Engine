// ---------------------------------------------------------------------------
// element_record_test.cpp — spec 007: the element record stream, the identity
// reproducers and the CPU reconstruction, with no ROM, GPU or session.
//
// The known answers are the published identity vectors of
// core/src/identity_kat.rs (synthetic VRAM byte i = (i*37+11) & 0xFF), so a
// drift between the C++ reproducers and the Rust identities fails here.
// ---------------------------------------------------------------------------
#include "ayther_element_record.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

std::vector<uint8_t> synthetic_vram(size_t n) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i)
    v[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
  return v;
}

/// Bus view of host bytes [off, off+n): bus[o] = host[(off + o) ^ 1].
std::vector<uint8_t> bus_slice(const std::vector<uint8_t> &host, size_t off,
                               size_t n) {
  std::vector<uint8_t> b(n);
  for (size_t o = 0; o < n; ++o) {
    const size_t i = (off + o) ^ 1u;
    b[o] = i < host.size() ? host[i] : 0;
  }
  return b;
}

ayther::ElementRecord sprite_record(uint8_t w, uint8_t h) {
  ayther::ElementRecord r;
  r.kind = ayther::ElementKind::Sprite;
  r.w_tiles = w;
  r.h_tiles = h;
  r.pattern.assign(static_cast<size_t>(w) * h * 32u, 0);
  return r;
}
} // namespace

int main() try {
  using namespace ayther;
  constexpr uint8_t kFull = 239; // 3-bit 7 at the framebuffer level
  const std::vector<uint8_t> vram = synthetic_vram(4096);

  // ---- identities: the published vectors --------------------------------
  {
    const auto bus = bus_slice(vram, 0, 32);
    check(sprite_pattern_hash(bus.data(), bus.size()) == 0x41573b4753eaaeb4ULL,
          "sprite hash of tile 0 (1x1) matches the published vector");
    const auto bus16 = bus_slice(vram, 3 * 32, 16 * 32);
    check(sprite_pattern_hash(bus16.data(), bus16.size()) ==
              0xadba9fc9e8c721a7ULL,
          "sprite hash of tiles 3..18 (4x4) matches the published vector");
    const auto tile1 = bus_slice(vram, 32, 32);
    check(plane_tile_hash(tile1.data(), 0) == 0x98d36be2cd04adb9ULL,
          "plane tile hash of pattern 1, line 0 matches the published vector");
    check(plane_tile_hash(tile1.data(), 1) != plane_tile_hash(tile1.data(), 0),
          "the palette line is part of the plane tile hash");
    check(sprite_pattern_hash(bus.data(), 31) == 0,
          "a byte count that is not a multiple of 32 hashes to 0");
    // Swapping the pairs must change the sprite hash (the vector is not
    // symmetric): the reproducer really undoes the bus order.
    std::vector<uint8_t> swapped(bus.size());
    for (size_t i = 0; i < bus.size(); ++i)
      swapped[i] = bus[i ^ 1u];
    check(sprite_pattern_hash(swapped.data(), swapped.size()) !=
              0x41573b4753eaaeb4ULL,
          "host-order bytes passed as bus order give another sprite hash");
  }

  // ---- colour: the framebuffer level, as the GPU path --------------------
  {
    uint8_t c[3];
    element_color_rgb(0x1FF, c);
    check(c[0] == kFull && c[1] == kFull && c[2] == kFull,
          "white is the core's framebuffer level (239), as the Vulkan path");
    element_color_rgb(0x0A3, c);
    check(c[0] == 99 && c[1] == 138 && c[2] == 66,
          "a mixed colour matches VkIndexedPlane::genesis_color_rgba");
  }

  // ---- decode: an asymmetric tile, flips, transparency ------------------
  {
    uint8_t tile[32] = {};
    tile[0] = 0x12;  // row 0: pixel 0 = index 1, pixel 1 = index 2
    tile[28] = 0x30; // row 7: pixel 0 = index 3
    uint16_t line[16] = {};
    line[1] = 0x007; // red
    line[2] = 0x038; // green
    line[3] = 0x1C0; // blue
    uint8_t rgba[256];
    decode_tile_rgba(tile, line, false, false, true, rgba);
    check(rgba[0] == kFull && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255,
          "pixel (0,0) is index 1 = red");
    check(rgba[4] == 0 && rgba[5] == kFull && rgba[6] == 0,
          "pixel (1,0) is index 2 = green (low nibble)");
    check(rgba[7 * 32 + 2] == kFull && rgba[7 * 32 + 3] == 255,
          "pixel (0,7) is index 3 = blue");
    check(rgba[8 + 3] == 0, "index 0 is transparent");
    decode_tile_rgba(tile, line, true, false, true, rgba);
    check(rgba[7 * 4] == kFull && rgba[7 * 4 + 1] == 0,
          "hflip moves pixel (0,0) to (7,0)");
    decode_tile_rgba(tile, line, false, true, true, rgba);
    check(rgba[2] == kFull && rgba[3] == 255,
          "vflip moves row 7 to row 0 (blue at (0,0))");
    decode_tile_rgba(tile, line, false, false, false, rgba);
    check(rgba[8 + 3] == 255 && rgba[8] == 0,
          "with index 0 opaque it takes the line's colour 0");
  }

  // ---- sprite placement: column-major tiles and grid mirroring ----------
  {
    check(sprite_tile_index(2, 2, 0, 1, false, false) == 1 &&
              sprite_tile_index(2, 2, 1, 0, false, false) == 2,
          "sprite tiles are column-major (tile 1 under tile 0)");
    check(sprite_tile_index(2, 2, 0, 0, true, false) == 2 &&
              sprite_tile_index(2, 2, 0, 0, false, true) == 1,
          "flips mirror the tile grid");
    ElementRecord r = sprite_record(2, 2);
    r.pattern[2 * 32] = 0x10; // tile 2 (column 1, row 0): pixel (0,0) index 1
    ElementPaletteVariant pv;
    pv.words[1] = 0x1FF;
    r.palettes.push_back(pv);
    ElementImage img;
    check(render_element_rgba(r, {}, img) && img.w == 16 && img.h == 16,
          "a 2x2 sprite renders at 16x16");
    check(img.rgba[(0 * 16 + 8) * 4 + 3] == 255 &&
              img.rgba[(0 * 16 + 8) * 4] == kFull,
          "tile 2 lands at screen column 1, row 0");
    ElementRenderOptions hf;
    hf.flips = 1;
    render_element_rgba(r, hf, img);
    check(img.rgba[(0 * 16 + 7) * 4 + 3] == 255,
          "hflip: the pixel moves to x = 7 (tile 2 → column 0, mirrored)");
    r.pattern.pop_back();
    check(!render_element_rgba(r, {}, img),
          "a pattern that does not match the tiles is refused");
  }

  // ---- compose -----------------------------------------------------------
  {
    ElementRecord a = sprite_record(1, 1);
    a.pattern[0] = 0x11; // row 0: pixels 0-3 index 1, the rest index 0
    a.pattern[1] = 0x11;
    ElementPaletteVariant pv;
    pv.words[1] = 0x007;
    a.palettes.push_back(pv);
    // At x = -2, pixels 2 and 3 land on canvas x = 0 and 1.
    const ElementPlacement pl[] = {{&a, -2, 0, 0, 0, nullptr}};
    ElementImage img;
    check(compose_elements_rgba(pl, 8, 8, 0x1C0, false, img) && img.w == 8,
          "compose clips a placement at the canvas edge");
    check(img.rgba[0 * 4] == kFull && img.rgba[1 * 4] == kFull &&
              img.rgba[4 * 4 + 2] == kFull && img.rgba[4 * 4] == 0,
          "pixels inside are red, the rest is the backdrop (blue)");
  }

  // ---- stream round-trip, unknown kinds, limits, crc ---------------------
  {
    ElementRecordSet set;
    set.sprite_hash_algo = 1;
    set.generation = 7;
    set.created_unix = 1760000000;
    ElementRecord s = sprite_record(2, 1);
    s.hash = 0x1122334455667788ULL;
    s.flips_seen = kElementFlipsNone | kElementFlipsH;
    s.priority_seen = kElementPriorityLow;
    for (size_t i = 0; i < s.pattern.size(); ++i)
      s.pattern[i] = static_cast<uint8_t>(i);
    ElementPaletteVariant pv;
    pv.line = 2;
    for (int i = 0; i < 16; ++i)
      pv.words[i] = static_cast<uint16_t>(i * 33);
    pv.line_sig = 0xABCDEF;
    pv.count = 3;
    s.palettes.push_back(pv);
    s.provenance.push_back({0xAA55AA55AA55AA55ULL, 10, 20, 11});
    s.first_seen_unix = 1;
    s.last_seen_unix = 2;
    set.records.push_back(s);
    ElementRecord t;
    t.kind = ElementKind::WindowTile;
    t.hash = 42;
    t.palette_line = 3;
    t.planes_seen = kElementPlaneW;
    t.pattern.assign(32, 0x5A);
    set.records.push_back(t);

    std::vector<uint8_t> bytes;
    std::string err;
    check(write_element_records(set, bytes, {}, &err) && err.empty(),
          "a valid set serializes");
    ElementRecordSet back;
    ElementRecordReadReport rep;
    check(read_element_records(bytes, back, &rep) && rep.crc_ok,
          "the stream reads back with a valid crc");
    check(back.records.size() == 2 && back.generation == 7 &&
              back.sprite_hash_algo == 1 &&
              back.plane_tile_hash_algo == kPlaneTileHashAlgo,
          "header fields round-trip");
    const ElementRecord &rs = back.records[0];
    check(rs.kind == ElementKind::Sprite && rs.hash == s.hash &&
              rs.w_tiles == 2 && rs.h_tiles == 1 && rs.pattern == s.pattern &&
              rs.flips_seen == s.flips_seen && rs.palettes.size() == 1 &&
              rs.palettes[0].line == 2 && rs.palettes[0].words[15] == 15 * 33 &&
              rs.palettes[0].line_sig == 0xABCDEF &&
              rs.provenance.size() == 1 &&
              rs.provenance[0].take_id == 0xAA55AA55AA55AA55ULL &&
              rs.provenance[0].last_frame == 20 && rs.last_seen_unix == 2,
          "the sprite record round-trips field by field");
    const ElementRecord &rt = back.records[1];
    check(rt.kind == ElementKind::WindowTile && rt.palette_line == 3 &&
              rt.planes_seen == kElementPlaneW && rt.pattern[5] == 0x5A,
          "the window tile record round-trips");

    // An unknown kind (a future audio record) is skipped, not an error.
    std::vector<uint8_t> with_unknown(bytes.begin(), bytes.end() - 4);
    // patch record_count (offset 4+12+4+16 = 36) from 2 to 3 and append a
    // kind-16 record of 3 bytes, then re-crc by rewriting through the API is
    // not possible, so recompute the crc here with the same polynomial.
    with_unknown[36] = 3;
    with_unknown.push_back(16);
    with_unknown.push_back(3);
    with_unknown.push_back(0);
    with_unknown.push_back(0);
    with_unknown.push_back(0);
    with_unknown.push_back(1);
    with_unknown.push_back(2);
    with_unknown.push_back(3);
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t b : with_unknown) {
      crc ^= b;
      for (int k = 0; k < 8; ++k)
        crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
    }
    crc ^= 0xFFFFFFFFu;
    for (int i = 0; i < 4; ++i)
      with_unknown.push_back(static_cast<uint8_t>(crc >> (8 * i)));
    ElementRecordSet skipped;
    check(read_element_records(with_unknown, skipped, &rep) &&
              rep.skipped_unknown_kind == 1 && skipped.records.size() == 2,
          "an unknown kind is skipped and counted");

    std::vector<uint8_t> corrupt = bytes;
    corrupt[40] ^= 0xFF;
    check(!read_element_records(corrupt, back, &rep) && !rep.crc_ok,
          "a flipped byte is caught by the crc");
    std::vector<uint8_t> truncated(bytes.begin(), bytes.end() - 10);
    check(!read_element_records(truncated, back, &rep),
          "a truncated stream is refused");

    ElementRecordSet bad = set;
    bad.records[0].pattern.resize(10);
    check(!write_element_records(bad, bytes, {}, &err) && !err.empty(),
          "a record with the wrong pattern size is refused on write");
    ElementRecordLimits tight;
    tight.max_palettes = 0;
    check(!write_element_records(set, bytes, tight, &err),
          "the palette-variant limit is enforced on write");
    tight = {};
    tight.max_records = 1;
    check(!write_element_records(set, bytes, tight, &err),
          "the record-count limit is enforced on write");
  }

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
