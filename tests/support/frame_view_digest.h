// FNV-1a digest of a FrameView's content for spec 002 tests (contracts.md C3
// and C4): two frames with the same digest drew the same thing. Pointers are
// followed, not hashed; the measured clocks (emu_fps, tile_ms, sprite_ms,
// audio_ms, drc_ratio, fps_timing) are left out because they are not content.
#pragma once

#include <ayther/ayther_session.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ayther::test {

class Digest {
public:
  void bytes(const void *data, std::size_t size) {
    const auto *p = static_cast<const unsigned char *>(data);
    for (std::size_t i = 0; i < size; ++i) {
      value_ ^= p[i];
      value_ *= 0x100000001B3ULL;
    }
  }
  template <typename T> void value(const T &v) { bytes(&v, sizeof(v)); }
  void text(const char *s, std::size_t capacity) {
    bytes(s, ::strnlen(s, capacity));
  }
  [[nodiscard]] std::uint64_t result() const { return value_; }

private:
  std::uint64_t value_ = 0xCBF29CE484222325ULL;
};

inline void add_subs(Digest &d, const AytherSpriteSub *subs,
                     std::uint32_t count) {
  d.value(count);
  for (std::uint32_t i = 0; subs != nullptr && i < count; ++i) {
    const AytherSpriteSub &s = subs[i];
    d.text(s.asset_path, sizeof(s.asset_path));
    d.text(s.mask_path, sizeof(s.mask_path));
    d.value(s.screen_x);
    d.value(s.screen_y);
    d.value(s.w_tiles);
    d.value(s.h_tiles);
    d.value(s.w_px);
    d.value(s.h_px);
    d.value(s.mirror);
    d.value(s.palette);
    d.value(s.synth_pal);
    d.bytes(s.ref_rgb, sizeof(s.ref_rgb));
    d.value(s.u0);
    d.value(s.v0);
    d.value(s.uw);
    d.value(s.vh);
    d.value(s.pose_key);
  }
}

template <typename T>
void add_array(Digest &d, const T *data, std::size_t count) {
  d.value(count);
  if (data != nullptr)
    d.bytes(data, count * sizeof(T));
}

/// What the frame draws: framebuffer, occurrences, every substitution array,
/// scene, VRAM/CRAM, scroll and camera, Picture and Kinematic.
inline std::uint64_t scene_digest(const FrameView &v) {
  Digest d;
  d.value(v.frame_index);
  d.value(v.fb_width);
  d.value(v.fb_height);
  d.value(v.fb_pitch);
  d.value(v.fb_format);
  if (v.fb_pixels != nullptr)
    d.bytes(v.fb_pixels, static_cast<std::size_t>(v.fb_pitch) * v.fb_height);
  d.value(v.tile_sub_count);
  add_subs(d, v.sprite_subs, v.sprite_sub_count);
  const std::uint32_t n_sub = v.sprite_subs ? v.sprite_sub_count : 0;
  add_array(d, v.sprite_sub_flips, v.sprite_sub_flips ? n_sub : 0);
  add_array(d, v.sprite_sub_tint, v.sprite_sub_tint ? n_sub * 3U : 0);
  add_array(d, v.sprite_sub_slot, v.sprite_sub_slot ? n_sub : 0);
  add_array(d, v.sprite_sub_prio, v.sprite_sub_prio ? n_sub : 0);
  add_subs(d, v.plane_tile_subs, v.plane_tile_sub_count);
  d.value(v.plane_tile_sub_hi);
  add_subs(d, v.entity_subs, v.entity_sub_count);
  add_subs(d, v.screen_subs, v.screen_sub_count);
  add_subs(d, v.panorama_subs, v.panorama_sub_count);
  d.value(v.sprite_occ_count);
  for (std::uint32_t i = 0; v.sprite_occs != nullptr && i < v.sprite_occ_count;
       ++i) {
    const AytherSpriteOccurrence &o = v.sprite_occs[i];
    d.value(o.hash);
    d.value(o.anim_group_id);
    d.value(o.w_tiles);
    d.value(o.h_tiles);
    d.value(o.screen_x);
    d.value(o.screen_y);
    d.value(o.link);
    d.value(o.palette);
    d.value(o.priority);
    d.value(o.slot);
    d.value(o.hflip);
    d.value(o.vflip);
  }
  d.value(v.scene_count);
  for (std::uint32_t i = 0; v.scene != nullptr && i < v.scene_count; ++i) {
    const SceneElement &e = v.scene[i];
    d.value(e.hash);
    d.value(e.x);
    d.value(e.y);
    d.value(e.w);
    d.value(e.h);
    d.value(e.pattern);
    d.value(e.palette);
    d.value(e.flips);
    d.value(e.layer);
    d.value(e.priority);
    d.value(e.slot);
    d.value(e.chain);
    d.value(e.sub_kind);
    d.value(e.hidden);
    d.value(e.claimed);
    d.bytes(e.fx_tint, sizeof(e.fx_tint));
    d.value(e.fx_opacity);
    d.value(e.fx_outline);
    d.value(e.fx_enhance);
    d.value(e.fx_enhance_k);
    d.value(e.sub);
  }
  add_array(d, v.scene_vram, v.scene_vram ? v.scene_vram_size : 0);
  add_array(d, v.scene_cram, v.scene_cram ? v.scene_cram_size : 0);
  d.value(v.scene_backdrop);
  d.value(v.scene_left_blank);
  d.value(v.scene_dirty);
  d.value(v.tile_occ_count);
  d.value(v.plane_tile_occ_count);
  d.value(v.plane_cell_count);
  d.value(v.plane_a_count);
  d.value(v.plane_b_count);
  d.value(v.plane_w_count);
  d.bytes(v.plane_hscroll, sizeof(v.plane_hscroll));
  d.bytes(v.plane_vscroll, sizeof(v.plane_vscroll));
  d.bytes(v.plane_vscroll_col, sizeof(v.plane_vscroll_col));
  d.bytes(v.plane_cam_x, sizeof(v.plane_cam_x));
  d.bytes(v.plane_cam_y, sizeof(v.plane_cam_y));
  d.value(v.plane_cam_valid);
  d.value(v.screen_match_id);
  d.value(v.kinematic_id);
  d.value(v.kinematic_step);
  d.value(v.audio_mute_mask);
  return d.result();
}

/// scene_digest plus the cumulative counts of unique tiles, sprites and
/// sounds: they only feed diagnostics and are not part of the visual state.
inline std::uint64_t frame_digest(const FrameView &v) {
  Digest d;
  d.value(scene_digest(v));
  d.value(v.unique_tile_count);
  d.value(v.unique_sprite_count);
  d.value(v.unique_audio_count);
  return d.result();
}

} // namespace ayther::test
