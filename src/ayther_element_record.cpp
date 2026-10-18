// ---------------------------------------------------------------------------
// ayther_element_record.cpp — the element record stream, the identity
// reproducers and the CPU reconstruction. See ayther_element_record.h.
//
// Stream layout (little-endian):
//   magic[4] "AYEI" · u32 schema · u32 sprite_hash_algo · u32
//   plane_tile_hash_algo u8 byte_order · u8[3] reserved · u64 generation · u64
//   created_unix u32 record_count record_count × (u8 kind · u32 body_len ·
//   body) u32 crc32 (IEEE, over every byte before it)
// Sprite body:
//   u64 hash · u8 w_tiles · u8 h_tiles · u8 flips_seen · u8 priority_seen
//   u8[w*h*32] pattern · palettes · provenance · u64 first_seen · u64 last_seen
// Tile body (PlaneTile, WindowTile):
//   u64 hash · u8 palette_line · u8[32] pattern · u8 planes_seen
//   u8 flips_seen · u8 priority_seen · palettes · provenance · u64 · u64
// palettes:   u8 n · n × (u8 line · u16[16] words · u64 line_sig · u32 count)
// provenance: u16 n · n × (u64 take_id · u32 first · u32 last · u32 count)
// ---------------------------------------------------------------------------
#include "ayther_element_record.h"

#include "ayther_core_ffi.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace ayther {
namespace {

constexpr std::array<uint8_t, 4> kMagic = {'A', 'Y', 'E', 'I'};
constexpr uint64_t kFnvSeed = 0x14650FB0739D0383ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

uint32_t crc32_ieee(const uint8_t *data, size_t n) noexcept {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k)
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[i] = c;
    }
    return t;
  }();
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i)
    c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

