// Spec 002, BR-110 and BR-112 (RF-10.1): the scene publishes the scroll the
// compositor needs — the 2-cell column vscroll of planes A and B
// (FrameView::plane_vscroll_col) and the hscroll of every visible line
// (FrameView::plane_hscroll_lines) — and they match the frame's VSRAM and
// hscroll table. The test core writes its VDP registers and VSRAM; its
// registers select 2-cell vscroll.
#include <ayther/ayther_session.h>

#include "../../tools/common/synth_rom.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <utility>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

std::uint32_t le32(const std::uint8_t *m, std::size_t size, std::size_t o) {
  if (m == nullptr || o + 3 >= size)
    return 0;
  return (std::uint32_t)m[o] | ((std::uint32_t)m[o + 1] << 8) |
         ((std::uint32_t)m[o + 2] << 16) | ((std::uint32_t)m[o + 3] << 24);
}
} // namespace

int main() try {
  const auto rom_path =
      std::filesystem::temp_directory_path() / "ayther_scroll_tables.md";
  {
    ayther::synth::Rom rom("AYTHER SCROLL TABLES");
    ayther::synth::program_canonical(rom, true);
    if (!rom.save(rom_path))
      return 1;
  }
  ayther::AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = rom_path.string();
  config.enable_audio = false;
  config.derive_core_pack = false;
  auto created = ayther::AytherSession::create(config);
  if (!created) {
    std::fprintf(stderr, "[FAIL] %s\n", created.error.message.c_str());
    return 1;
  }
  std::unique_ptr<ayther::AytherSession> s = std::move(*created);
  const ayther::FrameView *v = nullptr;
  // The test core writes 32 random VRAM bytes a frame: after 600 frames its
  // hscroll table holds non-zero entries, so the comparison means something.
  for (int f = 0; f < 600; ++f)
    v = &s->step();

  std::size_t vram_size = 0;
  std::size_t regs_size = 0;
  std::size_t vsram_size = 0;
  const std::uint8_t *vram = s->video_ram(&vram_size);
  const std::uint8_t *regs = s->vdp_regs(&regs_size);
  const std::uint8_t *vsram = s->vsram(&vsram_size);
  check(vram != nullptr && regs != nullptr && regs_size >= 0x20 &&
            vsram != nullptr && vsram_size >= 80,
        "the session exposes VRAM, VDP registers and VSRAM");
  if (vram == nullptr || regs == nullptr || vsram == nullptr)
    return 1;
  check((regs[0x0B] & 0x04) != 0, "the test core selects 2-cell vscroll");

  // BR-110: the column vscroll of planes A and B.
  bool columns = true;
  bool varies = false;
  for (int c = 0; c < 20; ++c) {
    const std::uint32_t w = le32(vsram, vsram_size, (std::size_t)c * 4);
    varies = varies || w != le32(vsram, vsram_size, 0);
    columns = columns && v->plane_vscroll_col[0][c] == (int16_t)(w & 0x3FF) &&
              v->plane_vscroll_col[1][c] == (int16_t)((w >> 16) & 0x3FF);
  }
  // The test core leaves its VSRAM uniform: this only fixes the plumbing.
  // plane_bands_test covers the VSRAM columns with varied values.
  std::printf("  VSRAM varies between columns: %s\n", varies ? "yes" : "no");
  check(columns, "RF-10.1: the scene's column vscroll matches the VSRAM");

  // BR-112: the hscroll of every visible line.
  const std::uint32_t hscb = ((std::uint32_t)regs[0x0D] << 10) & 0xFC00;
  const std::uint32_t mask_tab[4] = {0x00, 0x07, 0xF8, 0xFF};
  const std::uint32_t mask = mask_tab[regs[0x0B] & 3];
  bool lines = v->fb_height > 0;
  bool nonzero = false;
  for (std::uint32_t y = 0; y < v->fb_height && y < 240; ++y) {
    const std::uint32_t w = le32(vram, vram_size, hscb + ((y & mask) << 2));
    nonzero = nonzero || (w & 0x03FF03FFU) != 0;
    lines = lines && v->plane_hscroll_lines[0][y] == (int16_t)(w & 0x3FF) &&
            v->plane_hscroll_lines[1][y] == (int16_t)((w >> 16) & 0x3FF);
  }
  check(nonzero, "the frame's hscroll table has non-zero lines");
  check(lines, "RF-10.1: the scene's per-line hscroll matches the hscroll "
               "table");

  // BR-111/BR-113: with the scroll composed per band, a varying hscroll or
  // column vscroll no longer makes the frame dirty (scene_dirty bit 4). The
  // test core raises raster activity on 16 frames of 17.
  bool scroll_dirty = false;
  bool clean = false;
  for (int f = 0; f < 34; ++f) {
    const ayther::FrameView &fv = s->step();
    scroll_dirty = scroll_dirty || (fv.scene_dirty & 4U) != 0;
    clean = clean || fv.scene_dirty == 0;
  }
  check(!scroll_dirty && clean,
        "RF-10.1: a frame with varying scroll and no raster is composable");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
