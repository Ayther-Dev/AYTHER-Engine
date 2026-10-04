// Spec 002, BR-082 (RF-8.1, RF-9.1): the pose anchor by area. A pose of the
// pack catalog (no Lab override) whose bounding box covers `area` tiles must
// not take the sprite at SAT slot == area as its anchor: that sprite is not a
// member, so it must not be claimed (its original stays drawn).
//
// The test looks for a test-core frame with a member sprite of area A and a
// foreign sprite at slot A (pose_anchor_scene.h), and builds the pose from
// that frame, which is then produced again from its saved core state.
#include <ayther/ayther_session.h>

#include "pose_anchor_scene.h"
#include "pose_pack_fixture.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}
} // namespace

int main() try {
  using ayther::test::element_of;
  const std::optional<ayther::test::PoseAnchorScene> scene =
      ayther::test::find_pose_anchor_scene();
  check(scene.has_value(),
        "a frame has a member sprite of area A and a foreign sprite at slot A");
  if (!scene)
    return 1;
  const AytherSpriteOccurrence &m = scene->member;
  const AytherSpriteOccurrence &f = scene->foreign;
  std::printf("  member slot %u %ux%u, foreign slot %u\n", m.slot, m.w_tiles,
              m.h_tiles, f.slot);

  ayther::test::PosePackFixture pack("pose_anchor");
  pack.add_asset(
      "graphics/member.png",
      ayther::test::solid_png(m.w_tiles * 8, m.h_tiles * 8, 0x00FF00FFU));
  const std::string poses =
      "[[pose]]\nhashes = [\"" + ayther::test::hash_hex(m.hash) +
      "\"]\nasset = \"graphics/member.png\"\nrel = \"0,0\"\ndims = \"" +
      std::to_string(m.w_tiles * 8) + "," + std::to_string(m.h_tiles * 8) +
      "\"\n";
  std::string error;
  check(pack.bake(poses, error), "the pack with the catalog pose bakes");
  auto s = ayther::test::open_pose_session(scene->rom, pack.pack_path(),
                                           pack.registry_path());
  check(s && s->pack().is_valid(), "the session opens the pack");
  if (!s || !s->pack().is_valid()) {
    std::printf("  %s\n", error.c_str());
    return 1;
  }
  (void)s->unserialize(scene->before);
  const ayther::FrameView &v = s->step();
  const ayther::SceneElement *em = element_of(v, m);
  const ayther::SceneElement *ef = element_of(v, f);
  check(v.sprite_sub_count >= 1 && em != nullptr && em->claimed != 0,
        "the catalog pose is applied and its member is claimed");
  check(ef != nullptr && ef->claimed == 0,
        "RF-9.1: the foreign sprite at slot == area is not claimed");
  check(ef != nullptr && ef->sub < 0,
        "RF-8.1: the foreign sprite does not anchor the pose");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
