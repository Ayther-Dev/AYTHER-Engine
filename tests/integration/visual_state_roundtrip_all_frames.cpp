// Spec 002, BR-031 (RF-3.6, RF-5.2): the visual state restoration oracle
// (contracts.md C4, «Equivalencia»).
//
// A synthetic take of 600 frames with the test core and recorded inputs. For
// every frame k of the take, restoring the core state and the visual state of
// k and producing k+1 to k+30 with the same inputs gives, frame by frame, the
// same scene, the same replacements, the same claims and the same render
// observation (C3) as linear production.
//
// The scene is configured so the sections carry state through the take:
//   - level_camera: the camera tracks the planes every frame;
//   - palette_luma: the test core's palette brightens over the take;
//   - palette_signature: the CRAM changes every frame;
//   - animation_grouper: every SAT slot has a rolling history;
//   - sprite_tweens: a sprite changes content in the take, which switches
//     its instance between two poses joined by an in-between;
//   - hd_animation_phase: the same two contents are the poses of an HD clip;
//   - plane_sequence_clocks: two stable plane cells drive an Animation;
//   - screen_recognition: a Picture of one frame of the take is entered.
// Sections the test core cannot drive are reported, not faked: a Kinematic
// needs a Picture confirmed twice in a row (the test core redraws its planes
// every frame; BR-027b covers it by replay), a Panorama never anchors, and
// the audio mask stays 0 without audio.
//
// The take starts after 300 warm-up frames (DI-7): early on most VRAM is
// still zero, sprites 0 and 1 then share a hash in two SAT slots and the
// grouper's tie between slots depends on hash-map order, which no restore can
// reproduce. The oracle checks that the take is free of that tie.
//
// The test core's state is global to the library: one session runs the
// linear take and then every restore, never two sessions in turns.
#include <ayther/ayther_session.h>
#include <ayther/engine/render_observer.hpp>
#include <ayther/engine/visual_state.hpp>

#include "../../tools/common/synth_rom.h"
#include "frame_view_digest.h"
#include "session/visual_state_codec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

using ayther::AytherSession;
using ayther::FrameView;
namespace ro = ayther::engine::render_observation;
namespace vs = ayther::engine::visual_state;
namespace sv = ayther::session::visual;

constexpr std::uint64_t kWarmUp = 300;
constexpr std::uint64_t kTake = 600;
constexpr std::uint64_t kAfter = 30;
constexpr std::uint64_t kLast = kWarmUp + kTake + kAfter;

// Recorded inputs of the synthetic take: a deterministic pattern.
std::uint16_t input_at(std::uint64_t frame) {
  return static_cast<std::uint16_t>(((frame * 2654435761ULL) >> 13) & 0x0FFFU);
}

// Digest of the render observation of one frame (C3).
class ObservationDigest final : public ro::RenderObserver {
public:
  std::uint64_t last = 0;
  void on_render_frame(const ro::RenderFrameView &frame) noexcept override {
    ayther::test::Digest d;
    d.value(frame.frame.availability);
    d.value(frame.frame.emulation_frame);
    d.value(frame.composability);
    d.value(frame.occurrences_total);
    d.value(frame.replacements_total);
    for (const ro::OccurrenceView &o : frame.occurrences) {
      d.value(o.id.index);
      d.value(o.id.slot);
      d.value(o.id.chain);
      d.value(o.identity_hash);
      d.value(o.x);
      d.value(o.y);
      d.value(o.w);
      d.value(o.h);
      d.value(o.priority);
      d.value(o.status);
      d.value(o.replacement);
      field(d, o.pose);
      field(d, o.not_applied_reason);
    }
    for (const ro::ReplacementView &r : frame.replacements) {
      d.value(r.index);
      d.bytes(r.kind.data(), r.kind.size());
      d.bytes(r.pose_key.data(), r.pose_key.size());
      d.bytes(r.asset.data(), r.asset.size());
      for (const ro::OccurrenceId &m : r.members) {
        d.value(m.index);
        d.value(m.slot);
        d.value(m.chain);
      }
      d.value(r.render_availability);
    }
    last = d.result();
  }

private:
  static void field(ayther::test::Digest &d, const ro::FieldView &f) {
    d.value(f.availability);
    if (const auto *text = std::get_if<std::string_view>(&f.value))
      d.bytes(text->data(), text->size());
  }
};

struct Frame {
  std::uint64_t scene = 0;
  std::uint64_t observation = 0;
  bool tween_drawn = false;  // a sub shows an in-between drawing
  bool hd_animation = false; // the HD clip drew a frame
};

