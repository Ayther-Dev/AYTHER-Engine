#include "cross_pack.h"
#include "frame_dump.h"
#include "native_config.h"

#include <ayther/ayther_core_ffi.h>
#include <ayther/ayther_session.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <string_view>

extern "C" bool ayther_test_public_key_hex(unsigned char seed_byte,
                                           char *out_buf, size_t cap);
extern "C" bool ayther_test_pack_builder_finish_signed_as(
    AytherPackBuilder *builder, const char *out_path, const char *key_id,
    unsigned char seed_byte, char *error_buffer, size_t error_capacity);

namespace ayther::parity {
namespace {

// The signing identity of the baked packs: the test key of make_test_pack,
// vouched for by the registry written next to the pack.
constexpr unsigned char kSeed = 11;
constexpr const char *kKeyId = "ayther-parity-bench";
constexpr const char *kNotAfter = "4102444800"; // 2100-01-01

struct Fnv {
  std::uint64_t h = 1469598103934665603ULL;
  void bytes(const void *p, std::size_t len) {
    const auto *b = static_cast<const std::uint8_t *>(p);
    for (std::size_t i = 0; i < len; ++i)
      h = (h ^ b[i]) * 1099511628211ULL;
  }
  template <class T> void value(const T &v) { bytes(&v, sizeof v); }
  void text(const char *s) {
    const std::size_t n = std::strlen(s);
    value(static_cast<std::uint32_t>(n));
    bytes(s, n);
  }
};

std::optional<std::vector<std::uint8_t>> read_bytes(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return std::nullopt;
  return std::vector<std::uint8_t>{std::istreambuf_iterator<char>(in),
                                   std::istreambuf_iterator<char>()};
}

std::uint32_t crc32(const std::uint8_t *p, std::size_t n,
                    std::uint32_t value = 0xFFFFFFFFU) {
  for (std::size_t i = 0; i < n; ++i) {
    value ^= p[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      value = (value >> 1) ^ ((value & 1U) != 0 ? 0xEDB88320U : 0U);
  }
  return value;
}

void be32(std::vector<std::uint8_t> &out, std::uint32_t v) {
  for (int s = 24; s >= 0; s -= 8)
    out.push_back(static_cast<std::uint8_t>(v >> s));
}

void chunk(std::vector<std::uint8_t> &png, const char *type,
           const std::vector<std::uint8_t> &data) {
  be32(png, static_cast<std::uint32_t>(data.size()));
  const std::size_t at = png.size();
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), data.begin(), data.end());
  be32(png, ~crc32(png.data() + at, png.size() - at));
}

/// An 8x8 RGBA PNG of one colour, zlib with one stored block: the bench needs
/// a valid, distinct asset per rule, not a compressed one.
std::vector<std::uint8_t> solid_png(std::uint32_t rgba) {
  constexpr std::uint32_t kSide = 8;
  std::vector<std::uint8_t> raw;
  for (std::uint32_t y = 0; y < kSide; ++y) {
    raw.push_back(0); // filter: none
    for (std::uint32_t x = 0; x < kSide; ++x)
      for (int s = 24; s >= 0; s -= 8)
        raw.push_back(static_cast<std::uint8_t>(rgba >> s));
  }
  std::uint32_t a = 1, b = 0; // Adler-32
  for (auto byte : raw) {
    a = (a + byte) % 65521U;
    b = (b + a) % 65521U;
  }
  std::vector<std::uint8_t> z{0x78, 0x01, 0x01};
  const auto len = static_cast<std::uint16_t>(raw.size());
  z.push_back(static_cast<std::uint8_t>(len));
  z.push_back(static_cast<std::uint8_t>(len >> 8));
  z.push_back(static_cast<std::uint8_t>(~len));
  z.push_back(static_cast<std::uint8_t>(~len >> 8));
  z.insert(z.end(), raw.begin(), raw.end());
  be32(z, (b << 16) | a);
  std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<std::uint8_t> ihdr;
  be32(ihdr, kSide);
  be32(ihdr, kSide);
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA
  chunk(png, "IHDR", ihdr);
  chunk(png, "IDAT", z);
  chunk(png, "IEND", {});
  return png;
}

std::string hex16(std::uint64_t v) {
  char text[24];
  std::snprintf(text, sizeof text, "%016llx",
                static_cast<unsigned long long>(v));
  return text;
}

std::unique_ptr<AytherSession>
open_session(const CrossSession &s, bool with_pack, std::string *error) {
  const auto fail = [&](std::string why) -> std::unique_ptr<AytherSession> {
    if (error != nullptr)
      *error = std::move(why);
    return nullptr;
  };
  AytherSession::Config config;
  config.enable_audio = false;
  config.derive_core_pack = false;
  if (with_pack) {
    config.pack_path = s.pack;
    config.trust_registry = s.registry;
  }
  if (!s.native) {
    config.core_path = s.core;
    config.rom_path = s.rom_path;
    auto created = AytherSession::create(config);
    if (!created)
      return fail("libretro session: " + created.error.message);
    return std::move(*created);
  }
  auto rom = read_bytes(s.rom_path);
  if (!rom)
    return fail("cannot read rom: " + s.rom_path);
  auto native = megadrive_native_config(std::move(*rom),
                                        std::vector<std::uint8_t>(65536));
  if (!native)
    return fail("Tessera has no megadrive backend");
  config.provider = EmulationProvider::tessera_native;
  config.native_config = std::move(*native);
  engine::NativeResult result;
  auto created = AytherSession::create(config, result);
  if (!created || !*created)
    return fail("native session: " +
                (created ? std::string("no session") : created.error.message));
  return std::move(*created);
}

std::string manifest(const std::string &game_id) {
  return "[pack]\n"
         "name       = \"parity_bench_cross_pack\"\n"
         "version    = \"1.0.0\"\n"
         "game_id    = \"" +
         game_id +
         "\"\n"
         "ayther_min = \"0.1.0\"\n"
         "author = \"AYTHER parity_bench\"\n"
         "license = \"CC0-1.0\"\n"
         "contributors = [\"AYTHER parity_bench\"]\n"
         "\n[systems]\nincluded = [\"tiles\", \"sprites\", \"planes\"]\n"
         "\n[regions]\ndefault = \"NTSC\"\nsupported = [\"NTSC\"]\n";
}

void sprite_command(Fnv &f, const AytherSpriteSub &s) {
  f.text(s.asset_path);
  f.value(s.screen_x);
  f.value(s.screen_y);
  f.value(s.w_tiles);
  f.value(s.h_tiles);
  f.value(s.w_px);
  f.value(s.h_px);
  f.value(s.mirror);
  f.value(s.palette);
  f.value(s.synth_pal);
  f.bytes(s.ref_rgb, sizeof s.ref_rgb);
  f.value(s.u0);
  f.value(s.v0);
  f.value(s.uw);
  f.value(s.vh);
  f.value(s.pose_key);
  f.text(s.mask_path);
}

template <class T>
void parallel(Fnv &f, const T *p, std::size_t count, std::size_t stride = 1) {
  f.value(static_cast<std::uint8_t>(p != nullptr));
  if (p != nullptr)
    f.bytes(p, count * stride * sizeof(T));
}

void entry(std::vector<std::string> &out, const char *element,
           const char *asset, int x, int y) {
  char pos[32];
  std::snprintf(pos, sizeof pos, " %d %d", x, y);
  out.push_back(std::string(element) + " " + asset + pos);
}

} // namespace

