// Spec 002, BR-021 (RF-7.2, RF-7.3, RF-7.8) and BR-023 (RF-7.9): a live
// session publishes its render observation (contracts.md C3) through
// Config::render_observer, with the reason its frame is dirty. The
// in-repository test core draws a sprite table that depends only on the ROM and
// the slot, so a pose built from two occurrences of one frame matches in the
// next one.
#include <ayther/ayther_session.h>
#include <ayther/engine/render_observer.hpp>

#include "../../tools/common/synth_rom.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

namespace ro = ayther::engine::render_observation;

struct Copy {
  bool frame_known = false;
  ro::Composability composability = ro::Composability::composable;
  std::size_t total = 0;
  std::vector<ro::OccurrenceView> occurrences;
  // Per occurrence, the not-applied reason as copied text and availability.
  std::vector<std::string> reasons;
  std::vector<ro::Availability> reason_availability;
  std::vector<std::vector<ro::OccurrenceId>> members;
  std::vector<std::string> kinds;
};

class Recorder final : public ro::RenderObserver {
public:
  int calls = 0;
  Copy last;
  void on_render_frame(const ro::RenderFrameView &frame) noexcept override {
    ++calls;
    last = Copy{};
    last.frame_known = frame.frame.availability == ro::Availability::known;
    last.composability = frame.composability;
    last.total = frame.occurrences_total;
    last.occurrences.assign(frame.occurrences.begin(), frame.occurrences.end());
    for (const ro::OccurrenceView &o : frame.occurrences) {
      const auto *text =
          std::get_if<std::string_view>(&o.not_applied_reason.value);
      last.reasons.emplace_back(text ? *text : std::string_view{});
      last.reason_availability.push_back(o.not_applied_reason.availability);
    }
    for (const ro::ReplacementView &r : frame.replacements) {
      last.members.emplace_back(r.members.begin(), r.members.end());
      last.kinds.emplace_back(r.kind);
    }
  }
};
} // namespace