struct Writer {
  std::vector<uint8_t> &out;
  void u8(uint8_t v) { out.push_back(v); }
  void u16(uint16_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
  }
  void u32(uint32_t v) {
    for (int i = 0; i < 4; ++i)
      out.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  void u64(uint64_t v) {
    for (int i = 0; i < 8; ++i)
      out.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  void bytes(const uint8_t *p, size_t n) { out.insert(out.end(), p, p + n); }
};

struct Reader {
  std::span<const uint8_t> in;
  size_t pos = 0;
  [[nodiscard]] size_t left() const noexcept { return in.size() - pos; }
  bool u8(uint8_t &v) {
    if (left() < 1)
      return false;
    v = in[pos++];
    return true;
  }
  bool u16(uint16_t &v) {
    if (left() < 2)
      return false;
    v = static_cast<uint16_t>(in[pos] | (in[pos + 1] << 8));
    pos += 2;
    return true;
  }
  bool u32(uint32_t &v) {
    if (left() < 4)
      return false;
    v = 0;
    for (int i = 0; i < 4; ++i)
      v |= static_cast<uint32_t>(in[pos + i]) << (8 * i);
    pos += 4;
    return true;
  }
  bool u64(uint64_t &v) {
    if (left() < 8)
      return false;
    v = 0;
    for (int i = 0; i < 8; ++i)
      v |= static_cast<uint64_t>(in[pos + i]) << (8 * i);
    pos += 8;
    return true;
  }
  bool bytes(uint8_t *p, size_t n) {
    if (left() < n)
      return false;
    std::memcpy(p, in.data() + pos, n);
    pos += n;
    return true;
  }
};

bool kind_known(uint8_t k) noexcept {
  return k == static_cast<uint8_t>(ElementKind::Sprite) ||
         k == static_cast<uint8_t>(ElementKind::PlaneTile) ||
         k == static_cast<uint8_t>(ElementKind::WindowTile);
}

bool validate(const ElementRecord &r, const ElementRecordLimits &limits,
              std::string *error) {
  auto fail = [&](const char *what) {
    if (error)
      *error = what;
    return false;
  };
  if (!kind_known(static_cast<uint8_t>(r.kind)))
    return fail("unknown element kind");
  if (r.kind == ElementKind::Sprite) {
    if (r.w_tiles < 1 || r.w_tiles > 4 || r.h_tiles < 1 || r.h_tiles > 4)
      return fail("sprite tiles out of range 1-4");
  } else if (r.w_tiles != 1 || r.h_tiles != 1) {
    return fail("plane tile must be 1x1");
  }
  if (r.pattern.size() != r.expected_pattern_bytes())
    return fail("pattern size does not match tiles");
  if (r.palettes.size() > limits.max_palettes)
    return fail("too many palette variants");
  if (r.provenance.size() > limits.max_provenance)
    return fail("too many provenance runs");
  return true;
}

void write_body(Writer &w, const ElementRecord &r) {
  w.u64(r.hash);
  if (r.kind == ElementKind::Sprite) {
    w.u8(r.w_tiles);
    w.u8(r.h_tiles);
    w.u8(r.flips_seen);
    w.u8(r.priority_seen);
    w.bytes(r.pattern.data(), r.pattern.size());
  } else {
    w.u8(r.palette_line);
    w.bytes(r.pattern.data(), r.pattern.size());
    w.u8(r.planes_seen);
    w.u8(r.flips_seen);
    w.u8(r.priority_seen);
  }
  w.u8(static_cast<uint8_t>(r.palettes.size()));
  for (const ElementPaletteVariant &p : r.palettes) {
    w.u8(p.line);
    for (uint16_t word : p.words)
      w.u16(word);
    w.u64(p.line_sig);
    w.u32(p.count);
  }
  w.u16(static_cast<uint16_t>(r.provenance.size()));
  for (const ElementProvenance &p : r.provenance) {
    w.u64(p.take_id);
    w.u32(p.first_frame);
    w.u32(p.last_frame);
    w.u32(p.count);
  }
  w.u64(r.first_seen_unix);
  w.u64(r.last_seen_unix);
}

bool read_body(Reader &rd, ElementKind kind, ElementRecord &r) {
  r.kind = kind;
  if (!rd.u64(r.hash))
    return false;
  if (kind == ElementKind::Sprite) {
    if (!rd.u8(r.w_tiles) || !rd.u8(r.h_tiles) || !rd.u8(r.flips_seen) ||
        !rd.u8(r.priority_seen))
      return false;
    if (r.w_tiles < 1 || r.w_tiles > 4 || r.h_tiles < 1 || r.h_tiles > 4)
      return false;
    r.pattern.resize(r.expected_pattern_bytes());
    if (!rd.bytes(r.pattern.data(), r.pattern.size()))
      return false;
  } else {
    r.w_tiles = r.h_tiles = 1;
    r.pattern.resize(32);
    if (!rd.u8(r.palette_line) || !rd.bytes(r.pattern.data(), 32) ||
        !rd.u8(r.planes_seen) || !rd.u8(r.flips_seen) ||
        !rd.u8(r.priority_seen))
      return false;
  }
  uint8_t np = 0;
  if (!rd.u8(np))
    return false;
  r.palettes.resize(np);
  for (ElementPaletteVariant &p : r.palettes) {
    if (!rd.u8(p.line))
      return false;
    for (uint16_t &word : p.words)
      if (!rd.u16(word))
        return false;
    if (!rd.u64(p.line_sig) || !rd.u32(p.count))
      return false;
  }
  uint16_t nv = 0;
  if (!rd.u16(nv))
    return false;
  r.provenance.resize(nv);
  for (ElementProvenance &p : r.provenance)
    if (!rd.u64(p.take_id) || !rd.u32(p.first_frame) || !rd.u32(p.last_frame) ||
        !rd.u32(p.count))
      return false;
  return rd.u64(r.first_seen_unix) && rd.u64(r.last_seen_unix);
}

void palette_rgb(const uint16_t *line, int idx, uint8_t *d) noexcept {
  // Without a palette: a grey ramp in CRAM precision, the same words
  // build_element_scene uploads, so the CPU and the GPU agree.
  const uint16_t c = static_cast<uint16_t>((idx & 15) >> 1);
  const uint16_t v =
      line ? line[idx & 15] : static_cast<uint16_t>(c | (c << 3) | (c << 6));
  element_color_rgb(v, d);
}

const uint16_t *line_for(const ElementRecord &r, uint8_t variant,
                         const uint16_t *override_line) noexcept {
  if (override_line)
    return override_line;
  if (variant < r.palettes.size())
    return r.palettes[variant].words;
  if (!r.palettes.empty())
    return r.palettes.front().words;
  return nullptr;
}

} // namespace

bool write_element_records(const ElementRecordSet &set,
                           std::vector<uint8_t> &out,
                           const ElementRecordLimits &limits,
                           std::string *error) {
  if (set.records.size() > limits.max_records) {
    if (error)
      *error = "too many records";
    return false;
  }
  std::vector<uint8_t> buf;
  Writer w{buf};
  w.bytes(kMagic.data(), kMagic.size());
  w.u32(kElementRecordSchema);
  w.u32(set.sprite_hash_algo);
  w.u32(set.plane_tile_hash_algo);
  w.u8(kElementByteOrderBus);
  w.u8(0);
  w.u8(0);
  w.u8(0);
  w.u64(set.generation);
  w.u64(set.created_unix);
  w.u32(static_cast<uint32_t>(set.records.size()));
  std::vector<uint8_t> body;
  for (const ElementRecord &r : set.records) {
    if (!validate(r, limits, error))
      return false;
    body.clear();
    Writer bw{body};
    write_body(bw, r);
    if (body.size() > limits.max_body_bytes) {
      if (error)
        *error = "record body over the limit";
      return false;
    }
    w.u8(static_cast<uint8_t>(r.kind));
    w.u32(static_cast<uint32_t>(body.size()));
    w.bytes(body.data(), body.size());
    if (buf.size() + 4 > limits.max_stream_bytes) {
      if (error)
        *error = "stream over the size limit";
      return false;
    }
  }
  w.u32(crc32_ieee(buf.data(), buf.size()));
  out.swap(buf);
  return true;
}

bool read_element_records(std::span<const uint8_t> bytes, ElementRecordSet &out,
                          ElementRecordReadReport *report,
                          const ElementRecordLimits &limits) {
  ElementRecordReadReport local;
  ElementRecordReadReport &rep = report ? *report : local;
  rep = ElementRecordReadReport{};
  auto fail = [&](const char *what) {
    rep.error = what;
    return false;
  };
  if (bytes.size() < kMagic.size() + 4 * 4 + 16 + 4 + 4)
    return fail("stream too short");
  if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
    return fail("bad magic");
  const size_t body_end = bytes.size() - 4;
  const uint32_t stored_crc =
      static_cast<uint32_t>(bytes[body_end]) |
      (static_cast<uint32_t>(bytes[body_end + 1]) << 8) |
      (static_cast<uint32_t>(bytes[body_end + 2]) << 16) |
      (static_cast<uint32_t>(bytes[body_end + 3]) << 24);
  rep.crc_ok = crc32_ieee(bytes.data(), body_end) == stored_crc;
  Reader rd{bytes.first(body_end), kMagic.size()};
  uint8_t reserved = 0;
  if (!rd.u32(out.schema) || !rd.u32(out.sprite_hash_algo) ||
      !rd.u32(out.plane_tile_hash_algo) || !rd.u8(out.byte_order) ||
      !rd.u8(reserved) || !rd.u8(reserved) || !rd.u8(reserved) ||
      !rd.u64(out.generation) || !rd.u64(out.created_unix))
    return fail("truncated header");
  if (out.schema > kElementRecordSchema)
    return fail("schema newer than this reader");
  if (out.byte_order != kElementByteOrderBus)
    return fail("unknown byte order");
  uint32_t count = 0;
  if (!rd.u32(count))
    return fail("truncated header");
  if (count > limits.max_records)
    return fail("too many records");
  out.records.clear();
  out.records.reserve(std::min<uint32_t>(count, 65536u));
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t kind = 0;
    uint32_t len = 0;
    if (!rd.u8(kind) || !rd.u32(len))
      return fail("truncated record");
    if (len > limits.max_body_bytes)
      return fail("record body over the limit");
    if (rd.left() < len)
      return fail("truncated record body");
    if (!kind_known(kind)) {
      rd.pos += len;
      ++rep.skipped_unknown_kind;
      continue;
    }
    Reader body{rd.in.subspan(rd.pos, len), 0};
    ElementRecord r;
    if (!read_body(body, static_cast<ElementKind>(kind), r) || body.left() != 0)
      return fail("malformed record body");
    rd.pos += len;
    out.records.push_back(std::move(r));
  }
  if (rd.left() != 0)
    return fail("trailing bytes");
  if (!rep.crc_ok)
    return fail("crc mismatch");
  return true;
}

