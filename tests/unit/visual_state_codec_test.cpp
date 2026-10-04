// Spec 002, BR-030 (RF-3.6): framing and encoding of the visual state
// payload (contracts.md C4). Every section the session owns round-trips, and a
// malformed payload is rejected instead of half-read.
#include "session/visual_state_codec.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

namespace sv = ayther::session::visual;
namespace vs = ayther::engine::visual_state;
using Section = vs::VisualStateSection;

std::vector<std::byte> bytes(std::initializer_list<int> values) {
  std::vector<std::byte> out;
  for (const int v : values)
    out.push_back(static_cast<std::byte>(v));
  return out;
}
} // namespace

int main() try {
  // Framing: two sections in order split back into their bodies.
  {
    std::vector<std::byte> payload;
    sv::append_section(payload, Section::sprite_tweens, bytes({1, 2, 3}));
    sv::append_section(payload, Section::level_camera, bytes({9}));
    const std::uint32_t declared =
        vs::visual_state_section(Section::sprite_tweens) |
        vs::visual_state_section(Section::level_camera);
    sv::SectionBodies bodies;
    check(sv::split_sections(payload, declared, bodies) &&
              bodies[0].size() == 3 && bodies[2].size() == 1 &&
              bodies[2][0] == std::byte{9},
          "RF-3.6: sections split back into their bodies");
    check(!sv::split_sections(
              payload, declared | vs::visual_state_section(Section::cinematic),
              bodies),
          "a declared section missing from the payload is rejected");
    check(!sv::split_sections(payload,
                              vs::visual_state_section(Section::sprite_tweens),
                              bodies),
          "a section the header does not declare is rejected");
    std::vector<std::byte> truncated(payload.begin(), payload.end() - 1);
    check(!sv::split_sections(truncated, declared, bodies),
          "a truncated body is rejected");
    std::vector<std::byte> reversed;
    sv::append_section(reversed, Section::level_camera, bytes({9}));
    sv::append_section(reversed, Section::sprite_tweens, bytes({1}));
    check(!sv::split_sections(reversed, declared, bodies),
          "sections out of order are rejected");
    std::vector<std::byte> repeated;
    sv::append_section(repeated, Section::sprite_tweens, bytes({1}));
    sv::append_section(repeated, Section::sprite_tweens, bytes({1}));
    check(!sv::split_sections(repeated,
                              vs::visual_state_section(Section::sprite_tweens),
                              bodies),
          "a repeated section is rejected");
  }

  // Every session-owned section round-trips.
  {
    sv::ScreenRecognition screen{0x1234, 0x5678, 1};
    sv::ScreenRecognition screen_back;
    check(sv::decode(sv::encode(screen), screen_back) &&
              screen_back.active == 0x1234 && screen_back.candidate == 0x5678 &&
              screen_back.streak == 1,
          "RF-3.6: screen_recognition round-trips");

    sv::LevelCamera camera;
    camera.cam_x = {-5, 1000};
    camera.cam_y = {7, -8};
    camera.prev_h = {-1, 2};
    camera.prev_v = {3, -4};
    camera.last_frame = 99;
    camera.valid = true;
    camera.pano_last_id = 42;
    camera.pano_last_x = -16;
    camera.pano_last_y = 32;
    sv::LevelCamera camera_back;
    check(sv::decode(sv::encode(camera), camera_back) &&
              camera_back.cam_x == camera.cam_x &&
              camera_back.cam_y == camera.cam_y &&
              camera_back.prev_h == camera.prev_h &&
              camera_back.prev_v == camera.prev_v &&
              camera_back.last_frame == 99 && camera_back.valid &&
              camera_back.pano_last_id == 42 &&
              camera_back.pano_last_x == -16 && camera_back.pano_last_y == 32,
          "RF-3.6: level_camera round-trips, Panorama continuity included");

    sv::PaletteLuma luma{{0.25, 0.5, 0.75, 1.0}};
    sv::PaletteLuma luma_back;
    check(sv::decode(sv::encode(luma), luma_back) &&
              luma_back.peak == luma.peak,
          "RF-3.6: palette_luma round-trips bit for bit");
    luma.peak[2] = std::numeric_limits<double>::quiet_NaN();
    check(!sv::decode(sv::encode(luma), luma_back),
          "a NaN luminance peak is rejected");

    sv::PreviousAudioMask mask{0x3FF};
    sv::PreviousAudioMask mask_back;
    check(sv::decode(sv::encode(mask), mask_back) && mask_back.mask == 0x3FF,
          "RF-3.6: previous_audio_mask round-trips");

    const std::vector<sv::PlaneSequenceClock> clocks{{3, 10, 20}, {7, -1, -1}};
    std::vector<sv::PlaneSequenceClock> clocks_back;
    check(sv::decode(sv::encode(clocks), clocks_back) &&
              clocks_back.size() == 2 && clocks_back[0].anchor == 10 &&
              clocks_back[1].id == 7,
          "RF-3.6: plane_sequence_clocks round-trips");
    const std::vector<sv::PlaneSequenceClock> unsorted{{7, 0, 0}, {3, 0, 0}};
    check(!sv::decode(sv::encode(unsorted), clocks_back),
          "clocks out of id order are rejected");

    sv::Cinematic cinematic;
    cinematic.active = 5;
    cinematic.step = 2;
    cinematic.gap = 1;
    cinematic.last_frame = 77;
    cinematic.video_kinematic = 5;
    cinematic.video_step = 2;
    cinematic.video_anchor = 60;
    cinematic.audio_kinematic = 5;
    cinematic.audio_anchor = 40;
    cinematic.audio_on = true;
    cinematic.audio_last_frame = 76;
    cinematic.audio_still = 3;
    cinematic.audio_gain = 0.5F;
    sv::Cinematic cinematic_back;
    check(sv::decode(sv::encode(cinematic), cinematic_back) &&
              cinematic_back.active == 5 && cinematic_back.step == 2 &&
              cinematic_back.last_frame == 77 &&
              cinematic_back.video_anchor == 60 && cinematic_back.audio_on &&
              cinematic_back.audio_still == 3 &&
              cinematic_back.audio_gain == 0.5F,
          "RF-3.6: cinematic round-trips");

    const std::vector<sv::HdAnimationPhase> phases{{1, 0, 10}, {2, -1, 0}};
    std::vector<sv::HdAnimationPhase> phases_back;
    check(sv::decode(sv::encode(phases), phases_back) &&
              phases_back.size() == 2 &&
              phases_back[0].pose_start_frame == 10 &&
              phases_back[1].last_pose == -1,
          "RF-3.6: hd_animation_phase round-trips");
    const std::vector<sv::HdAnimationPhase> bad_pose{{1, -2, 0}};
    check(!sv::decode(sv::encode(bad_pose), phases_back),
          "a pose index below -1 is rejected");
  }

  // The Panorama tint reference round-trips bit for bit (DI-6).
  {
    sv::PanoramaTint tint;
    tint.id = 9;
    tint.ref_luma = 0.42;
    tint.ref_w = {0.1, 0.2, 0.3, 0.4};
    tint.ref_ch = {0.5, 0.6, 0.7};
    tint.ref_chroma = true;
    tint.ref_peak = 0.45;
    const std::vector<sv::PanoramaTint> tints{tint};
    std::vector<sv::PanoramaTint> back;
    check(sv::decode(sv::encode(tints), back) && back.size() == 1 &&
              back[0].id == 9 && back[0].ref_luma == 0.42 &&
              back[0].ref_w == tint.ref_w && back[0].ref_ch == tint.ref_ch &&
              back[0].ref_chroma && back[0].ref_peak == 0.45,
          "RF-3.6: panorama_tint round-trips bit for bit");
    std::vector<sv::PanoramaTint> twice{tint, tint};
    check(!sv::decode(sv::encode(twice), back),
          "a Panorama listed twice is rejected");
    tint.ref_peak = -1.0;
    const std::vector<sv::PanoramaTint> negative{tint};
    check(!sv::decode(sv::encode(negative), back),
          "a negative reference is rejected");
  }

  // A body with trailing bytes or cut short is rejected.
  {
    std::vector<std::byte> body = sv::encode(sv::PreviousAudioMask{1});
    body.push_back(std::byte{0});
    sv::PreviousAudioMask mask;
    check(!sv::decode(body, mask), "a body with trailing bytes is rejected");
    body.resize(2);
    check(!sv::decode(body, mask), "a body cut short is rejected");
    sv::ScreenRecognition screen;
    check(!sv::decode(sv::encode(sv::ScreenRecognition{1, 1, -1}), screen),
          "a negative Picture streak is rejected");
  }

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
