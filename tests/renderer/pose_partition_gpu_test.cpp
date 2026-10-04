// Spec 002, R2 (BR-106, BR-107; RF-8.3, RF-8.4), on a GPU: a pose
// replacement drawn by parts, each at the depth of its member.
//
// BR-106: a two-member pose (members at chains 0 and 2) with a third sprite
// at chain 1 overlapping both. The VDP puts the third sprite behind the
// first member and in front of the second; so must each part of the HD.
// BR-107: the asset overflows its members (silhouette excess); the excess
// takes the depth of the nearest member, so the third sprite (chain 1) is in
// front of the excess next to the chain-2 member.
// Both against the CPU reference compositor, the HD being the members'
// colour (and the excess drawn there as sprites of the nearest member).
#include "gpu_oracle.h"
#include "session/pose_depth.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

constexpr std::uint32_t kW = 320;
constexpr std::uint32_t kH = 224;
constexpr std::int16_t kY = 96;

ayther::SceneElement sprite(std::int16_t x, std::uint16_t pattern,
                            std::uint8_t chain) {
  ayther::SceneElement e{};
  e.hash = 0xE000U + chain * 16U + static_cast<std::uint64_t>(x);
  e.x = x;
  e.y = kY;
  e.w = 8;
  e.h = 8;
  e.pattern = pattern;
  e.layer = 3;
  e.slot = chain;
  e.chain = chain;
  return e;
}
} // namespace