// One step with the recorded input: the frame's scene and observation.
Frame produce(AytherSession &s, ObservationDigest &observer,
              std::uint64_t frame) {
  s.set_input(0, input_at(frame));
  const FrameView &v = s.step();
  Frame out;
  out.scene = ayther::test::scene_digest(v);
  for (std::uint32_t i = 0; v.sprite_subs != nullptr && i < v.sprite_sub_count;
       ++i)
    out.tween_drawn =
        out.tween_drawn ||
        std::string_view(v.sprite_subs[i].asset_path).rfind("graphics/t", 0) ==
            0;
  out.hd_animation = v.anim_frame_count > 0;
  s.publish_render_observation(nullptr);
  out.observation = observer.last;
  return out;
}

std::unique_ptr<AytherSession> open_session(const std::string &rom,
                                            ObservationDigest *observer) {
  AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = rom;
  config.enable_audio = false;
  config.derive_core_pack = false;
  config.render_observer = observer;
  auto created = AytherSession::create(config);
  if (!created) {
    std::fprintf(stderr, "[FAIL] %s\n", created.error.message.c_str());
    return nullptr;
  }
  return std::move(*created);
}

// What a discovery run of the same take finds to configure the scene.
struct Discovery {
  bool sprite_switch = false;
  std::uint64_t old_hash = 0;
  std::uint64_t new_hash = 0;
  std::uint64_t switch_frame = 0;
  std::uint64_t cell_a = 0;
  std::uint64_t cell_b = 0;
  std::vector<AytherSession::ScreenCell> picture;
  std::uint64_t picture_frame = 0;
  std::uint64_t tie_frames = 0; // frames of the take with the DI-7 tie
};

Discovery discover(const std::string &rom) {
  Discovery found;
  auto s = open_session(rom, nullptr);
  if (!s)
    return found;
  std::vector<std::uint64_t> sprite0;
  std::unordered_map<std::uint64_t, std::uint64_t> cell_frames;
  for (std::uint64_t f = 1; f <= kLast; ++f) {
    s->set_input(0, input_at(f));
    const FrameView &v = s->step();
    sprite0.push_back(v.sprite_occ_count > 0 ? v.sprite_occs[0].hash : 0);
    if (f <= kWarmUp)
      continue;
    // The DI-7 tie: one content at two SAT slots.
    std::unordered_map<std::uint64_t, std::uint8_t> slot_of;
    bool tie = false;
    for (std::uint32_t i = 0; i < v.sprite_occ_count; ++i) {
      const AytherSpriteOccurrence &o = v.sprite_occs[i];
      const auto [it, inserted] = slot_of.emplace(o.hash, o.slot);
      tie = tie || (!inserted && it->second != o.slot);
    }
    found.tie_frames += tie ? 1 : 0;
    for (std::uint32_t i = 0; i < v.plane_cell_count; ++i)
      if (v.plane_cells[i].plane == 0)
        ++cell_frames[v.plane_cells[i].hash];
    if (f == kWarmUp + 200) {
      found.picture_frame = f;
      for (std::uint32_t i = 0; i < v.plane_cell_count; ++i) {
        const ayther::PlaneCellHit &c = v.plane_cells[i];
        if (c.plane == 0 && c.screen_x >= 0 && c.screen_y >= 0)
          found.picture.push_back({c.hash, 0,
                                   static_cast<std::uint8_t>(c.screen_x / 8),
                                   static_cast<std::uint8_t>(c.screen_y / 8)});
      }
    }
  }
  // A change of sprite 0's content inside the take, with 12 stable frames
  // on each side.
  for (std::uint64_t f = kWarmUp + 12; f + 12 < kWarmUp + kTake; ++f) {
    const std::uint64_t before = sprite0[f - 1];
    const std::uint64_t after = sprite0[f];
    if (before == after || before == 0 || after == 0)
      continue;
    bool stable = true;
    for (std::uint64_t g = f - 12; g < f && stable; ++g)
      stable = sprite0[g] == before;
    for (std::uint64_t g = f; g < f + 12 && stable; ++g)
      stable = sprite0[g] == after;
    if (stable) {
      found.sprite_switch = true;
      found.old_hash = before;
      found.new_hash = after;
      found.switch_frame = f + 1; // sprite0 is 0-based
      break;
    }
  }
  // Two plane A contents present in (almost) every frame of the take.
  for (const auto &[hash, frames] : cell_frames) {
    if (frames + 10 < kTake + kAfter)
      continue;
    if (found.cell_a == 0)
      found.cell_a = hash;
    else if (found.cell_b == 0 && hash != found.cell_a)
      found.cell_b = hash;
  }
  return found;
}