int main() try {
  using ayther::AytherSession;
  const std::string rom = ayther::synth::canonical_rom_path();
  check(!rom.empty(),
        "the synthetic ROM is written to the temporary directory");
  if (rom.empty())
    return 1;

  Recorder recorder;
  AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = rom;
  config.enable_audio = false;
  config.render_observer = &recorder;
  auto created = AytherSession::create(config);
  check(static_cast<bool>(created),
        "the session opens with the in-repository test core");
  if (!created) {
    std::fprintf(stderr, "[FAIL] %s\n", created.error.message.c_str());
    return 1;
  }
  std::unique_ptr<AytherSession> &session = *created;

  // RF-7.8: before the first frame there is no position and no sprite.
  session->publish_render_observation(nullptr);
  check(recorder.calls == 1 && !recorder.last.frame_known &&
            recorder.last.total == 0,
        "RF-7.8: no frame yet -> unknown position and an empty list");

  const ayther::FrameView &first = session->step();
  session->publish_render_observation(nullptr);
  check(recorder.calls == 2 && recorder.last.frame_known,
        "one publication per call");
  check((recorder.last.composability == ro::Composability::composable) ==
            (first.scene_dirty == 0),
        "RF-7.9: the frame is composable exactly when it is not dirty");
  check(recorder.last.total == first.sprite_occ_count &&
            first.sprite_occ_count >= 2,
        "RF-7.2: every detected occurrence is observed");
  std::set<int> ids;
  bool all_unassigned = true;
  for (const ro::OccurrenceView &o : recorder.last.occurrences) {
    ids.insert(o.id.index);
    all_unassigned =
        all_unassigned && o.status == ro::OccurrenceStatus::original_unassigned;
  }
  check(ids.size() == recorder.last.occurrences.size(),
        "RF-7.2: every occurrence has its own id");
  if (ids.size() != recorder.last.occurrences.size())
    for (std::uint32_t i = 0; i < first.sprite_occ_count; ++i) {
      const AytherSpriteOccurrence &o = first.sprite_occs[i];
      std::printf(
          "  occ %u slot=%u link=%u hash=%016llx at %d,%d %ux%u chain=%u\n", i,
          o.slot, o.link, static_cast<unsigned long long>(o.hash), o.screen_x,
          o.screen_y, o.w_tiles, o.h_tiles,
          recorder.last.occurrences[i].id.chain);
    }
  check(all_unassigned && recorder.last.members.empty(),
        "without a pack or a pose every sprite is drawn as the original");

  // RF-7.3: a pose made of two occurrences of one frame. The test core's VRAM
  // changes every frame, so the frame is produced twice from the same saved
  // state: once to read its occurrences, once with the pose installed.
  std::vector<std::uint8_t> state;
  check(static_cast<bool>(session->serialize(state)),
        "the state before the frame is saved");
  const AytherSpriteOccurrence *probe = session->step().sprite_occs;
  const AytherSpriteOccurrence a = probe[0];
  const AytherSpriteOccurrence b = probe[1];
  AytherSession::PosePreview pose;
  pose.hashes = {a.hash, b.hash};
  pose.rel_x = {0, static_cast<std::int16_t>(b.screen_x - a.screen_x)};
  pose.rel_y = {0, static_cast<std::int16_t>(b.screen_y - a.screen_y)};
  pose.asset = "graphics/test-pose.png";
  check(static_cast<bool>(session->unserialize(state)),
        "the saved state is restored");
  session->set_pose_preview({pose});
  const ayther::FrameView &again = session->step();
  check(again.sprite_occ_count >= 2 && again.sprite_occs[0].hash == a.hash &&
            again.sprite_occs[1].hash == b.hash,
        "the same frame is produced again");
  session->publish_render_observation(nullptr);
  check(recorder.last.members.size() == 1 && recorder.last.kinds[0] == "pose",
        "RF-7.3: the pose is one replacement");
  if (recorder.last.members.size() == 1) {
    std::set<int> indices;
    for (const ro::OccurrenceId &m : recorder.last.members[0])
      indices.insert(m.index);
    check(indices == std::set<int>{0, 1}, "RF-7.3: its members are exactly the "
                                          "two occurrences it was built from");
  }
  int replaced = 0;
  for (const ro::OccurrenceView &o : recorder.last.occurrences)
    if (o.status == ro::OccurrenceStatus::replaced)
      ++replaced;
  check(replaced == 2, "RF-7.2: only the two members are replaced");
  if (replaced != 2)
    for (const ro::OccurrenceView &o : recorder.last.occurrences)
      std::printf("  occ %u hash=%016llx status=%d replacement=%d\n",
                  o.id.index, static_cast<unsigned long long>(o.identity_hash),
                  static_cast<int>(o.status), o.replacement);

  // RF-7.9: the host passes how the frame was drawn. A pending texture leaves
  // the pose assigned and not applied, with its reason; a draw the renderer
  // discarded has no known reason; a report of another frame is ignored.
  std::vector<ro::ReplacementDraw> rows(
      again.sprite_sub_count,
      ro::ReplacementDraw{ro::DrawOutcome::in_pass, ro::TextureState::pending});
  ro::DrawReport report;
  report.emulation_frame = again.frame_index;
  report.replacements = rows;
  session->publish_render_observation(&report);
  const auto both_members = [&](ro::OccurrenceStatus status) {
    return recorder.last.occurrences.size() >= 2 &&
           recorder.last.occurrences[0].status == status &&
           recorder.last.occurrences[1].status == status;
  };
  check(both_members(ro::OccurrenceStatus::assigned_not_applied) &&
            recorder.last.reasons[0] == "texture_pending" &&
            recorder.last.reason_availability[0] == ro::Availability::known,
        "RF-7.9: a pending texture -> assigned_not_applied (texture_pending)");
  for (ro::ReplacementDraw &row : rows)
    row = {ro::DrawOutcome::discarded, ro::TextureState::pending};
  session->publish_render_observation(&report);
  check(both_members(ro::OccurrenceStatus::assigned_not_applied) &&
            recorder.last.reason_availability[0] == ro::Availability::unknown,
        "RF-7.9: a reason the Engine cannot tell is unknown, not invented");
  report.emulation_frame = again.frame_index - 1;
  session->publish_render_observation(&report);
  check(both_members(ro::OccurrenceStatus::replaced),
        "a draw report of another frame is ignored");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