int main() try {
  std::printf("=== pose_partition_gpu_test (spec 002 R2) ===\n");
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path() / "ayther_pose_partition_gpu";
  fs::create_directories(dir, ec);

  ayther::test::GpuOracle oracle;
  if (!oracle.init(kW, kH)) {
    std::fprintf(stderr, "[FAIL] GPU oracle init\n");
    return 1;
  }
  // Pattern 1 = red (colour 1), pattern 2 = green (colour 2).
  std::vector<std::uint8_t> vram(0x10000, 0);
  for (int i = 0; i < 32; ++i) {
    vram[32 + i] = 0x11;
    vram[64 + i] = 0x22;
  }
  std::vector<std::uint8_t> cram(128, 0);
  cram[2] = 0x07; // red
  cram[4] = 0x38; // green
  std::vector<std::uint16_t> fb(static_cast<std::size_t>(kW) * kH, 0);
  std::uint8_t green[3];
  ayther::test::cram_rgb8(0x0038, green);

  std::uint32_t frame = 1;
  // Members m (green, claimed by the pose), a third sprite (red), the HD
  // quad [qx, qx + qw); `reference` = the expected scene for the CPU.
  const auto run = [&](const std::vector<ayther::SceneElement> &members,
                       const ayther::SceneElement &third, std::int16_t qx,
                       int qw,
                       const std::vector<ayther::SceneElement> &reference,
                       const char *message) {
    const std::string png =
        (dir / ("pose" + std::to_string(frame) + ".png")).string();
    if (!ayther::test::write_solid_png(png, qw, 8, green)) {
      check(false, "cannot write the pose PNG");
      return;
    }
    AytherSpriteSub sub{};
    std::snprintf(sub.asset_path, sizeof(sub.asset_path), "%s", png.c_str());
    sub.screen_x = qx;
    sub.screen_y = kY;
    sub.w_tiles = static_cast<std::uint8_t>(qw / 8);
    sub.h_tiles = 1;
    sub.w_px = static_cast<std::uint16_t>(qw);
    sub.h_px = 8;
    sub.palette = 0xFF;
    sub.synth_pal = 0xFF;
    sub.uw = 1.0F;
    sub.vh = 1.0F;
    std::uint8_t slot = 0;
    std::uint8_t prio = 0;
    std::uint8_t flip = 0;
    // The parts, as the session publishes them.
    std::vector<ayther::session::DepthBox> boxes;
    for (const ayther::SceneElement &m : members)
      boxes.push_back({m.x, m.y, 8, 8, m.chain});
    std::vector<ayther::SpritePartition> parts;
    for (const ayther::session::DepthBox &p :
         ayther::session::partition_by_members({qx, kY, qw, 8, 0}, boxes))
      parts.push_back({0, static_cast<std::int16_t>(p.x),
                       static_cast<std::int16_t>(p.y),
                       static_cast<std::uint16_t>(p.w),
                       static_cast<std::uint16_t>(p.h), p.chain});
    // The scene: the members claimed (the frontmost anchors the pose).
    std::vector<ayther::SceneElement> scene;
    std::uint8_t front = 0xFF;
    for (const ayther::SceneElement &m : members)
      front = std::min(front, m.chain);
    for (ayther::SceneElement m : members) {
      m.claimed = 1;
      m.owner = 0;
      if (m.chain == front) {
        m.sub_kind = 1;
        m.sub = 0;
      }
      scene.push_back(m);
    }
    scene.push_back(third);
    ayther::FrameView fv{};
    fv.fb_width = kW;
    fv.fb_height = kH;
    fv.fb_pixels = fb.data();
    fv.fb_pitch = kW * 2;
    fv.fb_format = 2;
    fv.scene = scene.data();
    fv.scene_count = static_cast<std::uint32_t>(scene.size());
    fv.scene_vram = vram.data();
    fv.scene_vram_size = vram.size();
    fv.scene_cram = cram.data();
    fv.scene_cram_size = cram.size();
    fv.sprite_subs = &sub;
    fv.sprite_sub_count = 1;
    fv.sprite_sub_prio = &prio;
    fv.sprite_sub_slot = &slot;
    fv.sprite_sub_flips = &flip;
    fv.sprite_partitions = parts.data();
    fv.sprite_partition_count = static_cast<std::uint32_t>(parts.size());
    fv.frame_index = frame++;
    const ayther::probe::RgbImage want = oracle.expected(reference, vram, cram);
    const ayther::probe::RgbImage got = oracle.render(fv);
    const ayther::probe::O1Result r = ayther::probe::check_o1(got, want);
    std::printf("  parts=%zu differing=%zu first=(%d,%d)\n", parts.size(),
                r.differing_pixels, r.first_x, r.first_y);
    check(!got.rgb.empty() && r.identical, message);
  };

  // BR-106: members at x=100 (chain 0) and x=116 (chain 2); the third sprite
  // at x=108..116 (chain 1) overlaps the gap between them; the quad spans
  // 100..124 and covers it. Expected: the gap part 108..112 belongs to the
  // chain-0 member (nearest) and is in front of the third sprite; 112..116
  // belongs to the chain-2 member and goes behind it.
  {
    const std::vector<ayther::SceneElement> members = {sprite(100, 2, 0),
                                                       sprite(116, 2, 2)};
    const ayther::SceneElement third = sprite(108, 1, 1);
    // The CPU scene: members as green originals, the gap as green sprites at
    // the depth of the member that owns each half (4 px each: two 8 px
    // sprites offset by 4 px are equivalent at those depths).
    std::vector<ayther::SceneElement> reference = members;
    reference.push_back(sprite(104, 2, 0)); // 104..112: chain 0 (left half)
    reference.push_back(sprite(112, 2, 2)); // 112..120: chain 2 (right half)
    reference.push_back(third);
    run(members, third, 100, 24, reference,
        "RF-8.3: each part of the pose is in front of or behind the third "
        "sprite according to its member");
  }
  // BR-107: silhouette excess. Members at x=100 (chain 0) and x=108
  // (chain 2); the asset covers 92..124 (8 px of excess on each side). The
  // third sprite at x=120 (chain 1) is in front of the right excess (nearest
  // member: chain 2) and the left excess goes with chain 0.
  {
    const std::vector<ayther::SceneElement> members = {sprite(100, 2, 0),
                                                       sprite(108, 2, 2)};
    const ayther::SceneElement third = sprite(120, 1, 1);
    std::vector<ayther::SceneElement> reference = members;
    reference.push_back(sprite(92, 2, 0));  // left excess: chain 0
    reference.push_back(sprite(116, 2, 2)); // right excess: chain 2
    reference.push_back(third);
    run(members, third, 92, 32, reference,
        "RF-8.3: the silhouette excess takes the depth of the nearest member");
  }

  fs::remove_all(dir, ec);
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
