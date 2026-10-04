// Validates one render probe frame record (spec 002 plan §4.3) against the
// render observation contract (contracts.md C2 `render_frame` and C3). Returns
// one message per violation; an empty list means the record conforms.
#pragma once

#include "mini_json.h"

#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace ayther::test {

namespace frame_contract_detail {
inline bool one_of(const std::string &v,
                   std::initializer_list<const char *> set) {
  for (const char *s : set)
    if (v == s)
      return true;
  return false;
}
inline bool hex16(const json::Value *v) {
  if (v == nullptr || !v->is(json::Value::Kind::string) || v->text.size() != 16)
    return false;
  for (const char c : v->text)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  return true;
}
inline bool number(const json::Value *v) {
  return v != nullptr && v->is(json::Value::Kind::number) && v->number >= 0.0;
}
} // namespace frame_contract_detail

inline std::vector<std::string>
validate_frame_record(const json::Value &record) {
  using frame_contract_detail::hex16;
  using frame_contract_detail::number;
  using frame_contract_detail::one_of;
  using Kind = json::Value::Kind;
  std::vector<std::string> errors;
  const auto fail = [&](const std::string &what) { errors.push_back(what); };
  if (!record.is(Kind::object))
    return {"the record is not an object"};
  if (!number(record.get("frame")))
    fail("frame is not a non-negative number");
  const json::Value *comp = record.get("composability");
  if (comp == nullptr || !comp->is(Kind::string) ||
      !one_of(comp->text, {"composable", "raster_split", "fade", "line_hscroll",
                           "column_vscroll", "other"}))
    fail("composability is missing or unknown");
  const json::Value *occs = record.get("occurrences");
  const json::Value *reps = record.get("replacements");
  const json::Value *occ_total = record.get("occurrences_total");
  const json::Value *rep_total = record.get("replacements_total");
  if (occs == nullptr || !occs->is(Kind::array) || reps == nullptr ||
      !reps->is(Kind::array) || !number(occ_total) || !number(rep_total))
    return errors.empty() ? std::vector<std::string>{"lists or totals missing"}
                          : errors;
  constexpr std::size_t kLimit = 256;
  if (occs->items.size() > kLimit || reps->items.size() > kLimit)
    fail("a list is above the per-frame limit");
  const bool excess =
      occ_total->number > static_cast<double>(occs->items.size()) ||
      rep_total->number > static_cast<double>(reps->items.size());
  const json::Value *overflow = record.get("overflow");
  if (occ_total->number < static_cast<double>(occs->items.size()) ||
      rep_total->number < static_cast<double>(reps->items.size()))
    fail("a total is below its list");
  if (excess && (overflow == nullptr || !number(overflow->get("rows_total")) ||
                 !number(overflow->get("limit"))))
    fail("an excess is not reported as overflow (RNF-3)");
  if (!excess && overflow != nullptr)
    fail("overflow is reported without an excess");

  // Replacements: index, kind, asset, disjoint members.
  std::set<double> claimed_members;
  for (std::size_t r = 0; r < reps->items.size(); ++r) {
    const json::Value &rep = reps->items[r];
    const json::Value *index = rep.get("index");
    const json::Value *kind = rep.get("kind");
    const json::Value *asset = rep.get("asset");
    const json::Value *members = rep.get("members");
    if (!number(index) || index->number != static_cast<double>(r))
      fail("replacement " + std::to_string(r) + ": index is not its position");
    if (kind == nullptr || !kind->is(Kind::string) ||
        !one_of(kind->text,
                {"pose", "sprite", "plane_set", "screen", "panorama"}))
      fail("replacement " + std::to_string(r) + ": unknown kind");
    if (asset == nullptr || !asset->is(Kind::string))
      fail("replacement " + std::to_string(r) + ": no asset");
    if (members == nullptr || !members->is(Kind::array)) {
      fail("replacement " + std::to_string(r) + ": no members");
      continue;
    }
    for (const json::Value &m : members->items)
      if (!number(&m) || !claimed_members.insert(m.number).second)
        fail("replacement " + std::to_string(r) +
             ": a member is invalid or belongs to another replacement");
    const json::Value *draw = rep.get("draw");
    if (draw != nullptr &&
        (!draw->is(Kind::string) ||
         !one_of(draw->text, {"in_pass", "partitioned", "lane", "discarded"})))
      fail("replacement " + std::to_string(r) + ": unknown draw outcome");
    const json::Value *texture = rep.get("texture");
    if (texture != nullptr &&
        (!texture->is(Kind::string) ||
         !one_of(texture->text, {"ready", "pending", "failed"})))
      fail("replacement " + std::to_string(r) + ": unknown texture state");
  }

  // Occurrences.
  std::set<double> ids;
  for (std::size_t i = 0; i < occs->items.size(); ++i) {
    const json::Value &occ = occs->items[i];
    const std::string at = "occurrence " + std::to_string(i) + ": ";
    const json::Value *id = occ.get("occ_id");
    if (id == nullptr || !number(id->get("index")) ||
        !number(id->get("slot")) || !number(id->get("chain")))
      fail(at + "occ_id is incomplete");
    else if (!ids.insert(id->get("index")->number).second)
      fail(at + "occ_id repeats another occurrence");
    if (!hex16(occ.get("identity")))
      fail(at + "identity is not a 16-digit hash");
    if (occ.get("pose") != nullptr && !hex16(occ.get("pose")))
      fail(at + "pose is not a 16-digit key");
    const json::Value *status = occ.get("status");
    if (status == nullptr || !status->is(Kind::string)) {
      fail(at + "no status");
      continue;
    }
    const std::string &s = status->text;
    const json::Value *replacement = occ.get("replacement");
    const json::Value *reason = occ.get("reason");
    if (s == "replaced") {
      if (!number(replacement) ||
          replacement->number >= static_cast<double>(reps->items.size()))
        fail(at + "replaced without a valid replacement");
      else {
        const json::Value &rep =
            reps->items[static_cast<std::size_t>(replacement->number)];
        bool member = false;
        if (const json::Value *members = rep.get("members"))
          for (const json::Value &m : members->items)
            member = member || (id != nullptr && id->get("index") != nullptr &&
                                m.number == id->get("index")->number);
        if (!member)
          fail(at + "replaced but not a member of its replacement");
      }
      const json::Value *asset = occ.get("asset");
      if (asset == nullptr || !asset->is(Kind::string) || asset->text.empty())
        fail(at + "replaced without its asset");
      const json::Value *draw = occ.get("draw");
      if (draw != nullptr &&
          (!draw->is(Kind::string) ||
           !one_of(draw->text, {"in_pass", "partitioned", "lane"})))
        fail(at + "a replaced occurrence names a draw that does not draw");
      if (reason != nullptr)
        fail(at + "replaced with a not-applied reason");
    } else if (s == "assigned_not_applied") {
      if (replacement != nullptr || occ.get("asset") != nullptr)
        fail(at + "not applied but names a replacement");
      if (reason != nullptr &&
          (!reason->is(Kind::string) ||
           !one_of(reason->text,
                   {"texture_pending", "texture_failed", "frame_not_composable",
                    "member_hidden", "hd_off"})))
        fail(at + "unknown not-applied reason");
    } else if (s == "original_unassigned" || s == "hidden_by_author") {
      if (replacement != nullptr || occ.get("asset") != nullptr ||
          occ.get("draw") != nullptr || reason != nullptr)
        fail(at + s + " with replacement fields");
    } else {
      fail(at + "unknown status");
    }
  }
  for (const double m : claimed_members)
    if (ids.count(m) == 0 && m < static_cast<double>(occs->items.size()))
      fail("a replacement member is not an occurrence of the frame");
  return errors;
}

} // namespace ayther::test
