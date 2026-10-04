// Spec 002, BR-027, BR-027b, BR-028 and BR-030 (RF-3.6): the visual state
// (contracts.md C4) of a live session with the in-repository test core.
//
// Two facts about the test core shape these scenes:
//   - Its state is global to the loaded library, so two sessions must never be
//     stepped in turns: each run below finishes before the next one starts,
//     and every run first restores the core state it needs.
//   - It rewrites VRAM and CRAM every frame, so a Picture or a pose only
//     matches again when the same frame is produced again. Such scenes replay
//     a frame from its saved core state ("replay" below); the visual state
//     carries over a replay exactly as over a new frame, and a restore has to
//     reproduce that.
#include <ayther/ayther_session.h>
#include <ayther/engine/visual_state.hpp>

#include "../../tools/common/synth_rom.h"
#include "frame_view_digest.h"
#include "session/visual_state_codec.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
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
namespace vs = ayther::engine::visual_state;
namespace sv = ayther::session::visual;
using Code = vs::VisualStateRestoreCode;
using Section = vs::VisualStateSection;
using Configure = std::function<void(AytherSession &)>;

std::string g_rom;

std::unique_ptr<AytherSession> open_session() {
  AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = g_rom;
  config.enable_audio = false;
  config.derive_core_pack = false;
  auto created = AytherSession::create(config);
  if (!created) {
    std::fprintf(stderr, "[FAIL] %s\n", created.error.message.c_str());
    return nullptr;
  }
  return std::move(*created);
}

std::vector<std::uint8_t> save(const AytherSession &s) {
  std::vector<std::uint8_t> state;
  (void)s.serialize(state);
  return state;
}

// Produces again the frame that follows the core state `before`.
const FrameView &replay(AytherSession &s,
                        const std::vector<std::uint8_t> &before) {
  (void)s.unserialize(before);
  return s.step();
}

template <typename T>
bool section(const vs::VisualState &state, Section which, T &out) {
  sv::SectionBodies bodies;
  return sv::split_sections(state.payload, state.header.sections, bodies) &&
         sv::decode(bodies[sv::section_index(which)], out);
}

// `state` with one section body replaced.
vs::VisualState with_section(const vs::VisualState &state, Section which,
                             const std::vector<std::byte> &body) {
  sv::SectionBodies bodies;
  vs::VisualState out = state;
  out.payload.clear();
  if (!sv::split_sections(state.payload, state.header.sections, bodies))
    return out;
  for (std::size_t i = 0; i < sv::kSectionCount; ++i) {
    const auto bit = static_cast<Section>(1U << i);
    if (i == sv::section_index(which))
      sv::append_section(out.payload, bit, body);
    else
      sv::append_section(out.payload, bit, bodies[i]);
  }
  return out;
}

// The plane A cells of a frame as a Picture.
std::vector<AytherSession::ScreenCell> plane_a_cells(const FrameView &v) {
  std::vector<AytherSession::ScreenCell> cells;
  for (std::uint32_t i = 0; i < v.plane_cell_count; ++i) {
    const ayther::PlaneCellHit &c = v.plane_cells[i];
    if (c.plane != 0 || c.screen_x < 0 || c.screen_y < 0)
      continue;
    cells.push_back({c.hash, c.plane, static_cast<std::uint8_t>(c.screen_x / 8),
                     static_cast<std::uint8_t>(c.screen_y / 8)});
  }
  return cells;
}

struct Restored {
  std::unique_ptr<AytherSession> session;
  vs::VisualStateRestoreResult result;
};

// A new session configured like the original, moved to the core state `core`
// and given `state` (or nothing, for the controls).
Restored restore_into(const Configure &configure,
                      const std::vector<std::uint8_t> &core,
                      const vs::VisualState *state) {
  Restored r{open_session(), {}};
  if (!r.session)
    return r;
  configure(*r.session);
  (void)r.session->unserialize(core);
  if (state != nullptr)
    r.result = r.session->restore_visual_state(
        *state, state->header.game_state_identity,
        state->header.emulation_frame);
  return r;
}

std::vector<std::uint64_t> scenes(AytherSession &s, int frames) {
  std::vector<std::uint64_t> out;
  for (int f = 0; f < frames; ++f)
    out.push_back(ayther::test::scene_digest(s.step()));
  return out;
}