void element_color_rgb(uint16_t packed, uint8_t *rgb) noexcept {
  // 3-bit channel → the core's "normal" level ×2 (0..14) → RGB565 as the
  // core packs it → 888 replicating the high bits. Byte-identical to
  // VkIndexedPlane::genesis_color_rgba and to the emulator's framebuffer.
  auto ch = [](unsigned c3, bool six) -> uint8_t {
    const unsigned c4 = (c3 & 7u) << 1;
    if (six) {
      const unsigned c6 = (c4 << 2) | (c4 >> 2);
      return static_cast<uint8_t>((c6 << 2) | (c6 >> 4));
    }
    const unsigned c5 = (c4 << 1) | (c4 >> 3);
    return static_cast<uint8_t>((c5 << 3) | (c5 >> 2));
  };
  rgb[0] = ch(packed, false);
  rgb[1] = ch(packed >> 3, true);
  rgb[2] = ch(packed >> 6, false);
}

uint64_t plane_tile_hash(const uint8_t *bus_bytes32, uint8_t pal) noexcept {
  uint64_t h = kFnvSeed;
  for (uint32_t b = 0; b < 32u; ++b) {
    h ^= bus_bytes32[b];
    h *= kFnvPrime;
  }
  h ^= static_cast<uint64_t>(pal & 3u);
  h *= kFnvPrime;
  return h;
}

