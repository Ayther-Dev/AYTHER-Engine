// Spec 002, BR-023 (RF-7.9, RF-10.1): why a frame cannot be composed element
// by element (contracts.md C3 Composability), from synthetic frames: raster
// in mid-screen, the authoring dim, per-line hscroll and per-column vscroll,
// and a clean frame. The scene_dirty bits the renderer reads come from the
// same computation.
#include "session/emulation_observer.h"
#include "session/frame_composability.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

using ayther::engine::render_observation::Composability;
using ayther::session::classify_frame;
using ayther::session::FrameComposability;
using ayther::session::FrameComposabilityInput;

constexpr std::uint32_t kHscrollTable = 0xFC00; // VDP register 13 = 0x3F

struct Frame {
  std::vector<std::uint8_t> vram = std::vector<std::uint8_t>(0x10000, 0);
  std::vector<std::uint8_t> regs = std::vector<std::uint8_t>(0x20, 0);
  std::vector<std::uint8_t> vsram = std::vector<std::uint8_t>(80, 0);
  std::uint32_t raster = 0;
  bool dim = false;
  bool parsed_sprites_complete = true;
  bool geometry_current = true;

  Frame() {
    regs[1] = 0x74; // the display is on (bit 6)
    regs[13] = 0x3F;
  }

  void hscroll(std::uint32_t line, std::uint16_t plane_a) {
    const std::uint32_t at = kHscrollTable + line * 4;
    vram[at] = static_cast<std::uint8_t>(plane_a & 0xFF);
    vram[at + 1] = static_cast<std::uint8_t>(plane_a >> 8);
  }
  void vscroll(std::uint32_t column, std::uint16_t plane_a) {
    vsram[column * 4] = static_cast<std::uint8_t>(plane_a & 0xFF);
    vsram[column * 4 + 1] = static_cast<std::uint8_t>(plane_a >> 8);
  }
  [[nodiscard]] FrameComposability classify() const {
    FrameComposabilityInput in;
    in.raster = raster;
    in.layer_dim = dim;
    in.parsed_sprites_complete = parsed_sprites_complete;
    in.geometry_current = geometry_current;
    in.vram = vram;
    in.vdp_regs = regs;
    in.vsram = vsram;
    in.fb_width = 320;
    in.fb_height = 224;
    return classify_frame(in);
  }
};
} // namespace

