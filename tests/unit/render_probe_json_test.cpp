// Spec 002, BR-037 (RF-7.2, RF-7.3): the render probe's frame record holds
// the observation of plan §4.3 and conforms to the render observation
// contract (contracts.md C2 `render_frame`, C3). Views come from the session's
// observation builder on synthetic frames; the validator itself is checked to
// reject records that break the contract.
#include "../../tools/render_probe/probe_json.h"
#include "mini_json.h"
#include "render_frame_contract.h"
#include "session/render_observation_builder.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

namespace ro = ayther::engine::render_observation;
namespace json = ayther::test::json;
using ayther::session::kNoPoseOwner;
using ayther::session::RenderObservationBuilder;
using ayther::session::RenderObservationInput;

AytherSpriteOccurrence occ(std::uint64_t hash, std::int16_t x,
                           std::uint8_t slot) {
  AytherSpriteOccurrence o{};
  o.hash = hash;
  o.w_tiles = 1;
  o.h_tiles = 1;
  o.screen_x = x;
  o.slot = slot;
  return o;
}

AytherSpriteSub sub(const char *asset, std::int16_t x, std::uint64_t key) {
  AytherSpriteSub s{};
  std::memcpy(s.asset_path, asset, std::strlen(asset));
  s.screen_x = x;
  s.w_tiles = 1;
  s.h_tiles = 1;
  s.pose_key = key;
  return s;
}

std::vector<std::string> errors_of(const std::string &text) {
  const auto value = json::parse(text);
  if (!value)
    return {"not JSON"};
  return ayther::test::validate_frame_record(*value);
}

bool has(const std::string &text, const std::string &needle) {
  return text.find(needle) != std::string::npos;
}
} // namespace

