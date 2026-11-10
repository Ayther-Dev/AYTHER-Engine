#include "frame_dump.h"
#include "native_config.h"

#include <ayther/ayther_session.h>

#include <cstdio>
#include <memory>
#include <type_traits>

namespace ayther::parity {
namespace {

struct Acc {
  std::uint64_t h = 1469598103934665603ULL;
  std::uint32_t n = 0;
  void bytes(const void *p, std::size_t len) {
    const auto *b = static_cast<const std::uint8_t *>(p);
    for (std::size_t i = 0; i < len; ++i)
      h = (h ^ b[i]) * 1099511628211ULL;
  }
  template <class T> void value(const T &v) { bytes(&v, sizeof v); }
  template <class T> void array(const T *p, std::size_t count) {
    static_assert(std::has_unique_object_representations_v<T>,
                  "padded record: hash it field by field (see records())");
    n += static_cast<std::uint32_t>(count);
    if (p != nullptr && count != 0)
      bytes(p, count * sizeof(T));
  }
};

// SceneElement has padding bytes that are not initialised: hashing the raw
// array made two identical runs differ, so it is hashed field by field.
void scene_element(Acc &a, const SceneElement &e) {
  a.value(e.hash);
  a.value(e.x);
  a.value(e.y);
  a.value(e.w);
  a.value(e.h);
  a.value(e.pattern);
  a.value(e.palette);
  a.value(e.flips);
  a.value(e.layer);
  a.value(e.priority);
  a.value(e.slot);
  a.value(e.chain);
  a.value(e.sub_kind);
  a.value(e.hidden);
  a.value(e.claimed);
  a.bytes(e.fx_tint, sizeof e.fx_tint);
  a.value(e.fx_opacity);
  a.value(e.fx_outline);
  a.value(e.fx_enhance);
  a.value(e.fx_enhance_k);
  a.value(e.owner);
  a.value(e.sub);
  a.value(e.clip_x0);
  a.value(e.clip_y0);
  a.value(e.clip_x1);
  a.value(e.clip_y1);
  a.value(e.raster_identity_known);
}

// The same holds for these records: raw hashing picked up uninitialised tail
// padding (visible under ASan), so each is hashed field by field.
template <class T, class F>
void records(Acc &a, const T *p, std::size_t count, F field) {
  a.n += static_cast<std::uint32_t>(count);
  for (std::size_t i = 0; p != nullptr && i < count; ++i)
    field(a, p[i]);
}

void sprite_occurrence(Acc &a, const AytherSpriteOccurrence &e) {
  a.value(e.hash);
  a.value(e.anim_group_id);
  a.value(e.w_tiles);
  a.value(e.h_tiles);
  a.value(e.screen_x);
  a.value(e.screen_y);
  a.value(e.link);
  a.value(e.palette);
  a.value(e.priority);
  a.value(e.slot);
  a.value(e.hflip);
  a.value(e.vflip);
}

void plane_cell(Acc &a, const PlaneCellHit &e) {
  a.value(e.hash);
  a.value(e.screen_x);
  a.value(e.screen_y);
  a.value(e.plane);
  a.value(e.pattern);
  a.value(e.palette);
  a.value(e.flags);
}

void entity_instance(Acc &a, const EntityInstance &e) {
  a.value(e.id);
  a.value(e.kind);
  a.value(e.screen_x);
  a.value(e.screen_y);
  a.value(e.w_px);
  a.value(e.h_px);
  a.value(e.sprite_count);
}

void emit(std::string &out, const char *name, const Acc &a, bool last) {
  char buf[96];
  std::snprintf(buf, sizeof buf, "\"%s\":{\"hash\":\"%016llx\",\"count\":%u}%s",
                name, static_cast<unsigned long long>(a.h), a.n,
                last ? "" : ",");
  out += buf;
}

std::uint16_t pixel_565(const FrameView &v, const std::uint8_t *row,
                        std::uint32_t x) {
  if (v.pixel_encoding == FramePixelEncoding::rgba8) {
    const std::uint8_t *q = row + std::size_t(x) * 4;
    return static_cast<std::uint16_t>(((q[0] >> 3) << 11) | ((q[1] >> 2) << 5) |
                                      (q[2] >> 3));
  }
  if (v.fb_format == 1) { // XRGB8888 (B, G, R, X bytes)
    const std::uint8_t *q = row + std::size_t(x) * 4;
    return static_cast<std::uint16_t>(((q[2] >> 3) << 11) | ((q[1] >> 2) << 5) |
                                      (q[0] >> 3));
  }
  const std::uint16_t w = static_cast<std::uint16_t>(
      row[std::size_t(x) * 2] | (row[std::size_t(x) * 2 + 1] << 8));
  if (v.fb_format == 2) // RGB565
    return w;
  // 0RGB1555
  return static_cast<std::uint16_t>((((w >> 10) & 0x1F) << 11) |
                                    (((w >> 5) & 0x1F) << 6) | (w & 0x1F));
}

std::string line(std::uint32_t frame, const FrameView &v,
                 std::uint64_t &fb_hash, std::uint32_t &fb_count) {
  Acc fb;
  if (v.fb_pixels != nullptr) {
    const auto *p = static_cast<const std::uint8_t *>(v.fb_pixels);
    fb.value(v.fb_width);
    fb.value(v.fb_height);
    // Canonical RGB565 per visible pixel, whatever the provider emits
    // (libretro RGB565 or XRGB8888, native RGBA8 expanded from RGB565), so
    // the two providers' images hash alike (spec 008, TP-209.1).
    for (std::uint32_t y = 0; y < v.fb_height; ++y) {
      const std::uint8_t *row = p + std::size_t(y) * v.fb_pitch;
      for (std::uint32_t x = 0; x < v.fb_width; ++x)
        fb.value(pixel_565(v, row, x));
    }
    fb.n = v.fb_height;
    fb_hash = fb.h;
    fb_count = fb.n;
  } else { // the core repeated the previous frame
    fb.h = fb_hash;
    fb.n = fb_count;
  }
  Acc scene, sprites, tiles, cells, raster, chip, chip_seq, palette, attr, pcm;
  for (std::uint32_t i = 0; v.scene != nullptr && i < v.scene_count; ++i)
    scene_element(scene, v.scene[i]);
  scene.n = v.scene_count;
  scene.value(v.scene_backdrop);
  scene.value(v.scene_left_blank);
  records(sprites, v.sprite_occs, v.sprite_occ_count, sprite_occurrence);
  tiles.array(v.tile_occs, v.tile_occ_count);
  records(cells, v.plane_cells, v.plane_cell_count, plane_cell);
  raster.value(v.raster_reasons);
  raster.value(v.raster_band_count);
  raster.bytes(v.raster_bands, sizeof v.raster_bands);
  raster.bytes(v.plane_hscroll, sizeof v.plane_hscroll);
  raster.bytes(v.plane_vscroll, sizeof v.plane_vscroll);
  raster.bytes(v.plane_hscroll_lines, sizeof v.plane_hscroll_lines);
  raster.bytes(v.plane_vscroll_col, sizeof v.plane_vscroll_col);
  raster.n = v.raster_band_count;
  chip.array(v.chip_writes, v.chip_write_count);
  // The same writes without their cycle: the functional sequence. The cycle
  // of Tessera differs from gpgx (DRAM refresh, EXC-001..004) and no identity
  // uses it, so TP-209.3 excepts chip_writes and keeps comparing this one.
  records(chip_seq, v.chip_writes, v.chip_write_count,
          [](Acc &a, const AytherAudioWrite &w) {
            a.value(w.addr);
            a.value(w.data);
            a.value(w.chip);
          });
  palette.array(v.scene_cram, v.scene_cram_size);
  attr.array(v.sprite_occ_core_drawn, v.sprite_occ_count);
  records(attr, v.entity_instances, v.entity_instance_count, entity_instance);
  pcm.array(v.audio_occs, v.audio_occ_count);

  std::string out = "{\"frame\":" + std::to_string(frame) + ",";
  emit(out, "fb", fb, false);
  emit(out, "scene", scene, false);
  emit(out, "sprites", sprites, false);
  emit(out, "tiles", tiles, false);
  emit(out, "plane_cells", cells, false);
  emit(out, "raster", raster, false);
  emit(out, "chip_writes", chip, false);
  emit(out, "chip_seq", chip_seq, false);
  emit(out, "palette", palette, false);
  emit(out, "attribution", attr, false);
  emit(out, "pcm", pcm, true);
  out += "}\n";
  return out;
}

} // namespace

namespace {
std::string run(AytherSession &s, const std::vector<std::uint16_t> &pads) {
  std::string out;
  std::uint64_t fb_hash = 0;
  std::uint32_t fb_count = 0;
  std::uint32_t frame = 1;
  for (const auto pad : pads) {
    s.set_input(0, pad);
    out += line(frame++, s.step(), fb_hash, fb_count);
  }
  return out;
}
} // namespace

std::optional<std::string>
dump_session(const std::string &core_path, const std::string &rom_path,
             const std::vector<std::uint16_t> &pads) {
  AytherSession::Config config;
  config.core_path = core_path;
  config.rom_path = rom_path;
  config.enable_audio = false;
  config.derive_core_pack = false;
  auto created = AytherSession::create(config);
  if (!created)
    return std::nullopt;
  std::unique_ptr<AytherSession> s = std::move(*created);
  return run(*s, pads);
}

std::optional<std::string>
dump_native_session(const std::vector<std::uint8_t> &rom,
                    const std::vector<std::uint16_t> &pads) {
  auto native = megadrive_native_config(rom, std::vector<std::uint8_t>(65536));
  if (!native)
    return std::nullopt;
  AytherSession::Config config;
  config.provider = EmulationProvider::tessera_native;
  config.native_config = std::move(*native);
  config.enable_audio = false;
  config.derive_core_pack = false;
  engine::NativeResult result;
  auto created = AytherSession::create(config, result);
  if (!created || !*created)
    return std::nullopt;
  std::unique_ptr<AytherSession> s = std::move(*created);
  return run(*s, pads);
}

} // namespace ayther::parity
