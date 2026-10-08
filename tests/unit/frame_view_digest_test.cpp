// Spec 002 (RF-3.6, RF-5.2): the visual-state oracle must include every
// field that can change what the renderer draws or where it falls back.
#include "frame_view_digest.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>

namespace {
int failures = 0;

void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

struct FrameFixture {
  AytherTileSub tile{};
  AytherSpriteSub sprite{};
  ayther::SpritePartition partition{};
  AytherSpriteSub plane_tile{};
  std::array<std::uint8_t, 1> plane_tile_flips{0};
  std::array<std::uint8_t, 3> plane_tile_tint{64, 64, 64};
  ayther::AnimHdFrame anim{};
  AytherSpriteSub panorama{};
  std::array<std::uint8_t, 3> panorama_tint{64, 64, 64};
  std::array<std::uint8_t, 32> video_y{};
  std::array<std::uint8_t, 16> video_u{};
  std::array<std::uint8_t, 16> video_v{};
  ayther::SceneElement element{};
  ayther::FrameView frame{};

  FrameFixture() {
    std::snprintf(tile.asset_path, sizeof(tile.asset_path), "tile.png");
    tile.tile_x = 4;
    tile.tile_y = 5;

    std::snprintf(sprite.asset_path, sizeof(sprite.asset_path), "sprite.png");
    sprite.w_tiles = 2;
    sprite.h_tiles = 2;
    sprite.uw = 1.0F;
    sprite.vh = 1.0F;

    partition.sub = 0;
    partition.x = 12;
    partition.y = 18;
    partition.w = 9;
    partition.h = 11;
    partition.chain = 3;

    std::snprintf(plane_tile.asset_path, sizeof(plane_tile.asset_path),
                  "plane.png");
    plane_tile.w_tiles = 1;
    plane_tile.h_tiles = 1;

    std::snprintf(anim.asset, sizeof(anim.asset), "anim.png");
    anim.dst_x = 1.0F;
    anim.dst_y = 2.0F;
    anim.dst_w = 3.0F;
    anim.dst_h = 4.0F;
    anim.src_x = 5.0F;
    anim.src_y = 6.0F;
    anim.src_w = 7.0F;
    anim.src_h = 8.0F;

    std::snprintf(panorama.asset_path, sizeof(panorama.asset_path),
                  "panorama.png");
    panorama.w_tiles = 8;
    panorama.h_tiles = 4;

    video_y.fill(0x10);
    video_u.fill(0x20);
    video_v.fill(0x30);

    element.layer = 3;
    element.slot = 7;
    element.chain = 1;

    frame.tile_subs = &tile;
    frame.tile_sub_count = 1;
    frame.sprite_subs = &sprite;
    frame.sprite_sub_count = 1;
    frame.sprite_partitions = &partition;
    frame.sprite_partition_count = 1;
    frame.plane_tile_subs = &plane_tile;
    frame.plane_tile_sub_count = 1;
    frame.plane_tile_flips = plane_tile_flips.data();
    frame.plane_tile_sub_tint = plane_tile_tint.data();
    frame.anim_frames = &anim;
    frame.anim_frame_count = 1;
    frame.panorama_subs = &panorama;
    frame.panorama_sub_count = 1;
    frame.panorama_sub_tint = panorama_tint.data();
    frame.panorama_plane = 0;
    frame.video_y = video_y.data();
    frame.video_u = video_u.data();
    frame.video_v = video_v.data();
    frame.video_y_stride = 4;
    frame.video_u_stride = 3;
    frame.video_v_stride = 3;
    frame.video_w = 3;
    frame.video_h = 3;
    frame.video_seq = 17;
    frame.video_plane_mask = 1;
    frame.wide_w = 398;
    frame.screen_presence_ids[0] = 0x1234;
    frame.screen_presence_count = 1;
    frame.fps_timing = 59.92;
    frame.scene = &element;
    frame.scene_count = 1;
  }