void api_and_rejections() {
  auto a = open_session();
  if (!a)
    return;
  for (int f = 0; f < 20; ++f)
    (void)a->step();
  const vs::VisualState state = a->export_visual_state();
  const std::string identity = a->game_state_identity();
  const std::vector<std::uint8_t> core = save(*a);
  check(state.header.version == vs::kVisualStateVersion &&
            state.header.emulation_frame == 20 &&
            state.header.game_state_identity == identity &&
            identity.size() == 64 &&
            state.header.sections == vs::kVisualStateRequiredSections &&
            state.header.script_state == vs::ScriptState::none,
        "RF-3.6: the export names version 1.0, the core state, the frame "
        "and every section");
  const std::vector<std::uint64_t> linear = scenes(*a, 30);
  a.reset();

  auto b = open_session();
  if (!b)
    return;
  (void)b->unserialize(core);
  check(b->game_state_identity() == identity,
        "the identity is that of the core state, whatever session holds it");
  const vs::VisualState untouched = b->export_visual_state();
  const auto unchanged = [&] {
    const vs::VisualState now = b->export_visual_state();
    return now.payload == untouched.payload &&
           now.header.emulation_frame == untouched.header.emulation_frame;
  };
  const auto rejects = [&](const vs::VisualState &bad, std::string_view id,
                           std::uint64_t frame, Code want, const char *what) {
    const vs::VisualStateRestoreResult r =
        b->restore_visual_state(bad, id, frame);
    check(r.code == want && unchanged(), what);
  };
  vs::VisualState bad = state;
  bad.header.version = {2, 0};
  rejects(bad, identity, 20, Code::unsupported_version,
          "RF-3.6: another version is rejected and nothing changes");
  rejects(state, "0123", 20, Code::identity_mismatch,
          "RF-3.6: another core state identity is rejected and nothing "
          "changes");
  rejects(state, identity, 21, Code::frame_mismatch,
          "RF-3.6: another frame is rejected and nothing changes");
  bad = state;
  bad.header.sections &= ~vs::visual_state_section(Section::cinematic);
  rejects(bad, identity, 20, Code::missing_sections,
          "a missing section is rejected and nothing changes");
  bad = state;
  bad.header.sections |= 1U << 20;
  rejects(bad, identity, 20, Code::unknown_sections,
          "an unknown section is rejected and nothing changes");
  bad = state;
  bad.payload.pop_back();
  rejects(bad, identity, 20, Code::invalid_payload,
          "a truncated payload is rejected and nothing changes");
  bad = state;
  // Body of sprite_tweens: section header (8) + magic (8), then the version.
  bad.payload[8 + 8] = std::byte{0x7F};
  rejects(bad, identity, 20, Code::invalid_payload,
          "a corrupt core-owned section is rejected and nothing changes");

  const vs::VisualStateRestoreResult ok =
      b->restore_visual_state(state, identity, 20);
  const vs::VisualState after = b->export_visual_state();
  check(ok.restored() && after.payload == state.payload &&
            after.header.emulation_frame == 20,
        "RF-3.6: a valid state is restored as exported, at its frame");
  check(scenes(*b, 30) == linear,
        "RF-3.6: the 30 frames after the restore match linear production");
  b.reset();

  auto scripted = open_session();
  if (scripted) {
    check(static_cast<bool>(
              scripted->load_script("ayther.on_frame(function() end)", "wp2")),
          "a pack script that runs every frame loads");
    check(scripted->export_visual_state().header.script_state ==
              vs::ScriptState::not_exportable,
          "its Lua state is declared not_exportable");
  }
}

