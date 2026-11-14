// Spec 008 (TP-209.2, RF-10.2): the identity files of parity_bench dump
// --identities. Segments parse strictly, the file round-trips, the collector
// files each identity under the segments of its frame and latches a palette
// line after 30 stable frames, and the comparison reports missing and extra
// identifiers per class.
#include "../../tools/parity_bench/identities.h"

#include <ayther/ayther_session.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

using namespace ayther::parity;

const ClassIds &cls(const Identities &ids, std::size_t seg, const char *name) {
  return ids.segments.at(seg).classes.at(name);
}

void segments() {
  const auto two = parse_segments("0-600,1800-2400");
  check(two && two->size() == 2 && (*two)[1].first == 1800 &&
            (*two)[1].last == 2400,
        "two segments parse");
  check(!parse_segments(""), "empty text is rejected");
  check(!parse_segments("5-4"), "a reversed range is rejected");
  check(!parse_segments("0-600,"), "a trailing comma is rejected");
  check(!parse_segments("0-6x0"), "a non-number is rejected");
}

Identities collect() {
  IdentityCollector c({{1, 40}, {31, 50}});
  std::vector<AytherSpriteOccurrence> sprites(2);
  std::memset(sprites.data(), 0, sprites.size() * sizeof sprites[0]);
  sprites[0].hash = 0x11;
  sprites[0].anim_group_id = 0x77;
  sprites[1].hash = 0x22;
  std::uint16_t cram[64] = {};
  for (int i = 0; i < 64; ++i)
    cram[i] = static_cast<std::uint16_t>(i * 3);
  for (std::uint32_t frame = 1; frame <= 50; ++frame) {
    ayther::FrameView v;
    if (frame == 10 || frame == 45) {
      v.sprite_occs = sprites.data();
      v.sprite_occ_count = frame == 10 ? 2 : 1;
    }
    // no scene CRAM in the view: the collector takes the fallback
    c.frame(frame, v, reinterpret_cast<const std::uint8_t *>(cram),
            sizeof cram);
  }
  return c.finish();
}

void collector() {
  const auto ids = collect();
  check(ids.frames == 50 && ids.segments.size() == 2, "50 frames, 2 segments");
  check(cls(ids, 0, "sprite").ids.size() == 2 &&
            cls(ids, 0, "sprite").occurrences == 2,
        "frame 10 files both sprites in segment 1-40");
  check(cls(ids, 1, "sprite").ids.size() == 1 &&
            cls(ids, 1, "sprite").ids.count(0x11) == 1,
        "frame 45 files one sprite in segment 31-50 only");
  check(cls(ids, 0, "anim_group").ids.size() == 1,
        "animation group 0 is skipped, 0x77 kept");
  // the CRAM is stable from frame 1: each line latches at frame 30, which is
  // inside 1-40 and before 31-50
  check(cls(ids, 0, "palette_variant").occurrences == 4 &&
            cls(ids, 0, "palette_variant").ids.size() == 4,
        "four palette lines latch once at frame 30");
  check(cls(ids, 1, "palette_variant").occurrences == 0,
        "a latched line is not filed again while it stays stable");
}

void round_trip_and_compare() {
  const auto ids = collect();
  const std::string text = format_identities(ids);
  std::string error;
  const auto back = parse_identities(text, &error);
  check(back.has_value(), "the formatted file parses");
  check(back && format_identities(*back) == text, "and formats back the same");
  const auto same = compare_identities(ids, *back);
  check(same.ok && same.equal(), "a file equals itself");

  auto other = *back;
  other.segments[0].classes["sprite"].ids.erase(0x22);
  other.segments[0].classes["sprite"].ids.insert(0x33);
  const auto diff = compare_identities(ids, other);
  bool found = false;
  for (const auto &c : diff.segments[0].classes)
    if (c.name == "sprite")
      found = c.missing == 1 && c.extra == 1 && c.missing_ids[0] == 0x22 &&
              c.extra_ids[0] == 0x33;
  check(diff.ok && !diff.equal() && found,
        "a changed sprite is one missing and one extra");
  check(comparison_json(diff).find("\"equal\":false") != std::string::npos,
        "the JSON says different");

  auto shifted = *back;
  shifted.segments[1].range.first = 32;
  check(!compare_identities(ids, shifted).ok,
        "files over other segments do not compare");
  check(!parse_identities("parity_bench identities 2\n", &error),
        "another file version is rejected");
  check(!parse_identities(text + "id 0000000000000001\n", &error),
        "an id beyond its class count is rejected");

  const std::string head = "parity_bench identities 1\r\n";
  const struct {
    std::string text;
    const char *what;
  } bad[] = {
      {head + "frames x\n", "bad frames"},
      {head + "frames 5\nsegment 1 x\n", "bad segment"},
      {head + "frames 5\nclass sprite occurrences 1 distinct 1\n",
       "class before segment"},
      {head + "frames 5\nsegment 1 5\nclass nope occurrences 1 distinct 1\n",
       "unknown class"},
      {head + "frames 5\nsegment 1 5\nclass sprite occurrences 1 distinct 1\n"
              "class tile occurrences 0 distinct 0\n",
       "class short of ids before the next class"},
      {head + "frames 5\nsegment 1 5\nclass sprite occurrences 1 distinct 1\n"
              "segment 6 9\n",
       "class short of ids before the next segment"},
      {head + "frames 5\nsegment 1 5\nclass sprite occurrences 1 distinct 1\n",
       "class short of ids at the end"},
      {head + "frames 5\nsegment 1 5\nclass sprite occurrences 1 distinct 2\n"
              "id 00000000000000zz\n",
       "bad id"},
      {head + "frames 5\nbogus 1\n", "unknown record"},
      {head, "truncated file"},
  };
  for (const auto &b : bad)
    check(!parse_identities(b.text, nullptr), b.what);

  Identities fewer = *back;
  fewer.segments.pop_back();
  const auto apart = compare_identities(ids, fewer);
  check(!apart.ok && !apart.equal() && !apart.error.empty(),
        "files with a different segment count do not compare");
}

void collector_views() {
  IdentityCollector c({{1, 2}});
  AytherTileOccurrence tile{};
  tile.hash = 0x44;
  ayther::PlaneCellHit cell;
  cell.hash = 0x55;
  ayther::FrameView v;
  v.tile_occs = &tile;
  v.tile_occ_count = 1;
  v.plane_cells = &cell;
  v.plane_cell_count = 1;
  c.frame(1, v, nullptr, 0); // no CRAM: palette lines do not run
  const auto ids = c.finish();
  check(cls(ids, 0, "tile").ids.count(0x44) == 1 &&
            cls(ids, 0, "plane_tile").ids.count(0x55) == 1,
        "tiles and plane cells are filed");
  check(cls(ids, 0, "palette_variant").occurrences == 0,
        "without CRAM no palette line latches");
}
} // namespace

int main() {
  segments();
  collector();
  round_trip_and_compare();
  collector_views();
  std::printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
  return failures ? 1 : 0;
}
