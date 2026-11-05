// identity_contract_kat_test.cpp - TP-011.4: the identity contract version and
// the known answers it covers, with no ROM, GPU or session. The numbers are the
// published vectors of core/src/identity_kat.rs; a change here means published
// packs stop matching and AYTHER_IDENTITY_CONTRACT must be raised.
#include <ayther/ayther_identity_contract.h>

#include "ayther_core_ffi.h"
#include "ayther_element_record.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
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
  // palette_variant: the synthetic CRAM word i is (i * 29 + 5) & 0x1FF; the
  // answers are those of core/src/identity_kat.rs (pose needs xxh3 and has no
  // C entry point, so it is covered on the Rust side only).
  std::vector<uint16_t> cram(64);
  for (size_t i = 0; i < cram.size(); ++i)
    cram[i] = static_cast<uint16_t>((i * 29 + 5) & 0x1FF);
  check(ayther_palette_signature(cram.data(), 64, 0, 0xFFFE) ==
            0x327269cf7f752943ULL,
        "palette variant, line 0, slots 1..15 matches the published vector");
  check(ayther_palette_signature(cram.data(), 64, 2, 0x0012) ==
            0xe602f24f070befbcULL,
        "palette variant, line 2, slots 1 and 4 matches the published vector");
  // Audio classes: FNV-1a with the AYTHER seed, written out here from
  // docs/IDENTITY_SPECIFICATION.md and compared with the values the Rust
  // detector produces for the synthetic stream of audio_class_identities().
  const auto mix = [](std::initializer_list<uint8_t> bytes) {
    uint64_t h = 0x14650FB0739D0383ULL;
    for (const uint8_t b : bytes) {
      h ^= b;
      h *= 0x00000100000001B3ULL;
    }
    return h;
  };
  check(mix({1, 0, 0x0E, 0x00}) == 0x917b96983089543cULL,
        "PSG tone signature matches the published vector");
  check(mix({0, 5, 0xDA, 0x10, 0xF0}) == 0x9fafa7e158f9353aULL,
        "DAC signature matches the published vector");
  check(mix({3, 2, 0x10, 0x23, 0x01, 0x00, 0x04}) == 0x5975df5f33250ed2ULL,
        "PCM signature matches the published vector");
  return failures ? 1 : 0;
}
