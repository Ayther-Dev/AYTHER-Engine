// Spec 002, BR-085 (RF-8.4): the Lab and the Runtime take the same path. The
// same pose, once as a Lab preview override (set_pose_preview) and once in
// the pack catalog, over the same test-core frame: the scene must anchor,
// claim and order it identically, because both use the members the pose
// resolve reports.
#include <ayther/ayther_session.h>

#include "pose_anchor_scene.h"
#include "pose_pack_fixture.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

struct SpriteRow {
  std::uint8_t slot;
  std::uint8_t claimed;
  bool anchor;
  bool operator==(const SpriteRow &) const = default;
};

struct Outcome {
  std::uint32_t subs = 0;
  std::uint8_t sub_slot = 255;
  std::vector<SpriteRow> sprites;
};

Outcome outcome_of(const ayther::FrameView &v) {
  Outcome o;
  o.subs = v.sprite_sub_count;
  if (v.sprite_sub_count > 0 && v.sprite_sub_slot)
    o.sub_slot = v.sprite_sub_slot[0];
  for (std::uint32_t i = 0; i < v.scene_count; ++i)
    if (v.scene[i].layer == 3)
      o.sprites.push_back(
          {v.scene[i].slot, v.scene[i].claimed, v.scene[i].sub >= 0});
  return o;
}
} // namespace

int main() try {
  const std::optional<ayther::test::PoseAnchorScene> scene =
      ayther::test::find_pose_anchor_scene();
  check(scene.has_value(), "the anchor scene is found");
  if (!scene)
    return 1;
  const AytherSpriteOccurrence &m = scene->member;
  const int w = m.w_tiles * 8, h = m.h_tiles * 8;

  // Runtime path: the pose in the pack catalog.
  ayther::test::PosePackFixture pack("pose_paths");
  pack.add_asset("graphics/member.png",
                 ayther::test::solid_png(w, h, 0x00FF00FFU));
  std::string error;
  check(pack.bake("[[pose]]\nhashes = [\"" + ayther::test::hash_hex(m.hash) +
                      "\"]\nasset = \"graphics/member.png\"\nrel = \"0,0\"\n"
                      "dims = \"" +
                      std::to_string(w) + "," + std::to_string(h) + "\"\n",
                  error),
        "the pack with the catalog pose bakes");
  Outcome runtime;
  {
    auto s = ayther::test::open_pose_session(scene->rom, pack.pack_path(),
                                             pack.registry_path());
    if (!s)
      return 1;
    (void)s->unserialize(scene->before);
    runtime = outcome_of(s->step());
  }

  // Lab path: the same pose as a preview override, without a pack.
  Outcome lab;
  {
    auto s = ayther::test::open_pose_session(scene->rom);
    if (!s)
      return 1;
    ayther::AytherSession::PosePreview pose;
    pose.hashes = {m.hash};
    pose.rel_x = {0};
    pose.rel_y = {0};
    pose.dim_w = {static_cast<std::int16_t>(w)};
    pose.dim_h = {static_cast<std::int16_t>(h)};
    pose.bbox_w = static_cast<std::uint16_t>(w);
    pose.bbox_h = static_cast<std::uint16_t>(h);
    pose.asset = "graphics/member.png";
    pose.hd = true;
    s->set_pose_preview({pose});
    (void)s->unserialize(scene->before);
    lab = outcome_of(s->step());
  }

  std::printf("  runtime: %u sub(s), slot %u; lab: %u sub(s), slot %u\n",
              runtime.subs, runtime.sub_slot, lab.subs, lab.sub_slot);
  check(runtime.subs >= 1 && lab.subs >= 1,
        "both paths apply the pose to the frame");
  check(runtime.sub_slot == lab.sub_slot,
        "RF-8.4: both paths order the replacement at the same slot");
  check(runtime.sprites == lab.sprites,
        "RF-8.4: both paths anchor and claim the same sprites");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