void configure(AytherSession &s, const Discovery &found) {
  if (found.sprite_switch) {
    AytherSession::PosePreview old_pose;
    old_pose.hashes = {found.old_hash};
    old_pose.rel_x = {0};
    old_pose.rel_y = {0};
    old_pose.asset = "graphics/old.png";
    AytherSession::PosePreview new_pose = old_pose;
    new_pose.hashes = {found.new_hash};
    new_pose.asset = "graphics/new.png";
    s.set_pose_preview({old_pose, new_pose});
    AytherSession::TweenPreview tween;
    tween.from = "graphics/old.png";
    tween.target = "graphics/new.png";
    tween.frames = {"graphics/t1.png", "graphics/t2.png", "graphics/t3.png"};
    tween.ticks = 3;
    s.set_tween_preview({tween});
    std::array<ayther::HdPose, 2> poses{};
    poses[0].pose = found.old_hash;
    poses[0].src_w = poses[0].src_h = 8;
    poses[0].anchor = {0, 0, 8, 8};
    poses[0].duration_ticks = 6;
    poses[1].pose = found.new_hash;
    poses[1].src_x = 8;
    poses[1].src_w = poses[1].src_h = 8;
    poses[1].anchor = {24, 0, 8, 8};
    poses[1].duration_ticks = 6;
    s.define_animation(0xC1, "graphics/sheet.png", poses.data(), 2, 1);
  }
  if (found.cell_a != 0 && found.cell_b != 0) {
    const AytherSession::PlaneSetMember a{found.cell_a, 0, 0};
    const AytherSession::PlaneSetMember b{found.cell_b, 0, 0};
    s.define_plane_set(1, 0, 1, 1, &a, 1, "graphics/set1.png");
    s.define_plane_set(2, 0, 1, 1, &b, 1, "graphics/set2.png");
    const AytherSession::PlaneSequenceStep steps[2] = {
        {1, "graphics/step1.png", 4}, {2, "graphics/step2.png", 4}};
    s.define_plane_sequence(10, steps, 2);
  }
  if (!found.picture.empty())
    s.define_screen(77, 1, found.picture.data(),
                    static_cast<std::uint32_t>(found.picture.size()), 0.92F,
                    0.08F, "graphics/picture.png");
}

struct Checkpoint {
  std::vector<std::uint8_t> core;
  vs::VisualState visual;
};
} // namespace