int main() try {
  std::array<std::uint8_t, 80> chain{};
  // A pose of two drawn members, a per-sprite replacement with a pending
  // texture, an unassigned sprite and a hidden one.
  const std::array occs{occ(0xA1, 10, 0), occ(0xA2, 18, 1), occ(0xB0, 50, 2),
                        occ(0xC0, 90, 3), occ(0xD0, 120, 4)};
  const std::array subs{sub("graphics/pose.png", 10, 0x1234),
                        sub("graphics/sprite.png", 50, 0)};
  const std::array<std::uint8_t, 5> claimed{1, 1, 0, 0, 0};
  const std::array<std::uint8_t, 5> hidden{0, 0, 0, 0, 1};
  const std::array<std::uint32_t, 5> owner{0, 0, kNoPoseOwner, kNoPoseOwner,
                                           kNoPoseOwner};
  const std::array draws{
      ro::ReplacementDraw{ro::DrawOutcome::in_pass, ro::TextureState::ready},
      ro::ReplacementDraw{ro::DrawOutcome::lane, ro::TextureState::pending}};
  // Contract 1.1 (DI-18): the stack's layers; one overlay, gated and drawn.
  const std::array layers{
      ro::LayerView{"plane_b", "Plano B", 0, true, false, false, false, false},
      ro::LayerView{"overlay", "Nubes", 1, true, true, true, true, true}};
  ro::DrawReport report{77, true, draws, layers};
  RenderObservationInput in;
  in.emulation_frame = 77;
  in.frame_known = true;
  in.occurrences = occs;
  in.claimed = claimed;
  in.hidden = hidden;
  in.chain_by_occurrence = chain;
  in.subs = subs;
  in.pose_sub_count = 1;
  in.pose_owner = owner;
  in.draw = &report;
  RenderObservationBuilder builder;
  const std::string record =
      ayther::probe::frame_json(12, 77, builder.build(in));
  const std::vector<std::string> errors = errors_of(record);
  for (const std::string &e : errors)
    std::printf("  %s\n", e.c_str());
  check(errors.empty(), "RF-7.2: a frame record conforms to the contract");
  check(has(record, "\"overlays\": [{\"name\": \"Nubes\", "
                    "\"stack_index\": 1, \"gated\": true, "
                    "\"gate_open\": true, \"drawn\": true}]"),
        "DI-18: the record lists the overlays with their gate and draw");
  check(has(record, "\"frame\": 12") && has(record, "\"emulation_frame\": 77"),
        "the record names its frame");
  check(has(record, "\"status\": \"replaced\"") &&
            has(record, "\"asset\": \"graphics/pose.png\"") &&
            has(record, "\"draw\": \"in_pass\"") &&
            has(record, "\"pose\": \"0000000000001234\""),
        "RF-7.3: a replaced occurrence names its asset, pose and draw");
  check(has(record, "\"members\": [0, 1]"),
        "RF-7.3: the pose names exactly its members");
  check(has(record, "\"reason\": \"texture_pending\"") &&
            has(record, "\"status\": \"original_unassigned\"") &&
            has(record, "\"status\": \"hidden_by_author\""),
        "RF-7.2: each status, with its reason when known");

  // A frame without sprites: empty lists. RNF-3 / P-11: the exact limit is
  // complete, while 257 and 300 rows are bounded with their real total.
  {
    RenderObservationInput empty;
    empty.frame_known = true;
    RenderObservationBuilder b;
    check(errors_of(ayther::probe::frame_json(0, 0, b.build(empty))).empty() &&
              has(ayther::probe::frame_json(0, 0, b.build(empty)),
                  "\"occurrences\": []"),
          "RF-7.8: a frame without sprites has an empty list");
    const auto check_limit = [&](std::size_t total, bool expect_overflow,
                                 const char *message) {
      std::vector<AytherSpriteOccurrence> occurrences;
      occurrences.reserve(total);
      for (std::size_t i = 0; i < total; ++i)
        occurrences.push_back(occ(static_cast<std::uint64_t>(i + 1),
                                  static_cast<std::int16_t>(i),
                                  static_cast<std::uint8_t>(i % 80)));
      RenderObservationInput input;
      input.frame_known = true;
      input.occurrences = occurrences;
      const std::string text = ayther::probe::frame_json(1, 1, b.build(input));
      const auto parsed = json::parse(text);
      const json::Value *shown = parsed ? parsed->get("occurrences") : nullptr;
      const json::Value *reported_total =
          parsed ? parsed->get("occurrences_total") : nullptr;
      const json::Value *overflow = parsed ? parsed->get("overflow") : nullptr;
      const bool exact_shape =
          errors_of(text).empty() && shown != nullptr &&
          shown->is(json::Value::Kind::array) &&
          shown->items.size() == std::min(total, ro::max_occurrences) &&
          reported_total != nullptr &&
          reported_total->is(json::Value::Kind::number) &&
          reported_total->number == static_cast<double>(total);
      const bool exact_overflow =
          expect_overflow ? overflow != nullptr &&
                                overflow->is(json::Value::Kind::object) &&
                                overflow->get("rows_total") != nullptr &&
                                overflow->get("rows_total")->number ==
                                    static_cast<double>(total) &&
                                overflow->get("limit") != nullptr &&
                                overflow->get("limit")->number ==
                                    static_cast<double>(ro::max_occurrences)
                          : overflow == nullptr;
      check(exact_shape && exact_overflow, message);
    };
    check_limit(ro::max_occurrences, false,
                "RNF-3/P-11: 256 rows are complete without overflow");
    check_limit(ro::max_occurrences + 1U, true,
                "RNF-3/P-11: 257 rows report 256 shown and total 257");
    check_limit(300U, true,
                "RNF-3/P-11: 300 rows report 256 shown and total 300");
  }

  // The validator rejects records that break the contract.
  const auto rejects = [&](std::string from, const std::string &what,
                           const std::string &with, const char *message) {
    const std::size_t at = from.find(what);
    if (at != std::string::npos)
      from.replace(at, what.size(), with);
    check(at != std::string::npos && !errors_of(from).empty(), message);
  };
  rejects(record, "\"status\": \"original_unassigned\"",
          "\"status\": \"drawn_somehow\"", "an unknown status is rejected");
  rejects(record, "\"asset\": \"graphics/pose.png\", ", "",
          "a replaced occurrence without its asset is rejected");
  rejects(record, "\"reason\": \"texture_pending\"", "\"reason\": \"bad_luck\"",
          "an unknown reason is rejected");
  rejects(record, "\"members\": [0, 1]", "\"members\": [0, 2]",
          "a member that is not replaced by it is rejected");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