  FrameFixture(const FrameFixture &) = delete;
  FrameFixture &operator=(const FrameFixture &) = delete;
};

using Mutate = void (*)(FrameFixture &);

struct MutationCase {
  const char *name;
  Mutate mutate;
};

const MutationCase kVisualMutations[] = {
    {"tile_subs asset", [](FrameFixture &f) { f.tile.asset_path[0] = 'T'; }},
    {"tile_subs tile_x", [](FrameFixture &f) { ++f.tile.tile_x; }},
    {"tile_subs tile_y", [](FrameFixture &f) { ++f.tile.tile_y; }},
    {"sprite_partition_count",
     [](FrameFixture &f) { f.frame.sprite_partition_count = 0; }},
    {"sprite_partitions sub", [](FrameFixture &f) { ++f.partition.sub; }},
    {"sprite_partitions x", [](FrameFixture &f) { ++f.partition.x; }},
    {"sprite_partitions y", [](FrameFixture &f) { ++f.partition.y; }},
    {"sprite_partitions w", [](FrameFixture &f) { ++f.partition.w; }},
    {"sprite_partitions h", [](FrameFixture &f) { ++f.partition.h; }},
    {"sprite_partitions chain", [](FrameFixture &f) { ++f.partition.chain; }},
    {"plane_tile_flips", [](FrameFixture &f) { f.plane_tile_flips[0] ^= 1; }},
    {"plane_tile_sub_tint", [](FrameFixture &f) { ++f.plane_tile_tint[1]; }},
    {"anim_frame_count", [](FrameFixture &f) { f.frame.anim_frame_count = 0; }},
    {"anim_frames asset", [](FrameFixture &f) { f.anim.asset[0] = 'A'; }},
    {"anim_frames dst_x", [](FrameFixture &f) { f.anim.dst_x += 0.5F; }},
    {"anim_frames dst_y", [](FrameFixture &f) { f.anim.dst_y += 0.5F; }},
    {"anim_frames dst_w", [](FrameFixture &f) { f.anim.dst_w += 0.5F; }},
    {"anim_frames dst_h", [](FrameFixture &f) { f.anim.dst_h += 0.5F; }},
    {"anim_frames src_x", [](FrameFixture &f) { f.anim.src_x += 0.5F; }},
    {"anim_frames src_y", [](FrameFixture &f) { f.anim.src_y += 0.5F; }},
    {"anim_frames src_w", [](FrameFixture &f) { f.anim.src_w += 0.5F; }},
    {"anim_frames src_h", [](FrameFixture &f) { f.anim.src_h += 0.5F; }},
    {"video_y presence", [](FrameFixture &f) { f.frame.video_y = nullptr; }},
    {"video_u presence", [](FrameFixture &f) { f.frame.video_u = nullptr; }},
    {"video_v presence", [](FrameFixture &f) { f.frame.video_v = nullptr; }},
    {"video_y payload", [](FrameFixture &f) { ++f.video_y[4]; }},
    {"video_u payload", [](FrameFixture &f) { ++f.video_u[3]; }},
    {"video_v payload", [](FrameFixture &f) { ++f.video_v[3]; }},
    {"video_y_stride", [](FrameFixture &f) { ++f.frame.video_y_stride; }},
    {"video_u_stride", [](FrameFixture &f) { ++f.frame.video_u_stride; }},
    {"video_v_stride", [](FrameFixture &f) { ++f.frame.video_v_stride; }},
    {"video_w", [](FrameFixture &f) { ++f.frame.video_w; }},
    {"video_h", [](FrameFixture &f) { ++f.frame.video_h; }},
    {"video_seq", [](FrameFixture &f) { ++f.frame.video_seq; }},
    {"video_plane_mask", [](FrameFixture &f) { f.frame.video_plane_mask = 2; }},
    {"video_front", [](FrameFixture &f) { f.frame.video_front = 1; }},
    {"panorama_sub_tint", [](FrameFixture &f) { ++f.panorama_tint[2]; }},
    {"panorama_plane", [](FrameFixture &f) { f.frame.panorama_plane = 1; }},
    {"wide_w", [](FrameFixture &f) { ++f.frame.wide_w; }},
    {"screen_presence_count",
     [](FrameFixture &f) { f.frame.screen_presence_count = 2; }},
    {"screen_presence_ids",
     [](FrameFixture &f) { ++f.frame.screen_presence_ids[0]; }},
    {"vs_two_cell", [](FrameFixture &f) { f.frame.vs_two_cell = true; }},
    {"fps_timing", [](FrameFixture &f) { f.frame.fps_timing = 50.0; }},
    {"scene raster_identity_known",
     [](FrameFixture &f) { f.element.raster_identity_known = 0; }},
    {"scene owner", [](FrameFixture &f) { f.element.owner = 4; }},
    {"scene clip_x0", [](FrameFixture &f) { f.element.clip_x0 = 1; }},
    {"scene clip_y0", [](FrameFixture &f) { f.element.clip_y0 = 1; }},
    {"scene clip_x1", [](FrameFixture &f) { f.element.clip_x1 = 8; }},
    {"scene clip_y1", [](FrameFixture &f) { f.element.clip_y1 = 8; }},
    {"raster_reasons", [](FrameFixture &f) { f.frame.raster_reasons = 8; }},
    {"raster_band_count",
     [](FrameFixture &f) { f.frame.raster_band_count = 1; }},
    {"raster_bands",
     [](FrameFixture &f) {
       f.frame.raster_band_count = 1;
       f.frame.raster_bands[0][0] = 12;
       f.frame.raster_bands[0][1] = 34;
     }},
};
} // namespace

int main() try {
  for (const MutationCase &test : kVisualMutations) {
    FrameFixture fixture;
    const std::uint64_t baseline = ayther::test::scene_digest(fixture.frame);
    test.mutate(fixture);
    check(ayther::test::scene_digest(fixture.frame) != baseline, test.name);
  }

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
