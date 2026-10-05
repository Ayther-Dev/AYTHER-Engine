// Spec 002, BR-021 (RF-7.2, RF-7.3, RF-7.7, RF-7.8, RF-7.9, RNF-3): the pure
// builder of the render observation (contracts.md C3) from synthetic frames.
#include "session/render_observation_builder.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string_view>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

namespace ro = ayther::engine::render_observation;
using ayther::session::kNoPoseOwner;
using ayther::session::RenderObservationBuilder;
using ayther::session::RenderObservationInput;

AytherSpriteOccurrence occ(std::uint64_t hash, std::int16_t x, std::int16_t y,
                           std::uint8_t slot) {
  AytherSpriteOccurrence o{};
  o.hash = hash;
  o.w_tiles = 1;
  o.h_tiles = 1;
  o.screen_x = x;
  o.screen_y = y;
  o.slot = slot;
  return o;
}

AytherSpriteSub sub(const char *asset, std::int16_t x, std::int16_t y,
                    std::uint64_t pose_key) {
  AytherSpriteSub s{};
  const std::size_t n = std::min(std::strlen(asset), sizeof(s.asset_path) - 1);
  std::memcpy(s.asset_path, asset, n);
  s.screen_x = x;
  s.screen_y = y;
  s.w_tiles = 1;
  s.h_tiles = 1;
  s.pose_key = pose_key;
  return s;
}

std::string_view text(const ro::FieldView &f) {
  const auto *v = std::get_if<std::string_view>(&f.value);
  return v ? *v : std::string_view{};
}
} // namespace

