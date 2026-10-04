// Spec 002, BR-099 (RF-8.1, RF-8.2): the VDP sprite order, pure. The four
// crossed cases of BR-034 (chain against priority) at one pixel, and the
// renderer's two-pass depth scheme agreeing with the VDP rule for every
// combination of up to four overlapping sprites in any draw order.
#include "session/vdp_sprite_order.h"

#include <algorithm>
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

using ayther::session::chain_depth;
using ayther::session::SpriteFragment;
using ayther::session::two_pass_depth;
using ayther::session::vdp_winner;

bool is(const std::optional<ayther::session::SpritePixel> &p,
        std::size_t fragment, std::uint8_t priority) {
  return p.has_value() && p->fragment == fragment && p->priority == priority;
}
} // namespace

int main() try {
  check(chain_depth(0) > 0.0F && chain_depth(255) < 1.0F &&
            chain_depth(3) < chain_depth(4),
        "chain depth is strictly increasing inside (0, 1)");

  // 1. A low sprite first in the chain over a high one: the low one wins and
  //    enters low.
  const std::vector<SpriteFragment> low_front = {{0, 0}, {1, 1}};
  check(is(vdp_winner(low_front), 0, 0),
        "RF-8.1: the first of the chain wins even when it is low priority");
  // 2. Same pixels: the winner enters at its own (low) level, so a high plane
  //    cell covers it and the high sprite behind does not show.
  check(is(two_pass_depth(low_front), 0, 0),
        "RF-8.2: the two passes keep the low front sprite and drop the high "
        "one behind it");
  // 3. A high sprite first in the chain over a low one: the high one wins.
  const std::vector<SpriteFragment> high_front = {{0, 1}, {1, 0}};
  check(is(vdp_winner(high_front), 0, 1) &&
            is(two_pass_depth(high_front), 0, 1),
        "RF-8.2: a high sprite first in the chain wins and enters high");
  // 4. Through a transparent pixel of the front sprite only the next one
  //    remains, with its own priority.
  const std::vector<SpriteFragment> through = {{1, 0}};
  check(is(vdp_winner(through), 0, 0) && is(two_pass_depth(through), 0, 0),
        "RF-8.1: through a transparent pixel the next sprite wins with its "
        "own priority");

  // Every combination of up to 4 fragments (distinct chains 0..3, any
  // priorities), in every draw order: the passes give the VDP's winner.
  bool agree = true;
  int combos = 0;
  for (int n = 1; n <= 4; ++n)
    for (int pri_bits = 0; pri_bits < (1 << n); ++pri_bits) {
      std::vector<SpriteFragment> f;
      for (int i = 0; i < n; ++i)
        f.push_back({static_cast<std::uint8_t>(i),
                     static_cast<std::uint8_t>((pri_bits >> i) & 1)});
      const auto want = vdp_winner(f);
      std::vector<std::size_t> order(f.size());
      for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
      do {
        std::vector<SpriteFragment> drawn;
        for (const std::size_t i : order)
          drawn.push_back(f[i]);
        const auto got = two_pass_depth(drawn);
        ++combos;
        agree = agree && want.has_value() && got.has_value() &&
                drawn[got->fragment].chain == f[want->fragment].chain &&
                got->priority == want->priority;
      } while (std::next_permutation(order.begin(), order.end()));
    }
  std::printf("  combinations checked: %d\n", combos);
  check(agree, "RF-8.1, RF-8.2: the two-pass depth scheme reproduces the VDP "
               "rule in every draw order");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