int main() try {
  const std::string rom = ayther::synth::canonical_rom_path();
  check(!rom.empty(),
        "the synthetic ROM is written to the temporary directory");
  if (rom.empty())
    return 1;

  const Discovery found = discover(rom);
  check(found.tie_frames == 0,
        "DI-7: the take has no content at two SAT slots (no grouper tie)");
  check(found.sprite_switch,
        "the take has a sprite that changes content (tweens and HD phase)");
  check(found.cell_a != 0 && found.cell_b != 0,
        "the take has two stable plane cells (Animation clock)");
  check(!found.picture.empty(), "the take has a frame to make a Picture of");

  ObservationDigest observer;
  auto session = open_session(rom, &observer);
  if (!session)
    return 1;
  AytherSession &s = *session;
  configure(s, found);

  // Linear production, with a checkpoint at every frame of the take.
  std::vector<Frame> linear(kLast + 1);
  std::vector<Checkpoint> checkpoints(kTake);
  for (std::uint64_t f = 1; f <= kLast; ++f) {
    linear[f] = produce(s, observer, f);
    if (f > kWarmUp && f <= kWarmUp + kTake) {
      Checkpoint &c = checkpoints[f - kWarmUp - 1];
      (void)s.serialize(c.core);
      c.visual = s.export_visual_state();
    }
  }

  // Which sections carried changing state through the take.
  std::array<std::uint32_t, sv::kSectionCount> changes{};
  for (std::size_t i = 1; i < checkpoints.size(); ++i) {
    sv::SectionBodies now;
    sv::SectionBodies previous;
    if (!sv::split_sections(checkpoints[i].visual.payload,
                            checkpoints[i].visual.header.sections, now) ||
        !sv::split_sections(checkpoints[i - 1].visual.payload,
                            checkpoints[i - 1].visual.header.sections,
                            previous))
      continue;
    for (std::size_t b = 0; b < sv::kSectionCount; ++b)
      if (!std::equal(now[b].begin(), now[b].end(), previous[b].begin(),
                      previous[b].end()))
        ++changes[b];
  }
  static constexpr std::array<const char *, sv::kSectionCount> kNames{
      "sprite_tweens",      "screen_recognition",    "level_camera",
      "palette_luma",       "previous_audio_mask",   "palette_signature",
      "animation_grouper",  "plane_sequence_clocks", "cinematic",
      "hd_animation_phase", "panorama_tint"};
  for (std::size_t b = 0; b < sv::kSectionCount; ++b)
    std::printf("visual_state_roundtrip section=%s changes=%u\n", kNames[b],
                changes[b]);
  std::uint64_t tween_frames = 0;
  std::uint64_t hd_frames = 0;
  for (std::uint64_t f = kWarmUp + 1; f <= kLast; ++f) {
    tween_frames += linear[f].tween_drawn ? 1 : 0;
    hd_frames += linear[f].hd_animation ? 1 : 0;
  }
  std::printf(
      "visual_state_roundtrip tween_frames=%llu hd_animation_frames=%llu\n",
      static_cast<unsigned long long>(tween_frames),
      static_cast<unsigned long long>(hd_frames));
  check(tween_frames > 0 && hd_frames > 0,
        "an in-between is drawn and the HD clip plays during the take");
  bool driven = true;
  for (const std::size_t b : {0U, 1U, 2U, 3U, 5U, 6U, 7U, 9U})
    driven = driven && changes[b] > 0;
  check(driven,
        "the take drives sprite_tweens, screen_recognition, level_camera, "
        "palette_luma, palette_signature, animation_grouper, "
        "plane_sequence_clocks and hd_animation_phase");

  // Every checkpoint: restore core and visual state of k, produce k+1..k+30.
  std::uint64_t restored = 0;
  std::uint64_t compared = 0;
  std::uint64_t scene_mismatch = 0;
  std::uint64_t observation_mismatch = 0;
  std::uint64_t first_bad = 0;
  for (std::uint64_t i = 0; i < kTake; ++i) {
    const std::uint64_t k = kWarmUp + 1 + i;
    const Checkpoint &c = checkpoints[i];
    if (!s.unserialize(c.core))
      continue;
    const vs::VisualStateRestoreResult r = s.restore_visual_state(
        c.visual, c.visual.header.game_state_identity, k);
    if (!r.restored())
      continue;
    ++restored;
    for (std::uint64_t f = k + 1; f <= k + kAfter; ++f) {
      const Frame got = produce(s, observer, f);
      ++compared;
      if (got.scene != linear[f].scene) {
        ++scene_mismatch;
        if (first_bad == 0)
          first_bad = k;
      }
      if (got.observation != linear[f].observation) {
        ++observation_mismatch;
        if (first_bad == 0)
          first_bad = k;
      }
    }
  }
  std::printf("visual_state_roundtrip restores=%llu frames=%llu "
              "scene_mismatches=%llu observation_mismatches=%llu "
              "first_bad_checkpoint=%llu\n",
              static_cast<unsigned long long>(restored),
              static_cast<unsigned long long>(compared),
              static_cast<unsigned long long>(scene_mismatch),
              static_cast<unsigned long long>(observation_mismatch),
              static_cast<unsigned long long>(first_bad));
  check(restored == kTake,
        "RF-3.6: the core and visual state of every frame of the take restore");
  check(compared == kTake * kAfter && scene_mismatch == 0,
        "RF-3.6, RF-5.2: for every k, k+1..k+30 draw the same scene, "
        "replacements and claims as linear production");
  check(observation_mismatch == 0,
        "RF-3.6, RF-5.2: for every k, k+1..k+30 publish the same render "
        "observation as linear production");

  // Control: the same checkpoints restoring only the core state. The visual
  // state left by the previous run is stale, and production must diverge,
  // or the oracle would not be able to see a missing section.
  std::uint64_t control_mismatch = 0;
  for (std::uint64_t i = 0; i < kTake; i += 25) {
    const std::uint64_t k = kWarmUp + 1 + i;
    if (!s.unserialize(checkpoints[i].core))
      continue;
    for (std::uint64_t f = k + 1; f <= k + kAfter; ++f) {
      const Frame got = produce(s, observer, f);
      control_mismatch += got.scene != linear[f].scene ? 1 : 0;
    }
  }
  std::printf("visual_state_roundtrip control_core_only_mismatches=%llu\n",
              static_cast<unsigned long long>(control_mismatch));
  check(control_mismatch > 0,
        "control: restoring only the core state does not reproduce linear "
        "production");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