std::string play_line(std::uint32_t frame, const FrameView &v,
                      std::uint64_t &fb_hash, std::uint32_t &fb_count,
                      std::string *detail) {
  if (fb_hash565(v, fb_hash))
    fb_count = v.fb_height;

  // The multiset of (element, identity, position): the rule's asset names the
  // identity the pack substituted. Sorted, so the order is not compared.
  std::vector<std::string> subs;
  for (std::uint32_t i = 0; v.tile_subs != nullptr && i < v.tile_sub_count; ++i)
    entry(subs, "tile", v.tile_subs[i].asset_path,
          static_cast<int>(v.tile_subs[i].tile_x),
          static_cast<int>(v.tile_subs[i].tile_y));
  const struct {
    const char *name;
    const AytherSpriteSub *p;
    std::uint32_t n;
  } lists[] = {{"sprite", v.sprite_subs, v.sprite_sub_count},
               {"plane", v.plane_tile_subs, v.plane_tile_sub_count},
               {"entity", v.entity_subs, v.entity_sub_count}};
  for (const auto &list : lists)
    for (std::uint32_t i = 0; list.p != nullptr && i < list.n; ++i)
      entry(subs, list.name, list.p[i].asset_path, list.p[i].screen_x,
            list.p[i].screen_y);
  for (std::uint32_t i = 0; v.anim_frames != nullptr && i < v.anim_frame_count;
       ++i)
    entry(subs, "anim", v.anim_frames[i].asset,
          static_cast<int>(v.anim_frames[i].dst_x),
          static_cast<int>(v.anim_frames[i].dst_y));
  std::sort(subs.begin(), subs.end());
  Fnv set;
  for (const auto &s : subs) {
    set.text(s.c_str());
    if (detail != nullptr)
      *detail += std::to_string(frame) + " " + s + "\n";
  }

  // The presented frame: the renderer's whole input in its drawing order.
  Fnv shown;
  shown.value(fb_hash);
  shown.value(v.tile_sub_count);
  for (std::uint32_t i = 0; v.tile_subs != nullptr && i < v.tile_sub_count;
       ++i) {
    shown.text(v.tile_subs[i].asset_path);
    shown.value(v.tile_subs[i].tile_x);
    shown.value(v.tile_subs[i].tile_y);
  }
  shown.value(v.sprite_sub_count);
  for (std::uint32_t i = 0; v.sprite_subs != nullptr && i < v.sprite_sub_count;
       ++i)
    sprite_command(shown, v.sprite_subs[i]);
  parallel(shown, v.sprite_sub_flips, v.sprite_sub_count);
  parallel(shown, v.sprite_sub_tint, v.sprite_sub_count, 3);
  parallel(shown, v.sprite_sub_slot, v.sprite_sub_count);
  parallel(shown, v.sprite_sub_prio, v.sprite_sub_count);
  shown.value(v.sprite_partition_count);
  for (std::uint32_t i = 0;
       v.sprite_partitions != nullptr && i < v.sprite_partition_count; ++i) {
    const SpritePartition &p = v.sprite_partitions[i];
    shown.value(p.sub);
    shown.value(p.x);
    shown.value(p.y);
    shown.value(p.w);
    shown.value(p.h);
    shown.value(p.chain);
  }
  shown.value(v.plane_tile_sub_count);
  shown.value(v.plane_tile_sub_hi);
  for (std::uint32_t i = 0;
       v.plane_tile_subs != nullptr && i < v.plane_tile_sub_count; ++i)
    sprite_command(shown, v.plane_tile_subs[i]);
  parallel(shown, v.plane_tile_flips, v.plane_tile_sub_count);
  parallel(shown, v.plane_tile_sub_tint, v.plane_tile_sub_count, 3);
  shown.value(v.entity_sub_count);
  for (std::uint32_t i = 0; v.entity_subs != nullptr && i < v.entity_sub_count;
       ++i)
    sprite_command(shown, v.entity_subs[i]);
  shown.value(v.anim_frame_count);
  for (std::uint32_t i = 0; v.anim_frames != nullptr && i < v.anim_frame_count;
       ++i) {
    const AnimHdFrame &a = v.anim_frames[i];
    shown.text(a.asset);
    for (float f : {a.dst_x, a.dst_y, a.dst_w, a.dst_h, a.src_x, a.src_y,
                    a.src_w, a.src_h})
      shown.value(f);
  }

  const auto count = [](std::size_t n) { return std::to_string(n); };
  return "{\"frame\":" + std::to_string(frame) + ",\"subs\":{\"hash\":\"" +
         hex16(set.h) + "\",\"count\":" + count(subs.size()) +
         "},\"presented\":{\"hash\":\"" + hex16(shown.h) +
         "\",\"count\":" + count(fb_count) + "}}\n";
}

