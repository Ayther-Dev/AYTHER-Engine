// identity_contract_kat_test.cpp - TP-011.4: the identity contract version and
// the known answers it covers, with no ROM, GPU or session. The numbers are the
// published vectors of core/src/identity_kat.rs; a change here means published
// packs stop matching and AYTHER_IDENTITY_CONTRACT must be raised.
#include <ayther/ayther_identity_contract.h>

#include "ayther_element_record.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}
} // namespace

int main() {
  static_assert(AYTHER_IDENTITY_CONTRACT == 1);
  check(AYTHER_IDENTITY_CONTRACT == 1, "identity contract version is 1");

  std::vector<uint8_t> vram(4096);
  for (size_t i = 0; i < vram.size(); ++i)
    vram[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);

  // The bus word-swap (^1) is applied by the reproducer's input convention:
  // the element_record test builds the same slices; here the tile-1 plane hash
  // is enough to tie the contract number to a concrete answer.
  std::vector<uint8_t> tile(32);
  for (size_t i = 0; i < 32; ++i)
    tile[i] = vram[(32 + i) ^ 1];
  check(ayther::plane_tile_hash(tile.data(), 0) == 0x98d36be2cd04adb9ULL,
        "plane tile hash of pattern 1, line 0 matches the published vector");
  std::vector<uint8_t> t0(32);
  for (size_t i = 0; i < 32; ++i)
    t0[i] = vram[i ^ 1];
  check(ayther::sprite_pattern_hash(t0.data(), t0.size()) ==
            0x41573b4753eaaeb4ULL,
        "sprite hash of tile 0 (1x1) matches the published vector");
  return failures ? 1 : 0;
}
