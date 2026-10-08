#include "session/render_observation_builder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace ayther::session {
namespace {

namespace ro = engine::render_observation;
using ro::Availability;
using ro::FieldView;

constexpr std::string_view kPoseField = "pose";
constexpr std::string_view kReasonField = "not_applied_reason";

FieldView not_applicable(std::string_view name, std::string_view why) {
  FieldView f;
  f.name = name;
  f.availability = Availability::not_applicable;
  f.unavailable_reason = why;
  return f;
}

FieldView unknown(std::string_view name, std::string_view why) {
  FieldView f;
  f.name = name;
  f.availability = Availability::unknown;
  f.unavailable_reason = why;
  return f;
}

FieldView known(std::string_view name, std::string_view value) {
  FieldView f;
  f.name = name;
  f.availability = Availability::known;
  f.value = value;
  return f;
}

std::string_view bounded(const char (&text)[256]) {
  return {text, ::strnlen(text, sizeof(text))};
}

bool same_rect(const AytherSpriteOccurrence &o, const AytherSpriteSub &s) {
  return o.screen_x == s.screen_x && o.screen_y == s.screen_y &&
         o.w_tiles == s.w_tiles && o.h_tiles == s.h_tiles;
}

} // namespace

const ro::RenderFrameView &
RenderObservationBuilder::build(const RenderObservationInput &in) {
  const std::size_t n_occ = in.occurrences.size();
  const std::size_t n_sub = in.subs.size();

  // 1. Owner of every occurrence: its pose substitution, or the per-sprite
  //    substitution drawn at exactly its rectangle (first unowned match).
  owner_.assign(n_occ, kNoPoseOwner);
  for (std::size_t i = 0; i < n_occ && i < in.pose_owner.size(); ++i)
    if (in.pose_owner[i] < in.pose_sub_count && in.pose_owner[i] < n_sub)
      owner_[i] = in.pose_owner[i];
  for (std::size_t s = in.pose_sub_count; s < n_sub; ++s)
    for (std::size_t i = 0; i < n_occ; ++i)
      if (owner_[i] == kNoPoseOwner &&
          !(i < in.claimed.size() && in.claimed[i] != 0) &&
          same_rect(in.occurrences[i], in.subs[s])) {
        owner_[i] = static_cast<std::uint32_t>(s);
        break;
      }

  // 2. Replacements, with their members laid out contiguously.
  member_count_.assign(n_sub, 0);
  for (std::size_t i = 0; i < n_occ; ++i)
    if (owner_[i] != kNoPoseOwner)
      ++member_count_[owner_[i]];
  const std::size_t shown_subs = std::min(n_sub, ro::max_replacements);
  members_.clear();
  members_.reserve(n_occ);
  pose_keys_.assign(shown_subs, std::string{});
  replacements_.assign(shown_subs, ro::ReplacementView{});
  const bool draw_known = in.draw != nullptr && in.frame_known &&
                          in.draw->emulation_frame == in.emulation_frame;
  for (std::size_t s = 0; s < shown_subs; ++s) {
    const AytherSpriteSub &sub = in.subs[s];
    ro::ReplacementView &r = replacements_[s];
    r.index = static_cast<std::uint32_t>(s);
    const bool is_pose = s < in.pose_sub_count;
    r.kind = is_pose ? "pose" : "sprite";
    if (is_pose && sub.pose_key != 0) {
      char hex[17];
      std::snprintf(hex, sizeof(hex), "%016llx",
                    static_cast<unsigned long long>(sub.pose_key));
      pose_keys_[s] = hex;
    }
    r.asset = bounded(sub.asset_path);
    const std::size_t first = members_.size();
    for (std::size_t i = 0; i < n_occ; ++i)
      if (owner_[i] == s)
        members_.push_back(
            {static_cast<std::uint16_t>(i), in.occurrences[i].slot,
             i < in.chain_by_occurrence.size() ? in.chain_by_occurrence[i]
                                               : std::uint8_t{0xFF}});
    r.members = std::span<const ro::OccurrenceId>(members_).subspan(
        first, members_.size() - first);
    if (draw_known && s < in.draw->replacements.size()) {
      r.render_availability = Availability::known;
      r.draw = in.draw->replacements[s].draw;
      r.texture = in.draw->replacements[s].texture;
    }
  }
  // pose_key strings are final now: take their views.
  for (std::size_t s = 0; s < shown_subs; ++s)
    replacements_[s].pose_key = pose_keys_[s];

  // 3. Occurrences and their status.
  const std::size_t shown_occ = std::min(n_occ, ro::max_occurrences);
  occurrences_.assign(shown_occ, ro::OccurrenceView{});
  for (std::size_t i = 0; i < shown_occ; ++i) {
    const AytherSpriteOccurrence &o = in.occurrences[i];
    ro::OccurrenceView &v = occurrences_[i];
    v.id = {static_cast<std::uint16_t>(i), o.slot,
            i < in.chain_by_occurrence.size() ? in.chain_by_occurrence[i]
                                              : std::uint8_t{0xFF}};
    v.identity_hash = o.hash;
    v.x = o.screen_x;
    v.y = o.screen_y;
    v.w = static_cast<std::uint16_t>(o.w_tiles * 8);
    v.h = static_cast<std::uint16_t>(o.h_tiles * 8);
    v.priority = o.priority;
    v.pose = not_applicable(kPoseField, "no assignment");
    v.not_applied_reason =
        not_applicable(kReasonField, "not assigned_not_applied");
    const bool hidden = i < in.hidden.size() && in.hidden[i] != 0;
    const bool claimed = i < in.claimed.size() && in.claimed[i] != 0;
    const std::uint32_t owner = owner_[i];
    if (owner != kNoPoseOwner && owner < shown_subs) {
      const ro::ReplacementView &r = replacements_[owner];
      if (!r.pose_key.empty())
        v.pose = known(kPoseField, r.pose_key);
      else
        v.pose = not_applicable(kPoseField, "per-sprite substitution");
    }
    if (hidden) {
      v.status = ro::OccurrenceStatus::hidden_by_author;
      continue;
    }
    if (owner == kNoPoseOwner) {
      if (claimed) {
        // Claimed by a substitution that was not written (buffer limit).
        v.status = ro::OccurrenceStatus::assigned_not_applied;
        v.not_applied_reason =
            unknown(kReasonField, "claiming substitution not reported");
      } else {
        v.status = ro::OccurrenceStatus::original_unassigned;
      }
      continue;
    }
    v.replacement = static_cast<std::int32_t>(owner);
    v.status = ro::OccurrenceStatus::replaced;
    if (owner < shown_subs) {
      const ro::ReplacementView &r = replacements_[owner];
      if (r.render_availability == Availability::known) {
        // A discarded draw never requested its texture: its state says
        // nothing, so it is checked first.
        if (!in.draw->hd_enabled) {
          v.status = ro::OccurrenceStatus::assigned_not_applied;
          v.not_applied_reason = known(kReasonField, "hd_off");
        } else if (r.draw == ro::DrawOutcome::discarded) {
          v.status = ro::OccurrenceStatus::assigned_not_applied;
          // Spec 002 (R8): in a frame that cannot be composed (raster in
          // mid-screen and the like; the authoring dim is not one) the
          // renderer draws the originals only.
          v.not_applied_reason =
              in.composability != ro::Composability::composable &&
                      in.composability != ro::Composability::fade
                  ? known(kReasonField, "frame_not_composable")
                  : unknown(kReasonField, "discarded by the renderer");
        } else if (r.texture == ro::TextureState::pending) {
          v.status = ro::OccurrenceStatus::assigned_not_applied;
          v.not_applied_reason = known(kReasonField, "texture_pending");
        } else if (r.texture == ro::TextureState::failed) {
          v.status = ro::OccurrenceStatus::assigned_not_applied;
          v.not_applied_reason = known(kReasonField, "texture_failed");
        }
      }
    }
    if (v.status == ro::OccurrenceStatus::assigned_not_applied)
      v.replacement = -1;
  }

  view_ = ro::RenderFrameView{};
  view_.frame.availability =
      in.frame_known ? Availability::known : Availability::unknown;
  view_.frame.emulation_frame = in.emulation_frame;
  if (!in.frame_known)
    view_.frame.unavailable_reason = "frame position not observed";
  view_.composability = in.composability;
  view_.occurrences = occurrences_;
  view_.replacements = replacements_;
  view_.occurrences_total = n_occ;
  view_.replacements_total = n_sub;
  // Contract 1.1 (DI-18): the layers come with the renderer's draw report.
  if (in.draw != nullptr)
    view_.layers = in.draw->layers;
  return view_;
}

} // namespace ayther::session