int main() try {
  {
    const FrameComposability c = Frame{}.classify();
    check(c.reason == Composability::composable && c.scene_dirty == 0,
          "RF-10.1: a clean frame is composable");
  }
  {
    Frame f;
    f.geometry_current = false;
    const FrameComposability c = f.classify();
    check(c.reason == Composability::other && c.scene_dirty != 0,
          "RF-10.1/O1: GEOMETRY_PENDING makes the emitted frame "
          "non-composable even without raster writes or sprites");
    check(ayther::session::frame_requires_core_image(c.scene_dirty),
          "RF-10.1/O1: the renderer shows the complete core image while "
          "VDP_REGS describe the following geometry");
  }
  {
    ayther_frame_snapshot_v1 snapshot{};
    snapshot.parsed_sprite_count = 128;
    snapshot.overflow_flags = AYTHER_OVERFLOW_PARSED_SPRITES;
    check(!ayther::session::parsed_sprite_capture_complete(snapshot),
          "RF-8.1/RF-9.1/O1: a 128-record PARSED_SPRITES snapshot with "
          "overflow is explicitly incomplete");

    Frame f;
    f.parsed_sprites_complete = false;
    const FrameComposability c = f.classify();
    check(c.reason == Composability::other &&
              (c.scene_dirty &
               ayther::session::kDirtyIncompleteSpriteCapture) != 0,
          "RF-8.1/RF-9.1/O1: incomplete parsed sprites force the complete "
          "core frame with no HD composition");
    check(ayther::session::frame_requires_core_image(c.scene_dirty),
          "RF-10.1/O1: the renderer gate consumes the incomplete-capture "
          "dirty bit");

    std::uint8_t truncated_prefix = 0xA5;
    using Observer = ayther::session::EmulationObserver;
    const Observer::ParsedSpritesView legacy{&truncated_prefix, 128, false,
                                             true};
    const Observer::ParsedSpritesView incomplete_abi{nullptr, 0, true, false};
    const auto selected = Observer::prefer_snapshot_parsed_sprites(
        std::optional{incomplete_abi}, legacy);
    check(selected.abi && !selected.complete && selected.data == nullptr &&
              selected.count == 0,
          "RF-8.1/RF-9.1/O1: an authoritative empty/incomplete ABI view "
          "never falls back to the 128-record legacy prefix");
  }
  {
    Frame f;
    f.raster = 1;
    const FrameComposability c = f.classify();
    check(c.reason == Composability::raster_split && c.scene_dirty == 1,
          "RF-7.9: raster writes in mid-screen -> raster_split");
  }
  {
    Frame f;
    f.dim = true;
    const FrameComposability c = f.classify();
    check(c.reason == Composability::fade && c.scene_dirty == 2,
          "RF-7.9: the authoring dim -> fade");
  }
  {
    Frame f;
    f.regs[11] = 3; // per-line hscroll
    f.hscroll(100, 5);
    const FrameComposability c = f.classify();
    check(c.reason == Composability::line_hscroll && c.scene_dirty == 4,
          "RF-10.1: per-line hscroll that varies -> line_hscroll");
  }
  {
    Frame f;
    f.regs[11] = 2; // per-cell hscroll: the table is read every 8 lines
    f.hscroll(16, 5);
    const FrameComposability c = f.classify();
    check(c.reason == Composability::line_hscroll,
          "RF-10.1: per-cell hscroll that varies -> line_hscroll");
  }
  {
    Frame f;
    f.regs[11] = 3;
    for (std::uint32_t line = 0; line < 224; ++line)
      f.hscroll(line, 7);
    f.hscroll(230, 9); // below the visible lines
    const FrameComposability c = f.classify();
    check(c.reason == Composability::composable && c.scene_dirty == 0,
          "a uniform hscroll over the visible lines composes");
  }
  {
    Frame f;
    f.regs[11] = 4; // 2-cell column vscroll
    f.vscroll(3, 12);
    const FrameComposability c = f.classify();
    check(c.reason == Composability::column_vscroll && c.scene_dirty == 4,
          "RF-10.1: per-column vscroll that varies -> column_vscroll");
  }
  {
    Frame f;
    f.regs[11] = 4;
    f.vsram[19 * 4 + 2] = 1; // plane B of the last visible column
    const FrameComposability c = f.classify();
    check(c.reason == Composability::column_vscroll,
          "the plane B entry of a visible column counts too");
  }
  {
    Frame f;
    f.regs[11] = 0; // full-screen scroll: the tables are not used
    f.hscroll(100, 5);
    f.vscroll(3, 12);
    const FrameComposability c = f.classify();
    check(c.reason == Composability::composable,
          "full-screen scroll ignores the per-line and per-column tables");
  }
  {
    Frame f;
    f.raster = 1;
    f.dim = true;
    f.regs[11] = 7;
    f.hscroll(100, 5);
    f.vscroll(3, 12);
    const FrameComposability c = f.classify();
    check(c.reason == Composability::raster_split && c.scene_dirty == 7,
          "several reasons: the first in contract order, every dirty bit");
  }
  {
    Frame f;
    f.regs[11] = 7;
    f.hscroll(100, 5);
    f.vscroll(3, 12);
    const FrameComposability c = f.classify();
    check(c.reason == Composability::line_hscroll && c.scene_dirty == 4,
          "hscroll goes before vscroll");
  }
  {
    Frame f;
    f.vram.resize(0x8000);
    f.regs[11] = 3;
    f.hscroll(10, 5);
    const FrameComposability c = f.classify();
    check(c.reason == Composability::composable,
          "without the whole VRAM the scroll tables are not read");
  }
  {
    // Spec 002 (A, Toma 3 frame 2): with the display off (register 1 bit 6)
    // the VDP shows the backdrop only, whatever the planes and sprites hold.
    Frame f;
    f.regs[1] = 0x34;
    const FrameComposability c = f.classify();
    check(c.reason == Composability::other &&
              c.scene_dirty == ayther::session::kDirtyDisplayOff,
          "A: a frame with the display off is not composable (other)");
  }
  {
    Frame f;
    f.regs[1] = 0x34;
    f.raster = 1;
    f.regs[11] = 3;
    f.hscroll(100, 5);
    const FrameComposability c = f.classify();
    check(c.reason == Composability::raster_split &&
              (c.scene_dirty & ayther::session::kDirtyDisplayOff) != 0,
          "A: turned off mid-screen: raster_split first, display-off bit set");
    f.raster = 0;
    check(f.classify().reason == Composability::other,
          "A: display off goes before the scroll reasons the session clears");
  }

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
