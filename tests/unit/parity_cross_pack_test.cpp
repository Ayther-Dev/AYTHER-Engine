// Spec 008 (TP-210.1, RF-10.3): the cross-pack play line and its comparison.
// A play line carries, per frame, the multiset of substitutions (order-free)
// and the presented frame (the renderer's input in drawing order).
#include "../../tools/parity_bench/cross_pack.h"

#include <ayther/ayther_session.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

AytherTileSub tile(const char *asset, std::uint32_t x, std::uint32_t y) {
  AytherTileSub t{};
  std::snprintf(t.asset_path, sizeof t.asset_path, "%s", asset);
  t.tile_x = x;
  t.tile_y = y;
  return t;
}

std::string line_of(const ayther::FrameView &v, std::string *detail = nullptr) {
  std::uint64_t fb = 0x1234;
  std::uint32_t count = 224;
  return ayther::parity::play_line(7, v, fb, count, detail);
}

std::string field(const std::string &line, const char *key) {
  const auto at = line.find(std::string("\"") + key + "\":{\"hash\":\"");
  return at == std::string::npos ? std::string{}
                                 : line.substr(at + std::strlen(key) + 12, 16);
}

std::string play(const char *a, const char *b) {
  return std::string("{\"frame\":1,\"subs\":{\"hash\":\"") + a +
         "\",\"count\":2},\"presented\":{\"hash\":\"" + b +
         "\",\"count\":224}}\n";
}
} // namespace

int main() {
  using namespace ayther::parity;
  AytherTileSub tiles[2] = {tile("t_a.png", 1, 2), tile("t_b.png", 3, 4)};
  AytherSpriteSub sprite{};
  std::snprintf(sprite.asset_path, sizeof sprite.asset_path, "s_c.png");
  sprite.screen_x = 10;
  sprite.screen_y = 20;
  std::uint8_t tint[3] = {64, 64, 64};

  ayther::FrameView v{};
  v.tile_subs = tiles;
  v.tile_sub_count = 2;
  v.sprite_subs = &sprite;
  v.sprite_sub_count = 1;
  v.sprite_sub_tint = tint;

  std::string detail;
  const std::string base = line_of(v, &detail);
  check(base == line_of(v), "the same frame gives the same line");
  check(base.find("\"count\":3}") != std::string::npos,
        "subs counts every substitution");
  check(detail == "7 sprite s_c.png 10 20\n7 tile t_a.png 1 2\n"
                  "7 tile t_b.png 3 4\n",
        "detail lists the multiset sorted, one entry per line");

  std::swap(tiles[0], tiles[1]);
  const std::string swapped = line_of(v);
  check(field(swapped, "subs") == field(base, "subs"),
        "the multiset does not depend on the order");
  check(field(swapped, "presented") != field(base, "presented"),
        "the presented frame does depend on the drawing order");
  std::swap(tiles[0], tiles[1]);

  tint[1] = 80;
  const std::string tinted = line_of(v);
  check(field(tinted, "subs") == field(base, "subs") &&
            field(tinted, "presented") != field(base, "presented"),
        "a tint change is a presented change, not a multiset change");
  tint[1] = 64;

  tiles[1].tile_x = 5;
  const std::string moved = line_of(v);
  check(field(moved, "subs") != field(base, "subs"),
        "a substitution at another position changes the multiset");
  tiles[1].tile_x = 3;

  std::uint64_t fb = 0x9999; // the framebuffer of the last drawn frame
  std::uint32_t count = 224;
  const std::string other_fb = play_line(7, v, fb, count);
  check(field(other_fb, "subs") == field(base, "subs") &&
            field(other_fb, "presented") != field(base, "presented"),
        "another framebuffer is a presented change");

  const std::string a = play("00000000000000aa", "00000000000000bb");
  auto r = compare_plays(a + a, a + a);
  check(r.equal() && r.frames == 2 && r.substituted_frames == 2,
        "identical plays are equal and count substituted frames");
  const std::string b = play("00000000000000aa", "00000000000000cc");
  r = compare_plays(a + a + a, a + b + b);
  check(r.ok && r.divergences.size() == 1 &&
            r.divergences[0].field == "presented" &&
            r.divergences[0].frames == 2,
        "a presented difference is reported with how many frames differ");
  check(!compare_plays(a, a + a).ok, "plays of different length are an error");
  check(!compare_plays("not a play\n", a).ok, "a malformed play is an error");

  const auto shard = parse_shard("3/10");
  check(shard && shard->index == 2 && shard->count == 10,
        "--shard K/N is 1-based on the command line, 0-based inside");
  check(parse_shard("1/1").has_value(), "1/1 is the whole set");
  check(!parse_shard("0/4") && !parse_shard("5/4") && !parse_shard("4") &&
            !parse_shard("/4") && !parse_shard("1/") && !parse_shard("a/4"),
        "a shard outside 1..N or not K/N is rejected");
  return failures == 0 ? 0 : 1;
}
