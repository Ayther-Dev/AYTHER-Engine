// FNV-1a digest of a FrameView's content for spec 002 tests (contracts.md C3
// and C4): two frames with the same digest drew the same thing. Pointers are
// followed, not hashed; the measured clocks (emu_fps, tile_ms, sprite_ms,
// audio_ms, drc_ratio) are left out because they are not content. fps_timing
// is included because the renderer uses it to position animated overlays.
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

inline void add_tile_subs(Digest &d, const AytherTileSub *subs,
                          std::uint32_t count) {
  d.value(count);
  for (std::uint32_t i = 0; subs != nullptr && i < count; ++i) {
    const AytherTileSub &s = subs[i];
    d.text(s.asset_path, sizeof(s.asset_path));
    d.value(s.tile_x);
    d.value(s.tile_y);
  }
}

inline void add_sprite_partitions(Digest &d, const SpritePartition *parts,
                                  std::uint32_t count) {
  d.value(count);
  for (std::uint32_t i = 0; parts != nullptr && i < count; ++i) {
    const SpritePartition &part = parts[i];
    d.value(part.sub);
    d.value(part.x);
    d.value(part.y);
    d.value(part.w);
    d.value(part.h);
    d.value(part.chain);
  }
}

inline void add_anim_frames(Digest &d, const AnimHdFrame *frames,
                            std::uint32_t count) {
  d.value(count);
  for (std::uint32_t i = 0; frames != nullptr && i < count; ++i) {
    const AnimHdFrame &frame = frames[i];
    d.text(frame.asset, sizeof(frame.asset));
    d.value(frame.dst_x);
    d.value(frame.dst_y);
    d.value(frame.dst_w);
    d.value(frame.dst_h);
    d.value(frame.src_x);
    d.value(frame.src_y);
    d.value(frame.src_w);
    d.value(frame.src_h);
  }
}

inline void add_video_plane(Digest &d, const void *pixels, std::uint32_t stride,
                            std::uint32_t width, std::uint32_t height) {
  const bool present = pixels != nullptr;
  d.value(present);
  if (!present)
    return;
  const auto *row = static_cast<const std::uint8_t *>(pixels);
  for (std::uint32_t y = 0; y < height; ++y) {
    d.bytes(row, width);
    row += stride;
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
  d.value(v.fps_timing);
  d.value(v.fb_width);
  d.value(v.fb_height);
  d.value(v.fb_pitch);
  d.value(v.fb_format);
  if (v.fb_pixels != nullptr)
    d.bytes(v.fb_pixels, static_cast<std::size_t>(v.fb_pitch) * v.fb_height);
  add_tile_subs(d, v.tile_subs, v.tile_sub_count);
  add_subs(d, v.sprite_subs, v.sprite_sub_count);
  const std::uint32_t n_sub = v.sprite_subs ? v.sprite_sub_count : 0;
  add_array(d, v.sprite_sub_flips, v.sprite_sub_flips ? n_sub : 0);
  add_array(d, v.sprite_sub_tint, v.sprite_sub_tint ? n_sub * 3U : 0);
  add_array(d, v.sprite_sub_slot, v.sprite_sub_slot ? n_sub : 0);
  add_array(d, v.sprite_sub_prio, v.sprite_sub_prio ? n_sub : 0);
  add_sprite_partitions(d, v.sprite_partitions, v.sprite_partition_count);
  add_subs(d, v.plane_tile_subs, v.plane_tile_sub_count);
  const std::uint32_t n_plane_sub =
      v.plane_tile_subs ? v.plane_tile_sub_count : 0;
  add_array(d, v.plane_tile_flips, v.plane_tile_flips ? n_plane_sub : 0);
  add_array(d, v.plane_tile_sub_tint,
            v.plane_tile_sub_tint ? n_plane_sub * 3U : 0);
  d.value(v.plane_tile_sub_hi);
  add_subs(d, v.entity_subs, v.entity_sub_count);
  add_anim_frames(d, v.anim_frames, v.anim_frame_count);
  add_subs(d, v.screen_subs, v.screen_sub_count);
  add_subs(d, v.panorama_subs, v.panorama_sub_count);
  const std::uint32_t n_panorama_sub =
      v.panorama_subs ? v.panorama_sub_count : 0;
  add_array(d, v.panorama_sub_tint,
            v.panorama_sub_tint ? n_panorama_sub * 3U : 0);
  d.value(v.panorama_plane);
  d.value(v.video_y_stride);
  d.value(v.video_u_stride);
  d.value(v.video_v_stride);
  d.value(v.video_w);
  d.value(v.video_h);
  d.value(v.video_seq);
  d.value(v.video_plane_mask);
  d.value(v.video_front);
  const std::uint32_t chroma_width = v.video_w / 2U + v.video_w % 2U;
  const std::uint32_t chroma_height = v.video_h / 2U + v.video_h % 2U;
  add_video_plane(d, v.video_y, v.video_y_stride, v.video_w, v.video_h);
  add_video_plane(d, v.video_u, v.video_u_stride, chroma_width, chroma_height);
  add_video_plane(d, v.video_v, v.video_v_stride, chroma_width, chroma_height);
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
    d.value(e.raster_identity_known);
    d.value(e.sub_kind);
    d.value(e.hidden);
    d.value(e.claimed);
    d.bytes(e.fx_tint, sizeof(e.fx_tint));
    d.value(e.fx_opacity);
    d.value(e.fx_outline);
    d.value(e.fx_enhance);
    d.value(e.fx_enhance_k);
    d.value(e.owner);
    d.value(e.sub);
    d.value(e.clip_x0);
    d.value(e.clip_y0);
    d.value(e.clip_x1);
    d.value(e.clip_y1);
  }
  add_array(d, v.scene_vram, v.scene_vram ? v.scene_vram_size : 0);
  add_array(d, v.scene_cram, v.scene_cram ? v.scene_cram_size : 0);
  d.value(v.scene_backdrop);
  d.value(v.scene_left_blank);
  d.value(v.scene_dirty);
  d.value(v.raster_reasons);
  d.value(v.raster_band_count);
  for (std::uint32_t i = 0; i < v.raster_band_count && i < 16; ++i)
    d.bytes(v.raster_bands[i], sizeof(v.raster_bands[i]));
  d.value(v.tile_occ_count);
  d.value(v.plane_tile_occ_count);
  d.value(v.plane_cell_count);
  d.value(v.plane_a_count);
  d.value(v.plane_b_count);
  d.value(v.plane_w_count);
  d.bytes(v.plane_hscroll, sizeof(v.plane_hscroll));
  d.bytes(v.plane_vscroll, sizeof(v.plane_vscroll));
  d.value(v.vs_two_cell);
  d.bytes(v.plane_vscroll_col, sizeof(v.plane_vscroll_col));
  d.bytes(v.plane_cam_x, sizeof(v.plane_cam_x));
  d.bytes(v.plane_cam_y, sizeof(v.plane_cam_y));
  d.value(v.plane_cam_valid);
  d.value(v.wide_w);
  d.value(v.screen_match_id);
  d.value(v.screen_presence_count);
  for (std::uint32_t i = 0; i < v.screen_presence_count && i < 8U; ++i)
    d.value(v.screen_presence_ids[i]);
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