uint64_t sprite_pattern_hash(const uint8_t *bus_bytes, size_t n) noexcept {
  if (!bus_bytes || n == 0 || (n % 32u) != 0)
    return 0;
  std::vector<uint8_t> host(n);
  for (size_t i = 0; i < n; ++i)
    host[i] = bus_bytes[i ^ 1u];
  return ayther_sprite_pattern_hash(host.data(), static_cast<uint32_t>(n));
}

void decode_tile_rgba(const uint8_t *bus_bytes32, const uint16_t *cram_line16,
                      bool hflip, bool vflip, bool index0_transparent,
                      uint8_t *out_rgba256) noexcept {
  for (int row = 0; row < 8; ++row)
    for (int col = 0; col < 8; ++col) {
      const int sx = hflip ? 7 - col : col;
      const int sy = vflip ? 7 - row : row;
      const uint8_t byte = bus_bytes32[sy * 4 + (sx >> 1)];
      const int idx = (sx & 1) ? (byte & 0xF) : (byte >> 4);
      uint8_t *d = out_rgba256 + (static_cast<size_t>(row) * 8 + col) * 4;
      if (idx == 0 && index0_transparent) {
        d[0] = d[1] = d[2] = d[3] = 0;
        continue;
      }
      palette_rgb(cram_line16, idx, d);
      d[3] = 0xFF;
    }
}

bool render_element_rgba(const ElementRecord &record,
                         const ElementRenderOptions &opts, ElementImage &out) {
  if (record.pattern.size() != record.expected_pattern_bytes() ||
      record.w_tiles == 0 || record.h_tiles == 0)
    return false;
  const uint32_t w = record.w_tiles * 8u, h = record.h_tiles * 8u;
  out.w = w;
  out.h = h;
  out.rgba.assign(static_cast<size_t>(w) * h * 4u, 0);
  const uint16_t *line = line_for(record, opts.palette_variant, opts.cram_line);
  const bool hf = (opts.flips & 1u) != 0, vf = (opts.flips & 2u) != 0;
  uint8_t tile[256];
  for (uint32_t cy = 0; cy < record.h_tiles; ++cy)
    for (uint32_t cx = 0; cx < record.w_tiles; ++cx) {
      const uint32_t t =
          sprite_tile_index(record.w_tiles, record.h_tiles, cx, cy, hf, vf);
      decode_tile_rgba(record.pattern.data() + static_cast<size_t>(t) * 32u,
                       line, hf, vf, opts.index0_transparent, tile);
      for (uint32_t row = 0; row < 8; ++row)
        std::memcpy(out.rgba.data() +
                        ((static_cast<size_t>(cy) * 8 + row) * w + cx * 8) * 4,
                    tile + row * 32, 32);
    }
  return true;
}

bool compose_elements_rgba(std::span<const ElementPlacement> placements,
                           uint32_t w, uint32_t h, uint16_t backdrop_packed,
                           bool transparent_backdrop, ElementImage &out) {
  out.w = w;
  out.h = h;
  out.rgba.assign(static_cast<size_t>(w) * h * 4u, 0);
  if (!transparent_backdrop) {
    uint8_t bd[3];
    element_color_rgb(backdrop_packed, bd);
    const uint8_t r = bd[0], g = bd[1], b = bd[2];
    for (size_t i = 0; i < out.rgba.size(); i += 4) {
      out.rgba[i] = r;
      out.rgba[i + 1] = g;
      out.rgba[i + 2] = b;
      out.rgba[i + 3] = 0xFF;
    }
  }
  ElementImage img;
  for (const ElementPlacement &p : placements) {
    if (!p.record)
      return false;
    ElementRenderOptions o;
    o.palette_variant = p.palette_variant;
    o.flips = p.flips;
    o.cram_line = p.cram_line;
    o.index0_transparent = true;
    if (!render_element_rgba(*p.record, o, img))
      return false;
    for (uint32_t y = 0; y < img.h; ++y) {
      const int64_t dy = static_cast<int64_t>(p.y) + y;
      if (dy < 0 || dy >= static_cast<int64_t>(h))
        continue;
      for (uint32_t x = 0; x < img.w; ++x) {
        const int64_t dx = static_cast<int64_t>(p.x) + x;
        if (dx < 0 || dx >= static_cast<int64_t>(w))
          continue;
        const uint8_t *s =
            img.rgba.data() + (static_cast<size_t>(y) * img.w + x) * 4;
        if (s[3] == 0)
          continue;
        std::memcpy(
            out.rgba.data() +
                (static_cast<size_t>(dy) * w + static_cast<size_t>(dx)) * 4,
            s, 4);
      }
    }
  }
  return true;
}

} // namespace ayther
