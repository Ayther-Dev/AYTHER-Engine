// Spec 002, BR-029b (RF-3.6): the phase of each Level-1 HD animation
// (contracts.md C4 `hd_animation_phase`) is exported and restored, and the
// frames after a restore are drawn as in linear production.
#include <ayther/ayther_animation.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

constexpr std::uint64_t kClip = 0xC11;
constexpr std::uint64_t kPoseA = 0xA0;
constexpr std::uint64_t kPoseB = 0xB0;

void define_clip(ayther::AnimationPlayer &player) {
  ayther::HdPose poses[2];
  poses[0].pose = kPoseA;
  poses[0].src_w = poses[0].src_h = 16;
  poses[0].anchor = {0, 0, 16, 16};
  poses[0].duration_ticks = 8;
  poses[1].pose = kPoseB;
  poses[1].src_x = 16;
  poses[1].src_w = poses[1].src_h = 16;
  poses[1].anchor = {64, 0, 16, 16};
  poses[1].duration_ticks = 8;
  player.define(kClip, "graphics/sheet.png", poses, 2, 1);
}

// Pose A for frames 0-5, then pose B: the tween glides from A's anchor.
float frame_x(ayther::AnimationPlayer &player, std::uint64_t frame) {
  AytherSpriteOccurrence occ{};
  occ.hash = frame < 6 ? kPoseA : kPoseB;
  occ.w_tiles = occ.h_tiles = 2;
  if (player.resolve(&occ, 1, frame) != 1)
    return -1000.0F;
  return player.frames()[0].dst_x;
}
} // namespace

int main() try {
  ayther::AnimationPlayer linear;
  define_clip(linear);
  std::vector<float> expected;
  for (std::uint64_t f = 0; f < 14; ++f)
    expected.push_back(frame_x(linear, f));
  check(expected[3] != expected[0],
        "the Level-1 tween moves while a pose is held (not a vacuous run)");

  ayther::AnimationPlayer first;
  define_clip(first);
  for (std::uint64_t f = 0; f < 4; ++f)
    (void)frame_x(first, f);
  const std::vector<ayther::AnimationPlayer::Phase> saved = first.phases();
  check(saved.size() == 1 && saved[0].clip_id == kClip &&
            saved[0].last_pose == 0 && saved[0].pose_start_frame == 0,
        "RF-3.6: the phase names the clip, its pose and when it started");

  ayther::AnimationPlayer restored;
  define_clip(restored);
  check(restored.restore_phases(saved.data(),
                                static_cast<std::uint32_t>(saved.size())),
        "RF-3.6: the phase is restored into a player with the same clip");
  bool same = true;
  for (std::uint64_t f = 4; f < 14; ++f)
    same = same && frame_x(restored, f) == expected[f];
  check(same, "RF-3.6: the frames after the restore match linear production");

  ayther::AnimationPlayer fresh;
  define_clip(fresh);
  check(frame_x(fresh, 4) != expected[4],
        "control: without the phase the pose restarts its glide");

  // Rejected entries leave the player unchanged.
  ayther::AnimationPlayer target;
  define_clip(target);
  (void)frame_x(target, 0);
  const auto before = target.phases();
  const ayther::AnimationPlayer::Phase unknown_clip{0xBAD, 0, 0};
  const ayther::AnimationPlayer::Phase bad_pose{kClip, 2, 0};
  check(!target.restore_phases(&unknown_clip, 1) &&
            !target.restore_phases(&bad_pose, 1) &&
            !target.restore_phases(nullptr, 1),
        "an unknown clip, a pose it does not have or no entries are "
        "rejected");
  const auto after = target.phases();
  check(after.size() == before.size() &&
            after[0].last_pose == before[0].last_pose &&
            after[0].pose_start_frame == before[0].pose_start_frame,
        "a rejected restore leaves the phases unchanged");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