int main() try {
  std::array<std::uint8_t, 80> chain{};
  for (std::size_t i = 0; i < chain.size(); ++i)
    chain[i] = static_cast<std::uint8_t>(79 - i);

  // RF-7.8: a frame without sprites publishes empty lists.
  {
    RenderObservationBuilder builder;
    RenderObservationInput in;
    in.emulation_frame = 5;
    in.frame_known = true;
    const ro::RenderFrameView &v = builder.build(in);
    check(v.occurrences.empty() && v.replacements.empty() &&
              v.occurrences_total == 0,
          "RF-7.8: no sprites -> no occurrences and no replacements");
    check(v.frame.availability == ro::Availability::known &&
              v.frame.emulation_frame == 5,
          "the frame position is known");
  }

  // Contract 1.1 (DI-18): the layers of the draw report reach the view.
  {
    const std::array layers{
        ro::LayerView{"plane_b", "Plano B", 0, true, false, false, false,
                      false},
        ro::LayerView{"overlay", "Nubes", 1, true, true, true, false, false}};
    const ro::DrawReport report{9, true, {}, layers};
    RenderObservationBuilder builder;
    RenderObservationInput in;
    in.emulation_frame = 9;
    in.frame_known = true;
    in.draw = &report;
    const ro::RenderFrameView &v = builder.build(in);
    check(v.layers.size() == 2 && v.layers[1].overlay &&
              v.layers[1].name == "Nubes" && v.layers[1].stack_index == 1 &&
              v.layers[1].gated && !v.layers[1].gate_open,
          "DI-18: the view carries the stack's layers and overlay gates");
  }

  // RF-7.2, RF-7.3: one pose with two members, one per-sprite substitution,
  // one unrelated sprite and two occurrences of the same identity.
  {
    const std::array occs{occ(0xA1, 10, 10, 0), occ(0xA2, 18, 10, 1),
                          occ(0xB0, 50, 50, 2), occ(0xC0, 90, 90, 3),
                          occ(0xC0, 120, 90, 4)};
    const std::array subs{sub("pose.png", 10, 10, 0x1234),
                          sub("sprite.png", 50, 50, 0)};
    const std::array<std::uint8_t, 5> claimed{1, 1, 0, 0, 0};
    const std::array<std::uint32_t, 5> owner{0, 0, kNoPoseOwner, kNoPoseOwner,
                                             kNoPoseOwner};
    RenderObservationBuilder builder;
    RenderObservationInput in;
    in.emulation_frame = 7;
    in.frame_known = true;
    in.occurrences = occs;
    in.claimed = claimed;
    in.chain_by_slot = chain;
    in.subs = subs;
    in.pose_sub_count = 1;
    in.pose_owner = owner;
    const ro::RenderFrameView &v = builder.build(in);
    check(v.replacements.size() == 2, "RF-7.3: two replacements");
    check(v.replacements[0].kind == "pose" &&
              v.replacements[0].members.size() == 2 &&
              v.replacements[0].members[0] == ro::OccurrenceId{0, 0, 79} &&
              v.replacements[0].members[1] == ro::OccurrenceId{1, 1, 78},
          "RF-7.3: the pose names exactly its two members");
    check(v.replacements[0].pose_key == "0000000000001234" &&
              v.replacements[0].asset == "pose.png",
          "RF-7.3: the pose carries its key and asset from the same frame");
    check(v.replacements[1].kind == "sprite" &&
              v.replacements[1].members.size() == 1 &&
              v.replacements[1].members[0].slot == 2,
          "RF-7.3: the per-sprite substitution names the sprite at its "
          "rectangle");
    check(v.occurrences[0].status == ro::OccurrenceStatus::replaced &&
              v.occurrences[0].replacement == 0 &&
              v.occurrences[0].pose.availability == ro::Availability::known &&
              text(v.occurrences[0].pose) == "0000000000001234",
          "RF-7.2: a pose member is replaced, with its pose");
    check(v.occurrences[2].status == ro::OccurrenceStatus::replaced &&
              v.occurrences[2].pose.availability ==
                  ro::Availability::not_applicable,
          "RF-7.2: a per-sprite replacement has no pose");
    check(v.occurrences[3].status ==
                  ro::OccurrenceStatus::original_unassigned &&
              v.occurrences[3].replacement == -1,
          "RF-7.2: an unassigned sprite is drawn as the original");
    check(v.occurrences[3].identity_hash == v.occurrences[4].identity_hash &&
              !(v.occurrences[3].id == v.occurrences[4].id),
          "RF-7.2: two occurrences of the same identity stay distinct");
    check(v.replacements[0].render_availability == ro::Availability::unknown,
          "RF-7.7: without a draw report the draw outcome is unknown, not "
          "invented");
  }

  // RF-7.9: draw report outcomes; a report of another frame is ignored.
  {
    const std::array occs{occ(0xA1, 10, 10, 0), occ(0xA2, 18, 10, 1),
                          occ(0xA3, 40, 40, 2)};
    const std::array subs{sub("a.png", 10, 10, 1), sub("b.png", 18, 10, 2),
                          sub("c.png", 40, 40, 3)};
    const std::array<std::uint8_t, 3> claimed{1, 1, 1};
    const std::array<std::uint32_t, 3> owner{0, 1, 2};
    std::array draws{
        ro::ReplacementDraw{ro::DrawOutcome::in_pass,
                            ro::TextureState::pending},
        ro::ReplacementDraw{ro::DrawOutcome::in_pass, ro::TextureState::failed},
        ro::ReplacementDraw{ro::DrawOutcome::lane, ro::TextureState::ready}};
    ro::DrawReport report{9, true, draws};
    RenderObservationBuilder builder;
    RenderObservationInput in;
    in.emulation_frame = 9;
    in.frame_known = true;
    in.occurrences = occs;
    in.claimed = claimed;
    in.chain_by_slot = chain;
    in.subs = subs;
    in.pose_sub_count = 3;
    in.pose_owner = owner;
    in.draw = &report;
    const ro::RenderFrameView &v = builder.build(in);
    check(v.occurrences[0].status ==
                  ro::OccurrenceStatus::assigned_not_applied &&
              text(v.occurrences[0].not_applied_reason) == "texture_pending",
          "RF-7.9: a pending texture is an assignment not applied, with its "
          "reason");
    check(v.occurrences[1].status ==
                  ro::OccurrenceStatus::assigned_not_applied &&
              text(v.occurrences[1].not_applied_reason) == "texture_failed",
          "RF-7.9: a failed texture is an assignment not applied, with its "
          "reason");
    check(v.occurrences[2].status == ro::OccurrenceStatus::replaced &&
              v.replacements[2].render_availability ==
                  ro::Availability::known &&
              v.replacements[2].draw == ro::DrawOutcome::lane,
          "a ready texture drawn in the lane is a replacement");
    report.hd_enabled = false;
    const ro::RenderFrameView &hd_off = builder.build(in);
    check(hd_off.occurrences[2].status ==
                  ro::OccurrenceStatus::assigned_not_applied &&
              text(hd_off.occurrences[2].not_applied_reason) == "hd_off" &&
              hd_off.occurrences[2].replacement == -1,
          "RF-7.9: a frame rendered with HD off reports hd_off");
    report.hd_enabled = true;
    // A discarded draw never requested its texture: the reason is unknown,
    // not the texture state.
    draws[0].draw = ro::DrawOutcome::discarded;
    const ro::RenderFrameView &discarded = builder.build(in);
    check(discarded.occurrences[0].status ==
                  ro::OccurrenceStatus::assigned_not_applied &&
              discarded.occurrences[0].not_applied_reason.availability ==
                  ro::Availability::unknown,
          "RF-7.9: a draw the renderer discarded has an unknown reason");
    draws[0].draw = ro::DrawOutcome::in_pass;
    report.emulation_frame = 8;
    const ro::RenderFrameView &stale = builder.build(in);
    check(stale.replacements[0].render_availability ==
                  ro::Availability::unknown &&
              stale.occurrences[0].status == ro::OccurrenceStatus::replaced,
          "RF-7.5: a draw report of another frame is not reused");
  }

  // RF-7.9: claimed without a reported substitution -> reason unknown.
  // Hidden by an authoring tool is its own status.
  {
    const std::array occs{occ(0xA1, 10, 10, 0), occ(0xA2, 30, 30, 1)};
    const std::array<std::uint8_t, 2> claimed{1, 0};
    const std::array<std::uint8_t, 2> hidden{0, 1};
    const std::array<std::uint32_t, 2> owner{kNoPoseOwner, kNoPoseOwner};
    RenderObservationBuilder builder;
    RenderObservationInput in;
    in.frame_known = true;
    in.occurrences = occs;
    in.claimed = claimed;
    in.hidden = hidden;
    in.chain_by_slot = chain;
    in.pose_owner = owner;
    const ro::RenderFrameView &v = builder.build(in);
    check(v.occurrences[0].status ==
                  ro::OccurrenceStatus::assigned_not_applied &&
              v.occurrences[0].not_applied_reason.availability ==
                  ro::Availability::unknown,
          "RF-7.9: an unknown reason is reported as unknown, not as a cause");
    check(v.occurrences[1].status == ro::OccurrenceStatus::hidden_by_author,
          "a sprite hidden by the author keeps that status");
  }

  // RNF-3: above the limit the lists stop at it and the totals stay real.
  {
    std::vector<AytherSpriteOccurrence> occs;
    for (int i = 0; i < 300; ++i)
      occs.push_back(occ(static_cast<std::uint64_t>(i + 1),
                         static_cast<std::int16_t>(i), 0,
                         static_cast<std::uint8_t>(i % 80)));
    RenderObservationBuilder builder;
    RenderObservationInput in;
    in.frame_known = true;
    in.occurrences = occs;
    const ro::RenderFrameView &v = builder.build(in);
    check(v.occurrences.size() == ro::max_occurrences &&
              v.occurrences_total == 300,
          "RNF-3: 300 occurrences -> 256 shown and a total of 300");
  }

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
