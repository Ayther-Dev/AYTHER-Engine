// JSON frame record of the render probe (spec 002, plan §4.3): the frame,
// its composability and the render observation (contracts.md C2
// `render_frame`, C3). Only `known` values are written (RF-7.7); a frame
// without sprites has empty lists (RF-7.8); an excess above the per-frame
// limit is reported as `overflow` (RNF-3). Pure: no session or renderer.
#pragma once

#include <ayther/engine/render_observer.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <variant>

namespace ayther::probe {

namespace probe_json_detail {
namespace ro = engine::render_observation;

inline void quoted(std::string &out, std::string_view text) {
  out += '"';
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char esc[8];
      std::snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(c));
      out += esc;
    } else {
      out += c;
    }
  }
  out += '"';
}

inline std::string hex16(std::uint64_t value) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx",
                static_cast<unsigned long long>(value));
  return text;
}

inline const char *name(ro::Composability c) {
  switch (c) {
  case ro::Composability::composable:
    return "composable";
  case ro::Composability::raster_split:
    return "raster_split";
  case ro::Composability::fade:
    return "fade";
  case ro::Composability::line_hscroll:
    return "line_hscroll";
  case ro::Composability::column_vscroll:
    return "column_vscroll";
  case ro::Composability::other:
    return "other";
  }
  return "other";
}

inline const char *name(ro::OccurrenceStatus s) {
  switch (s) {
  case ro::OccurrenceStatus::replaced:
    return "replaced";
  case ro::OccurrenceStatus::original_unassigned:
    return "original_unassigned";
  case ro::OccurrenceStatus::assigned_not_applied:
    return "assigned_not_applied";
  case ro::OccurrenceStatus::hidden_by_author:
    return "hidden_by_author";
  }
  return "original_unassigned";
}

inline const char *name(ro::DrawOutcome d) {
  switch (d) {
  case ro::DrawOutcome::in_pass:
    return "in_pass";
  case ro::DrawOutcome::partitioned:
    return "partitioned";
  case ro::DrawOutcome::lane:
    return "lane";
  case ro::DrawOutcome::discarded:
    return "discarded";
  }
  return "discarded";
}

inline const char *name(ro::TextureState t) {
  switch (t) {
  case ro::TextureState::ready:
    return "ready";
  case ro::TextureState::pending:
    return "pending";
  case ro::TextureState::failed:
    return "failed";
  }
  return "pending";
}

inline const std::string_view *known_text(const ro::FieldView &field) {
  if (field.availability != ro::Availability::known)
    return nullptr;
  return std::get_if<std::string_view>(&field.value);
}
} // namespace probe_json_detail

inline std::string
frame_json(std::uint32_t frame, std::uint64_t emulation_frame,
           const engine::render_observation::RenderFrameView &view) {
  namespace d = probe_json_detail;
  namespace ro = engine::render_observation;
  std::string out =
      "{\n  \"frame\": " + std::to_string(frame) +
      ",\n  \"emulation_frame\": " + std::to_string(emulation_frame) +
      ",\n  \"composability\": \"" + d::name(view.composability) + "\"" +
      ",\n  \"occurrences_total\": " + std::to_string(view.occurrences_total) +
      ",\n  \"replacements_total\": " + std::to_string(view.replacements_total);
  if (view.occurrences_total > view.occurrences.size() ||
      view.replacements_total > view.replacements.size())
    out += ",\n  \"overflow\": {\"rows_total\": " +
           std::to_string(view.occurrences_total) +
           ", \"limit\": " + std::to_string(ro::max_occurrences) + "}";

  out += ",\n  \"occurrences\": [";
  for (std::size_t i = 0; i < view.occurrences.size(); ++i) {
    const ro::OccurrenceView &o = view.occurrences[i];
    out += i == 0 ? "\n    {" : ",\n    {";
    out += "\"occ_id\": {\"index\": " + std::to_string(o.id.index) +
           ", \"slot\": " + std::to_string(o.id.slot) +
           ", \"chain\": " + std::to_string(o.id.chain) + "}";
    out += ", \"identity\": \"" + d::hex16(o.identity_hash) + "\"";
    if (const std::string_view *pose = d::known_text(o.pose)) {
      out += ", \"pose\": ";
      d::quoted(out, *pose);
    }
    out += ", \"status\": \"";
    out += d::name(o.status);
    out += "\"";
    if (o.status == ro::OccurrenceStatus::replaced && o.replacement >= 0 &&
        static_cast<std::size_t>(o.replacement) < view.replacements.size()) {
      const ro::ReplacementView &r =
          view.replacements[static_cast<std::size_t>(o.replacement)];
      out += ", \"replacement\": " + std::to_string(o.replacement);
      out += ", \"asset\": ";
      d::quoted(out, r.asset);
      if (r.render_availability == ro::Availability::known) {
        out += ", \"draw\": \"";
        out += d::name(r.draw);
        out += "\"";
      }
    }
    if (o.status == ro::OccurrenceStatus::assigned_not_applied)
      if (const std::string_view *reason =
              d::known_text(o.not_applied_reason)) {
        out += ", \"reason\": ";
        d::quoted(out, *reason);
      }
    out += "}";
  }
  out += view.occurrences.empty() ? "]" : "\n  ]";

  out += ",\n  \"replacements\": [";
  for (std::size_t r = 0; r < view.replacements.size(); ++r) {
    const ro::ReplacementView &rep = view.replacements[r];
    out += r == 0 ? "\n    {" : ",\n    {";
    out += "\"index\": " + std::to_string(rep.index) + ", \"kind\": ";
    d::quoted(out, rep.kind);
    if (!rep.pose_key.empty()) {
      out += ", \"pose_key\": ";
      d::quoted(out, rep.pose_key);
    }
    out += ", \"asset\": ";
    d::quoted(out, rep.asset);
    out += ", \"members\": [";
    for (std::size_t m = 0; m < rep.members.size(); ++m) {
      if (m > 0)
        out += ", ";
      out += std::to_string(rep.members[m].index);
    }
    out += "]";
    if (rep.render_availability == ro::Availability::known) {
      out += ", \"draw\": \"";
      out += d::name(rep.draw);
      out += "\", \"texture\": \"";
      out += d::name(rep.texture);
      out += "\"";
    }
    out += "}";
  }
  out += view.replacements.empty() ? "]" : "\n  ]";
  out += "\n}\n";
  return out;
}

} // namespace ayther::probe