// BR-027: restore in the middle of a Picture entry and with the level camera
// tracking; the following frames confirm the Picture and keep the camera as
// linear production does.
void picture_and_camera() {
  auto a = open_session();
  if (!a)
    return;
  for (int f = 0; f < 40; ++f)
    (void)a->step();
  const std::vector<std::uint8_t> before = save(*a);
  const std::vector<AytherSession::ScreenCell> cells = plane_a_cells(a->step());
  check(!cells.empty(), "the frame has plane A cells to make a Picture of");
  const Configure configure = [cells](AytherSession &s) {
    s.define_screen(77, 1, cells.data(),
                    static_cast<std::uint32_t>(cells.size()), 0.92F, 0.08F,
                    "graphics/picture.png");
  };
  configure(*a);
  const std::uint64_t k = replay(*a, before).frame_index;
  const vs::VisualState state = a->export_visual_state();
  const std::vector<std::uint8_t> core = save(*a);
  sv::ScreenRecognition screen;
  sv::LevelCamera camera;
  check(section(state, Section::screen_recognition, screen) &&
            screen.candidate == 77 && screen.streak == 1 && screen.active == 0,
        "RF-3.6: the state is taken in the middle of the Picture entry");
  check(section(state, Section::level_camera, camera) && camera.valid &&
            camera.last_frame == k,
        "RF-3.6: and with the level camera tracking frame by frame");

  // The same frame again (confirms the Picture), then new frames.
  const auto run = [&](AytherSession &s, std::vector<std::uint64_t> &match) {
    std::vector<std::uint64_t> scene;
    const FrameView &confirm = replay(s, before);
    match.push_back(confirm.screen_match_id);
    scene.push_back(ayther::test::scene_digest(confirm));
    for (int f = 0; f < 5; ++f) {
      const FrameView &v = s.step();
      match.push_back(v.screen_match_id);
      scene.push_back(ayther::test::scene_digest(v));
    }
    return scene;
  };
  std::vector<std::uint64_t> want_match;
  const std::vector<std::uint64_t> want_scene = run(*a, want_match);
  a.reset();
  check(want_match[0] == 77, "linear production confirms the Picture");

  Restored b = restore_into(configure, core, &state);
  std::vector<std::uint64_t> got_match;
  std::vector<std::uint64_t> got_scene;
  if (b.session)
    got_scene = run(*b.session, got_match);
  b.session.reset();
  check(b.result.restored() && got_match == want_match &&
            got_scene == want_scene,
        "RF-3.6: after the restore the Picture and the camera follow linear "
        "production");

  Restored c = restore_into(configure, core, nullptr);
  std::vector<std::uint64_t> control_match;
  if (c.session)
    (void)run(*c.session, control_match);
  check(!control_match.empty() && control_match[0] != 77,
        "control: without the state the Picture entry starts over");
}

// BR-027b: a plane Animation clock restored in mid-cycle, and a Kinematic
// restored while it runs.
void sequences_and_cinematic() {
  auto a = open_session();
  if (!a)
    return;
  for (int f = 0; f < 30; ++f)
    (void)a->step();
  const FrameView &seed = a->step();
  // Two plane A cells with different content: one Object each.
  std::uint64_t first = 0;
  std::uint64_t second = 0;
  for (std::uint32_t i = 0; i < seed.plane_cell_count && second == 0; ++i) {
    const ayther::PlaneCellHit &c = seed.plane_cells[i];
    if (c.plane != 0)
      continue;
    if (first == 0)
      first = c.hash;
    else if (c.hash != first)
      second = c.hash;
  }
  check(first != 0 && second != 0, "the frame has two distinct plane cells");
  const Configure configure_sequence = [first, second](AytherSession &s) {
    const AytherSession::PlaneSetMember m1{first, 0, 0};
    const AytherSession::PlaneSetMember m2{second, 0, 0};
    s.define_plane_set(1, 0, 1, 1, &m1, 1, "graphics/set1.png");
    s.define_plane_set(2, 0, 1, 1, &m2, 1, "graphics/set2.png");
    const AytherSession::PlaneSequenceStep steps[2] = {
        {1, "graphics/step1.png", 3}, {2, "graphics/step2.png", 3}};
    s.define_plane_sequence(10, steps, 2);
  };
  configure_sequence(*a);
  for (int f = 0; f < 4; ++f)
    (void)a->step();
  const vs::VisualState state = a->export_visual_state();
  const std::vector<std::uint8_t> core = save(*a);
  std::vector<sv::PlaneSequenceClock> clocks;
  check(section(state, Section::plane_sequence_clocks, clocks) &&
            clocks.size() == 1 && clocks[0].id == 10 && clocks[0].anchor >= 0,
        "RF-3.6: the state carries the Animation clock in mid-cycle");
  const std::vector<std::uint64_t> want = scenes(*a, 12);
  a.reset();
  Restored b = restore_into(configure_sequence, core, &state);
  const std::vector<std::uint64_t> got =
      b.session ? scenes(*b.session, 12) : std::vector<std::uint64_t>{};
  b.session.reset();
  check(b.result.restored() && got == want,
        "RF-3.6: after the restore the Animation shows the steps of linear "
        "production");
  Restored c = restore_into(configure_sequence, core, nullptr);
  const std::vector<std::uint64_t> control =
      c.session ? scenes(*c.session, 12) : std::vector<std::uint64_t>{};
  c.session.reset();
  check(control != want, "control: without the clock the cycle re-anchors");

  // Kinematic: its first Picture confirmed by producing its frame twice.
  auto k = open_session();
  if (!k)
    return;
  for (int f = 0; f < 50; ++f)
    (void)k->step();
  const std::vector<std::uint8_t> before = save(*k);
  const std::vector<AytherSession::ScreenCell> cells = plane_a_cells(k->step());
  const Configure configure_kinematic = [cells](AytherSession &s) {
    s.define_screen(88, 1, cells.data(),
                    static_cast<std::uint32_t>(cells.size()), 0.92F, 0.08F,
                    "graphics/picture.png");
    const AytherSession::KinematicStep steps[2] = {
        {88, "graphics/shot1.png", 0}, {89, "graphics/shot2.png", 0}};
    s.define_kinematic(500, steps, 2, 12);
  };
  configure_kinematic(*k);
  (void)replay(*k, before);
  const bool started = replay(*k, before).kinematic_id == 500;
  check(started, "the Kinematic starts on its Picture");
  const vs::VisualState kin_state = k->export_visual_state();
  const std::vector<std::uint8_t> kin_core = save(*k);
  sv::Cinematic cinematic;
  check(section(kin_state, Section::cinematic, cinematic) &&
            cinematic.active == 500 && cinematic.step == 0,
        "RF-3.6: the state carries the Kinematic in progress");
  const auto progress = [](AytherSession &s) {
    std::vector<std::uint64_t> out;
    for (int f = 0; f < 6; ++f) {
      const FrameView &v = s.step();
      out.push_back((v.kinematic_id << 8) | v.kinematic_step);
    }
    return out;
  };
  const std::vector<std::uint64_t> want_kin = progress(*k);
  k.reset();
  check(want_kin[0] == (500U << 8),
        "linear production keeps it within its gap tolerance");
  Restored kb = restore_into(configure_kinematic, kin_core, &kin_state);
  const std::vector<std::uint64_t> got_kin =
      kb.session ? progress(*kb.session) : std::vector<std::uint64_t>{};
  kb.session.reset();
  check(kb.result.restored() && got_kin == want_kin,
        "RF-3.6: after the restore the Kinematic continues as in linear "
        "production");
  Restored kc = restore_into(configure_kinematic, kin_core, nullptr);
  check(kc.session && kc.session->step().kinematic_id == 0,
        "control: without the state there is no Kinematic in progress");
}