std::optional<Shard> parse_shard(const std::string &text) {
  const auto slash = text.find('/');
  if (slash == std::string::npos || slash == 0 || slash + 1 == text.size())
    return std::nullopt;
  const auto number = [](std::string_view digits) -> std::optional<unsigned> {
    if (digits.empty() || digits.size() > 6)
      return std::nullopt;
    unsigned value = 0;
    for (const char c : digits) {
      if (c < '0' || c > '9')
        return std::nullopt;
      value = value * 10 + static_cast<unsigned>(c - '0');
    }
    return value;
  };
  const auto k = number(std::string_view(text).substr(0, slash));
  const auto n = number(std::string_view(text).substr(slash + 1));
  if (!k || !n || *k == 0 || *k > *n)
    return std::nullopt;
  return Shard{*k - 1, *n};
}

BakeResult bake_pack(const CrossSession &session,
                     const std::vector<std::uint16_t> &pads,
                     const std::string &pack_path,
                     const std::string &registry_path, Shard shard) {
  BakeResult r;
  if (shard.count == 0 || shard.index >= shard.count) {
    r.error = "invalid shard";
    return r;
  }
  auto s = open_session(session, false, &r.error);
  if (!s)
    return r;
  std::set<std::uint64_t> tiles, sprites, planes;
  for (const auto pad : pads) {
    s->set_input(0, pad);
    const FrameView &v = s->step();
    for (std::uint32_t i = 0; v.tile_occs != nullptr && i < v.tile_occ_count;
         ++i)
      tiles.insert(v.tile_occs[i].hash);
    for (std::uint32_t i = 0;
         v.sprite_occs != nullptr && i < v.sprite_occ_count; ++i)
      sprites.insert(v.sprite_occs[i].hash);
    for (std::uint32_t i = 0;
         v.plane_tile_occs != nullptr && i < v.plane_tile_occ_count; ++i)
      planes.insert(v.plane_tile_occs[i].hash);
  }
  s.reset();
  r.observed = tiles.size() + sprites.size() + planes.size();
  for (auto *ids : {&tiles, &sprites, &planes})
    std::erase_if(*ids, [&](std::uint64_t id) {
      return id % shard.count != shard.index;
    });

  const auto rom = read_bytes(session.rom_path);
  if (!rom) {
    r.error = "cannot read rom: " + session.rom_path;
    return r;
  }
  char game_id[16];
  std::snprintf(game_id, sizeof game_id, "crc32:%08x",
                ~crc32(rom->data(), rom->size()));

  std::error_code ec;
  for (const std::string &path : {pack_path, registry_path}) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty())
      std::filesystem::create_directories(parent, ec);
    std::filesystem::remove(path, ec);
  }
  char public_key[65] = {};
  if (!ayther_test_public_key_hex(kSeed, public_key, sizeof public_key)) {
    r.error = "could not derive the signing key";
    return r;
  }
  {
    std::ofstream reg(registry_path, std::ios::binary | std::ios::trunc);
    reg << "version = 1\n\n[[keys]]\n"
        << "id = \"" << kKeyId << "\"\n"
        << "algorithm = \"ed25519\"\n"
        << "public_key = \"" << public_key << "\"\n"
        << "not_before_unix = 0\n"
        << "not_after_unix = " << kNotAfter << "\n"
        << "revoked = false\n"
        << "games = [\"" << game_id << "\"]\n";
    if (!reg) {
      r.error = "cannot write the registry: " + registry_path;
      return r;
    }
  }

  // One rule and one asset per identity: the asset's name says which rule the
  // renderer was told to draw, so play can compare identities by asset.
  AytherPackBuilder *builder = ayther_pack_builder_new();
  if (builder == nullptr) {
    r.error = "could not create the pack builder";
    return r;
  }
  const std::string text = manifest(game_id);
  bool staged = ayther_pack_builder_add_bytes(
      builder, "manifest.toml", reinterpret_cast<const uint8_t *>(text.data()),
      text.size());
  std::uint32_t colour = 0x102030FFU;
  const auto catalog = [&](const std::set<std::uint64_t> &ids,
                           const char *prefix, const char *file) {
    std::string rules;
    for (const auto id : ids) {
      const std::string name = std::string(prefix) + hex16(id) + ".png";
      rules +=
          "[[sub]]\nhash = \"0x" + hex16(id) + "\"\nasset = \"" + name + "\"\n";
      const auto png = solid_png(colour);
      colour += 0x01020300U;
      staged = staged &&
               ayther_pack_builder_add_bytes(
                   builder, ("assets/" + name).c_str(), png.data(), png.size());
    }
    if (!ids.empty())
      staged = staged && ayther_pack_builder_add_bytes(
                             builder, file,
                             reinterpret_cast<const uint8_t *>(rules.data()),
                             rules.size());
  };
  catalog(tiles, "t_", "tile_substitutions.toml");
  catalog(sprites, "s_", "sprite_substitutions.toml");
  catalog(planes, "p_", "plane_tile_substitutions.toml");
  char error[512] = {};
  const bool baked = staged && ayther_test_pack_builder_finish_signed_as(
                                   builder, pack_path.c_str(), kKeyId, kSeed,
                                   error, sizeof error);
  ayther_pack_builder_free(builder);
  if (!baked) {
    r.error = std::string("could not bake the pack: ") +
              (error[0] != '\0' ? error : "staging failed") + " (tiles " +
              std::to_string(tiles.size()) + ", sprites " +
              std::to_string(sprites.size()) + ", planes " +
              std::to_string(planes.size()) + ")";
    return r;
  }
  r.tiles = tiles.size();
  r.sprites = sprites.size();
  r.planes = planes.size();
  r.ok = true;
  return r;
}

