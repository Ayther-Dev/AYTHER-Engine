#include "frame_dump.h"

#include <ayther/ayther_session.h>

#include <cstdio>
#include <memory>

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
    n += static_cast<std::uint32_t>(count);
    if (p != nullptr && count != 0)
      bytes(p, count * sizeof(T));
  }
};

// SceneElement has padding bytes that are not initialised: hashing the raw
// array made two identical runs differ, so it is hashed field by field.
void scene_element(Acc &a, const SceneElement &e) {
  a.value(e.hash);
  a.value(e.x); a.value(e.y); a.value(e.w); a.value(e.h);
  a.value(e.pattern); a.value(e.palette); a.value(e.flips);
  a.value(e.layer); a.value(e.priority); a.value(e.slot); a.value(e.chain);
  a.value(e.sub_kind); a.value(e.hidden); a.value(e.claimed);
  a.bytes(e.fx_tint, sizeof e.fx_tint);
  a.value(e.fx_opacity); a.value(e.fx_outline); a.value(e.fx_enhance);
  a.value(e.fx_enhance_k); a.value(e.owner); a.value(e.sub);
  a.value(e.clip_x0); a.value(e.clip_y0); a.value(e.clip_x1); a.value(e.clip_y1);
  a.value(e.raster_identity_known);
}

void emit(std::string &out, const char *name, const Acc &a, bool last) {
  char buf[96];
  std::snprintf(buf, sizeof buf, "\"%s\":{\"hash\":\"%016llx\",\"count\":%u}%s", name,
                static_cast<unsigned long long>(a.h), a.n, last ? "" : ",");
  out += buf;
}

std::string line(std::uint32_t frame, const FrameView &v, std::uint64_t &fb_hash,
                 std::uint32_t &fb_count) {
  Acc fb;
  if (v.fb_pixels != nullptr) {
    const auto *p = static_cast<const std::uint8_t *>(v.fb_pixels);
    fb.value(v.fb_width);
    fb.value(v.fb_height);
    for (std::uint32_t y = 0; y < v.fb_height; ++y)
      fb.bytes(p + std::size_t(y) * v.fb_pitch, v.fb_pitch);
    fb.n = v.fb_height;
    fb_hash = fb.h;
    fb_count = fb.n;
  } else { // the core repeated the previous frame
    fb.h = fb_hash;
    fb.n = fb_count;
  }
  Acc scene, sprites, tiles, cells, raster, chip, palette, attr;
  for (std::uint32_t i = 0; v.scene != nullptr && i < v.scene_count; ++i)
    scene_element(scene, v.scene[i]);
  scene.n = v.scene_count;
  scene.value(v.scene_backdrop);
  scene.value(v.scene_left_blank);
  sprites.array(v.sprite_occs, v.sprite_occ_count);
  tiles.array(v.tile_occs, v.tile_occ_count);
  cells.array(v.plane_cells, v.plane_cell_count);
  raster.value(v.raster_reasons);
  raster.value(v.raster_band_count);
  raster.bytes(v.raster_bands, sizeof v.raster_bands);
  raster.bytes(v.plane_hscroll, sizeof v.plane_hscroll);
  raster.bytes(v.plane_vscroll, sizeof v.plane_vscroll);
  raster.bytes(v.plane_hscroll_lines, sizeof v.plane_hscroll_lines);
  raster.bytes(v.plane_vscroll_col, sizeof v.plane_vscroll_col);
  raster.n = v.raster_band_count;
  chip.array(v.chip_writes, v.chip_write_count);
  palette.array(v.scene_cram, v.scene_cram_size);
  attr.array(v.sprite_occ_core_drawn, v.sprite_occ_count);
  attr.array(v.entity_instances, v.entity_instance_count);
  attr.array(v.audio_occs, v.audio_occ_count);

  std::string out = "{\"frame\":" + std::to_string(frame) + ",";
  const Acc *all[] = {&fb, &scene, &sprites, &cells, &raster, &chip, &palette, &attr};
  (void)all;
  emit(out, "fb", fb, false);
  emit(out, "scene", scene, false);
  emit(out, "sprites", sprites, false);
  emit(out, "tiles", tiles, false);
  emit(out, "plane_cells", cells, false);
  emit(out, "raster", raster, false);
  emit(out, "chip_writes", chip, false);
  emit(out, "palette", palette, false);
  emit(out, "attribution", attr, true);
  out += "}\n";
  return out;
}

} // namespace

std::optional<std::string> dump_session(const std::string &core_path,
                                        const std::string &rom_path,
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
  std::string out;
  std::uint64_t fb_hash = 0;
  std::uint32_t fb_count = 0;
  std::uint32_t frame = 1;
  for (const auto pad : pads) {
    s->set_input(0, pad);
    out += line(frame++, s->step(), fb_hash, fb_count);
  }
  return out;
}

} // namespace ayther::parity