// BR-028: the per-palette luminance peak decides the E1 tint of a pose. The
// test core's palette only brightens over the first frames, so the peak is
// taken at a brighter frame (400) and a darker one (200) is drawn after it.
void palette_luma_and_audio_mask() {
  auto a = open_session();
  if (!a)
    return;
  for (int f = 0; f < 199; ++f)
    (void)a->step();
  const std::vector<std::uint8_t> before = save(*a);
  const FrameView &seen = a->step();
  check(seen.sprite_occ_count >= 2, "the frame has sprites to make a pose of");
  if (seen.sprite_occ_count < 2)
    return;
  AytherSession::PosePreview pose;
  pose.hashes = {seen.sprite_occs[0].hash, seen.sprite_occs[1].hash};
  pose.rel_x = {0, static_cast<std::int16_t>(seen.sprite_occs[1].screen_x -
                                             seen.sprite_occs[0].screen_x)};
  pose.rel_y = {0, static_cast<std::int16_t>(seen.sprite_occs[1].screen_y -
                                             seen.sprite_occs[0].screen_y)};
  pose.asset = "graphics/pose.png";
  for (int f = 200; f < 400; ++f)
    (void)a->step();
  const Configure configure = [pose](AytherSession &s) {
    s.set_pose_preview({pose});
  };
  configure(*a);
  const vs::VisualState state = a->export_visual_state();
  const std::vector<std::uint8_t> core = save(*a);
  sv::PaletteLuma luma;
  sv::PreviousAudioMask mask;
  check(section(state, Section::palette_luma, luma) && luma.peak[0] > 0.0 &&
            section(state, Section::previous_audio_mask, mask),
        "RF-3.6: the state carries the luminance peaks and the audio mask");

  // Frame 200 drawn again after the peak of frame 400.
  const auto draw = [&](AytherSession &s, std::vector<std::uint8_t> &tint) {
    const FrameView &v = replay(s, before);
    const std::uint32_t n = v.sprite_subs ? v.sprite_sub_count : 0U;
    tint.assign(v.sprite_sub_tint, v.sprite_sub_tint + n * 3U);
    return ayther::test::scene_digest(v);
  };
  std::vector<std::uint8_t> want_tint;
  const std::uint64_t want_scene = draw(*a, want_tint);
  a.reset();
  check(want_tint.size() == 3 && want_tint[0] < 64,
        "linear production draws the pose dimmed against the brighter peak");
  Restored b = restore_into(configure, core, &state);
  std::vector<std::uint8_t> got_tint;
  const std::uint64_t got_scene = b.session ? draw(*b.session, got_tint) : 0;
  b.session.reset();
  check(b.result.restored() && got_tint == want_tint && got_scene == want_scene,
        "RF-3.6: after the restore the next frame draws the same scene and "
        "tint");
  Restored c = restore_into(configure, core, nullptr);
  std::vector<std::uint8_t> control_tint;
  if (c.session)
    (void)draw(*c.session, control_tint);
  c.session.reset();
  check(control_tint != want_tint,
        "control: without the peaks the pose is not dimmed");

  // The mask of channels muted on the last frame is restored as exported.
  sv::PreviousAudioMask other{mask.mask ^ 0x5U};
  const vs::VisualState changed =
      with_section(state, Section::previous_audio_mask, sv::encode(other));
  Restored d = restore_into(configure, core, &changed);
  sv::PreviousAudioMask restored_mask;
  check(d.result.restored() && d.session &&
            section(d.session->export_visual_state(),
                    Section::previous_audio_mask, restored_mask) &&
            restored_mask.mask == other.mask,
        "RF-3.6: the previous audio mask is restored as given");
}

