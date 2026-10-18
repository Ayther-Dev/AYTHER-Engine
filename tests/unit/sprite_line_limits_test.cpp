// Spec 002, BR-096 (RF-9.4): the per-line sprite limit and the x = 0 mask,
// as the core draws them, according to genesis_plus_gx_no_sprite_limit. A
// line with more sprites than the limit loses the last ones of the chain
// unless the option lifts it; a sprite at raw x = 0 after a sprite with
// x != 0 masks the rest of the line with or without the option.
#include "session/sprite_line_limits.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

using ayther::session::drawn_on_some_line;
using ayther::session::drawn_rows;
using ayther::session::line_limits_for;
using ayther::session::ParsedSprite;

ParsedSprite at(int x, int y, std::uint8_t chain, std::uint8_t w = 1) {
  ParsedSprite s;
  s.x_raw = static_cast<std::int16_t>(x + 128);
  s.y = static_cast<std::int16_t>(y);
  s.w_tiles = w;
  s.h_tiles = 1;
  s.chain = chain;
  return s;
}
} // namespace

int main() try {
  // 22 one-tile sprites on lines 40..47 of an H40 frame.
  std::vector<ParsedSprite> row;
  for (std::uint8_t i = 0; i < 22; ++i)
    row.push_back(at(10 * i, 40, i));
  const auto limited =
      drawn_on_some_line(row, 224, line_limits_for(320, false));
  check(limited.size() == 22 && limited[19] == 1 && limited[20] == 0 &&
            limited[21] == 0,
        "RF-9.4: over 20 sprites on a line, the 21st and 22nd of the chain "
        "are not drawn");
  const auto unlimited =
      drawn_on_some_line(row, 224, line_limits_for(320, true));
  bool all = unlimited.size() == 22;
  for (const std::uint8_t d : unlimited)
    all = all && d == 1;
  check(all, "RF-9.4: with no_sprite_limit every sprite of the line is drawn");

  // H32: 16 sprites per line.
  const auto h32 = drawn_on_some_line(row, 224, line_limits_for(256, false));
  check(h32[15] == 1 && h32[16] == 0, "RF-9.4: H32 draws 16 sprites per line");

  // Pixel limit: ten 4-tile sprites (32 px each) = 320 px; an 11th is not
  // drawn on an H40 line even though only 11 sprites share it.
  std::vector<ParsedSprite> wide;
  for (std::uint8_t i = 0; i < 11; ++i)
    wide.push_back(at(4 * i, 100, i, 4));
  const auto px = drawn_on_some_line(wide, 224, line_limits_for(320, false));
  check(px[9] == 1 && px[10] == 0,
        "RF-9.4: past 320 sprite pixels on a line, the next sprite is not "
        "drawn");

  // x = 0 mask: A (x = 50) then the mask (raw x = 0) then B on the same line.
  std::vector<ParsedSprite> mask = {at(50, 60, 0), at(-128, 60, 1),
                                    at(80, 60, 2)};
  const auto masked = drawn_on_some_line(mask, 224, line_limits_for(320, true));
  check(masked[0] == 1 && masked[2] == 0,
        "RF-9.4: a sprite at x = 0 after one with x != 0 masks the rest of "
        "the line");
  // Without a sprite before it, the mask does nothing.
  std::vector<ParsedSprite> first = {at(-128, 60, 0), at(80, 60, 1)};
  const auto unmasked =
      drawn_on_some_line(first, 224, line_limits_for(320, true));
  check(unmasked[1] == 1,
        "the x = 0 sprite first on the line does not mask the next one");

  // A sprite below the frame covers no visible line: nothing to judge.
  std::vector<ParsedSprite> below = {at(10, 300, 0)};
  check(drawn_on_some_line(below, 224, line_limits_for(320, false))[0] == 1,
        "a sprite on no visible line is not judged");

  // Sonic 1 title (per-line pixel budget): ten 32x32 sprites parked off
  // screen on lines 104..135 spend the whole H40 budget there; a 32x16
  // sprite later in the chain at y = 102 is drawn on lines 102-103 only.
  std::vector<ParsedSprite> sonic;
  for (std::uint8_t i = 0; i < 10; ++i) {
    ParsedSprite d = at(-48, 104, i, 4);
    d.h_tiles = 4;
    sonic.push_back(d);
  }
  ParsedSprite body = at(120, 102, 10, 4);
  body.h_tiles = 2;
  sonic.push_back(body);
  const auto rows = drawn_rows(sonic, 224, line_limits_for(320, false));
  check(rows[10].visible && rows[10].drawn() && rows[10].y0 == 102 &&
            rows[10].y1 == 104,
        "RF-9.4: a sprite crossing the lines whose budget is spent is drawn "
        "only above them");
  check(rows[10].partial(sonic[10], 224),
        "RF-9.4: that sprite is reported as partially drawn");
  check(!rows[0].partial(sonic[0], 224) && rows[0].y0 == 104 &&
            rows[0].y1 == 136,
        "a sprite drawn on all its lines is not partial");
  check(drawn_on_some_line(sonic, 224, line_limits_for(320, false))[10] == 1,
        "a partially drawn sprite is still drawn on some line");
  const auto lifted = drawn_rows(sonic, 224, line_limits_for(320, true));
  check(!lifted[10].partial(sonic[10], 224) && lifted[10].y1 == 118,
        "with no_sprite_limit the sprite is drawn whole");
  // Straddling the bottom edge of the frame is not a partial draw.
  ParsedSprite edge = at(10, 216, 0, 1);
  edge.h_tiles = 2;
  const std::vector<ParsedSprite> edge_list = {edge};
  const auto er = drawn_rows(edge_list, 224, line_limits_for(320, false));
  check(!er[0].partial(edge, 224) && er[0].y0 == 216 && er[0].y1 == 224,
        "the frame edge does not make a sprite partial");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