std::optional<std::string> play_pack(const CrossSession &session,
                                     const std::vector<std::uint16_t> &pads,
                                     std::string *error, std::string *detail) {
  auto s = open_session(session, true, error);
  if (!s)
    return std::nullopt;
  if (!s->has_pack()) {
    if (error != nullptr)
      *error = "the pack did not open: " + session.pack;
    return std::nullopt;
  }
  std::string out;
  std::uint64_t fb_hash = 0;
  std::uint32_t fb_count = 0;
  std::uint32_t frame = 1;
  for (const auto pad : pads) {
    s->set_input(0, pad);
    out += play_line(frame++, s->step(), fb_hash, fb_count, detail);
  }
  return out;
}

namespace {
struct PlayLine {
  std::uint32_t frame = 0;
  std::string subs, presented;
  std::size_t subs_count = 0;
};

/// The decimal number at `at` up to the next non-digit; false if none.
bool number_at(std::string_view l, std::size_t at, std::size_t &value) {
  std::size_t end = at;
  value = 0;
  while (end < l.size() && l[end] >= '0' && l[end] <= '9')
    value = value * 10 + static_cast<std::size_t>(l[end++] - '0');
  return end != at && end - at <= 9;
}

/// The 16-digit hash and the count of `"key":{"hash":"…","count":N}`.
bool field_at(std::string_view l, std::string_view key, std::string &hash,
              std::size_t &count) {
  const std::string head = "\"" + std::string(key) + "\":{\"hash\":\"";
  const auto at = l.find(head);
  if (at == std::string_view::npos)
    return false;
  const auto h = at + head.size();
  if (h + 16 > l.size() ||
      l.substr(h + 16, 10) != std::string_view("\",\"count\":"))
    return false;
  hash.assign(l.substr(h, 16));
  if (hash.find_first_not_of("0123456789abcdef") != std::string::npos)
    return false;
  return number_at(l, h + 26, count);
}

bool parse_play(const std::string &text, std::vector<PlayLine> &out) {
  std::istringstream in(text);
  std::string l;
  constexpr std::string_view kFrame = "{\"frame\":";
  while (std::getline(in, l)) {
    if (l.empty())
      continue;
    PlayLine p;
    std::size_t frame = 0, fb = 0;
    if (l.rfind(kFrame, 0) != 0 || !number_at(l, kFrame.size(), frame) ||
        !field_at(l, "subs", p.subs, p.subs_count) ||
        !field_at(l, "presented", p.presented, fb))
      return false;
    p.frame = static_cast<std::uint32_t>(frame);
    out.push_back(std::move(p));
  }
  return true;
}
} // namespace

CrossCompare compare_plays(const std::string &golden,
                           const std::string &candidate) {
  CrossCompare r;
  std::vector<PlayLine> a, b;
  if (!parse_play(golden, a) || !parse_play(candidate, b)) {
    r.error = "malformed play file";
    return r;
  }
  if (a.size() != b.size()) {
    r.error = "the play files have different frame counts (" +
              std::to_string(a.size()) + " and " + std::to_string(b.size()) +
              ")";
    return r;
  }
  CrossDivergence subs{"subs"}, shown{"presented"};
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].frame != b[i].frame) {
      r.error = "the play files number their frames differently";
      return r;
    }
    if (a[i].subs_count != 0)
      ++r.substituted_frames;
    for (auto [d, same] : {std::pair{&subs, a[i].subs == b[i].subs},
                           std::pair{&shown, a[i].presented == b[i].presented}})
      if (!same && d->frames++ == 0)
        d->frame = a[i].frame;
  }
  for (const auto &d : {subs, shown})
    if (d.frames != 0)
      r.divergences.push_back(d);
  r.frames = a.size();
  r.ok = true;
  return r;
}

} // namespace ayther::parity