// DI-6: the tint reference of a Panorama is exported and restored. The test
// core cannot anchor a Panorama (its planes change every frame), so the
// section is checked on a defined Panorama without anchoring it.
void panorama_tint() {
  const Configure configure = [](AytherSession &s) {
    const AytherSession::PanoramaCell cells[2] = {{0x1111, 0, 0},
                                                  {0x2222, 1, 0}};
    s.define_panorama(31, 0, 0, 0, 2, 1, cells, 2, "graphics/pano.png");
  };
  auto a = open_session();
  if (!a)
    return;
  configure(*a);
  for (int f = 0; f < 5; ++f)
    (void)a->step();
  const vs::VisualState state = a->export_visual_state();
  const std::vector<std::uint8_t> core = save(*a);
  a.reset();
  std::vector<sv::PanoramaTint> tints;
  check(section(state, Section::panorama_tint, tints) && tints.size() == 1 &&
            tints[0].id == 31,
        "RF-3.6: panorama_tint lists each defined Panorama");
  if (tints.size() != 1)
    return;
  tints[0].ref_peak = 0.5;
  tints[0].ref_luma = 0.45;
  tints[0].ref_w = {0.1, 0.2, 0.3, 0.4};
  tints[0].ref_ch = {0.2, 0.3, 0.4};
  tints[0].ref_chroma = true;
  const vs::VisualState changed =
      with_section(state, Section::panorama_tint, sv::encode(tints));
  Restored b = restore_into(configure, core, &changed);
  std::vector<sv::PanoramaTint> back;
  check(b.result.restored() && b.session &&
            section(b.session->export_visual_state(), Section::panorama_tint,
                    back) &&
            back.size() == 1 && back[0].ref_peak == 0.5 &&
            back[0].ref_luma == 0.45 && back[0].ref_w == tints[0].ref_w &&
            back[0].ref_ch == tints[0].ref_ch && back[0].ref_chroma,
        "RF-3.6: the Panorama tint reference is restored as given");
  b.session.reset();
  tints[0].id = 32;
  const vs::VisualState foreign =
      with_section(state, Section::panorama_tint, sv::encode(tints));
  Restored c = restore_into(configure, core, &foreign);
  check(c.result.code == Code::invalid_payload,
        "a tint for a Panorama the session does not define is rejected");
}
} // namespace

int main() try {
  g_rom = ayther::synth::canonical_rom_path();
  check(!g_rom.empty(),
        "the synthetic ROM is written to the temporary directory");
  if (g_rom.empty())
    return 1;
  api_and_rejections();
  picture_and_camera();
  sequences_and_cinematic();
  palette_luma_and_audio_mask();
  panorama_tint();
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
