// Spec 002, BR-105 (RF-8.3): each pose replacement publishes its depth parts
// (FrameView::sprite_partitions), one per region of its members, each at its
// member's link-chain position. A catalog pose of two test-core sprites at
// different chain positions, over a frame reproduced from its saved state.
#include <ayther/ayther_session.h>

#include "../../tools/common/synth_rom.h"
#include "pose_anchor_scene.h"
#include "pose_pack_fixture.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

struct TwoSprites {
  std::string rom;
  std::vector<std::uint8_t> before;
  AytherSpriteOccurrence a{};
  AytherSpriteOccurrence b{};
};

// A frame with two sprites of different hashes and SAT slots, the second to
// the right of the first, so a two-member pose can be authored from them.
std::optional<TwoSprites> find_two() {
  for (int r = 0; r < 64; ++r) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("ayther_pose_partition_" + std::to_string(r) + ".md");
    const std::string title = "AYTHER POSE PARTITION " + std::to_string(r);
    ayther::synth::Rom rom(title.c_str());
    ayther::synth::program_canonical(rom, true);
    if (!rom.save(path))
      return std::nullopt;
    auto s = ayther::test::open_pose_session(path.string());
    if (!s)
      return std::nullopt;
    for (int f = 0; f < 40; ++f) {
      std::vector<std::uint8_t> before;
      (void)s->serialize(before);
      const ayther::FrameView &v = s->step();
      for (std::uint32_t i = 0; i < v.sprite_occ_count; ++i)
        for (std::uint32_t j = 0; j < v.sprite_occ_count; ++j) {
          const AytherSpriteOccurrence &a = v.sprite_occs[i];
          const AytherSpriteOccurrence &b = v.sprite_occs[j];
          // b to the right of a, without overlapping it.
          const bool apart = a.screen_x + a.w_tiles * 8 <= b.screen_x;
          if (i != j && a.hash != b.hash && a.slot != b.slot && apart &&
              a.screen_x >= 0 && a.screen_y >= 0 && b.screen_y >= 0 &&
              b.screen_x + b.w_tiles * 8 <= 320)
            return TwoSprites{path.string(), std::move(before), a, b};
        }
    }
  }
  return std::nullopt;
}
} // namespace

int main() try {
  const std::optional<TwoSprites> two = find_two();
  check(two.has_value(), "a frame has two separate sprites");
  if (!two)
    return 1;
  const AytherSpriteOccurrence &a = two->a;
  const AytherSpriteOccurrence &b = two->b;
  const int w = b.screen_x + b.w_tiles * 8 - a.screen_x;
  const int top = std::min(a.screen_y, b.screen_y);
  const int h =
      std::max(a.screen_y + a.h_tiles * 8, b.screen_y + b.h_tiles * 8) - top;
  std::printf("  members: slot %u at x=%d, slot %u at x=%d; pose %dx%d\n",
              a.slot, a.screen_x, b.slot, b.screen_x, w, h);

  ayther::test::PosePackFixture pack("pose_partition");
  pack.add_asset("graphics/pose.png",
                 ayther::test::solid_png(w, h, 0x00FF00FFU));
  const std::string poses =
      "[[pose]]\nhashes = [\"" + ayther::test::hash_hex(a.hash) + "\", \"" +
      ayther::test::hash_hex(b.hash) +
      "\"]\nasset = \"graphics/pose.png\"\nrel = \"0,0|" +
      std::to_string(b.screen_x - a.screen_x) + "," +
      std::to_string(b.screen_y - a.screen_y) + "\"\ndims = \"" +
      std::to_string(a.w_tiles * 8) + "," + std::to_string(a.h_tiles * 8) +
      "|" + std::to_string(b.w_tiles * 8) + "," +
      std::to_string(b.h_tiles * 8) + "\"\n";
  std::string error;
  check(pack.bake(poses, error), "the two-member pose pack bakes");
  auto s = ayther::test::open_pose_session(two->rom, pack.pack_path(),
                                           pack.registry_path());
  if (!s)
    return 1;
  (void)s->unserialize(two->before);
  const ayther::FrameView &v = s->step();
  check(v.sprite_sub_count >= 1, "the pose is applied");

  const ayther::SceneElement *ea = ayther::test::element_of(v, a);
  const ayther::SceneElement *eb = ayther::test::element_of(v, b);
  std::vector<const ayther::SpritePartition *> parts;
  for (std::uint32_t i = 0; v.sprite_partitions && i < v.sprite_partition_count;
       ++i)
    if (v.sprite_partitions[i].sub == 0)
      parts.push_back(&v.sprite_partitions[i]);
  std::printf("  parts of sub 0: %zu\n", parts.size());
  bool depth_a = false;
  bool depth_b = false;
  for (const ayther::SpritePartition *p : parts) {
    const bool covers_a = a.screen_x >= p->x && a.screen_x < p->x + p->w;
    const bool covers_b = b.screen_x >= p->x && b.screen_x < p->x + p->w;
    depth_a = depth_a || (covers_a && ea != nullptr && p->chain == ea->chain);
    depth_b = depth_b || (covers_b && eb != nullptr && p->chain == eb->chain);
  }
  check(!parts.empty() && depth_a && depth_b,
        "RF-8.3: the pose publishes a part per member, each at its member's "
        "chain position");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
