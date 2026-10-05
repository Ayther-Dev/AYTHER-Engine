// Spec 002, DI-17 (RF-10.1, RF-10.3): a frame with raster writes in
// mid-screen is non-composable only on the lines those writes touch. From
// the core's per-line raster journal and the VDP state at the end of the
// frame, session/raster_bands.h finds the bands of lines whose state differs
// from the one the scene is composed with; the rest of the frame keeps HD.
#include "session/raster_bands.h"

#include <array>
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

namespace rb = ayther::session;
using Event = rb::RasterEvent;

constexpr std::uint32_t kReg = rb::kRasterReasonReg;
constexpr std::uint32_t kCram = rb::kRasterReasonCram;
constexpr std::uint32_t kVsram = rb::kRasterReasonVsram;
constexpr std::uint32_t kHscroll = rb::kRasterReasonHscroll;
constexpr std::uint32_t kVram = 1U << 6;
constexpr std::uint32_t kDma = 1U << 4;

struct State {
  std::array<std::uint8_t, 0x20> regs{};
  std::array<std::uint8_t, 0x80> cram{};
  std::array<std::uint8_t, 0x50> vsram{};
  std::vector<std::uint8_t> vram = std::vector<std::uint8_t>(0x10000, 0);
  rb::RasterFinalState view() const { return {regs, cram, vsram, vram}; }
  void set_cram(int index, std::uint16_t raw) {
    cram[index * 2] = static_cast<std::uint8_t>(raw & 0xFF);
    cram[index * 2 + 1] = static_cast<std::uint8_t>(raw >> 8);
  }
};

bool band(const rb::RasterBands &b, std::size_t i, int y0, int y1) {
  return i < b.count && b.bands[i].y0 == y0 && b.bands[i].y1 == y1;
}
} // namespace

int main() try {
  State s;
  s.set_cram(5, 0x0EEE); // the colour the frame ends with
  s.regs[7] = 0x00;

  {
    const rb::RasterBands b = rb::raster_bands(0, {}, 0, s.view(), 224);
    check(b.localized && b.count == 0,
          "a frame without raster writes has no band");
  }
  {
    // A sign: entry 5 changes at line 48 and is restored at line 117.
    const std::array<Event, 2> ev{Event{48, kCram, 5, 0x0222},
                                  Event{117, kCram, 5, 0x0EEE}};
    const rb::RasterBands b = rb::raster_bands(kCram, ev, 0, s.view(), 224);
    std::printf("  sign: localized=%d count=%u [%d,%d)\n", b.localized, b.count,
                b.count ? b.bands[0].y0 : -1, b.count ? b.bands[0].y1 : -1);
    check(b.localized && b.count == 1 && band(b, 0, 48, 117),
          "a palette change restored before the end touches only its band");
  }
  {
    // Changed at line 100 and restored in vblank (not in the journal): the
    // lines from 100 down are drawn with a colour the frame does not end with.
    const std::array<Event, 1> ev{Event{100, kCram, 5, 0x0222}};
    const rb::RasterBands b = rb::raster_bands(kCram, ev, 0, s.view(), 224);
    check(b.localized && b.count == 1 && band(b, 0, 100, 224),
          "a change not restored in the active area runs to the bottom");
  }
  {
    // A write of the value the frame ends with changes nothing.
    const std::array<Event, 1> ev{Event{60, kCram, 5, 0x0EEE}};
    const rb::RasterBands b = rb::raster_bands(kCram, ev, 0, s.view(), 224);
    check(b.localized && b.count == 0,
          "writing the final value touches no line");
  }
  {
    // Register, VSRAM and hscroll writes, two separate bands.
    s.regs[0x0B] = 0x00;
    s.vsram[4] = 0x10;
    s.vsram[5] = 0x00;
    s.vram[0xFC00] = 0x34;
    s.vram[0xFC01] = 0x12;
    const std::array<Event, 6> ev{Event{20, kReg, 0x0B, 0x03},
                                  Event{30, kReg, 0x0B, 0x00},
                                  Event{150, kVsram, 4, 0x0020},
                                  Event{160, kVsram, 4, 0x0010},
                                  Event{158, kHscroll, 0xFC00, 0x9999},
                                  Event{165, kHscroll, 0xFC00, 0x1234}};
    const rb::RasterBands b =
        rb::raster_bands(kReg | kVsram | kHscroll, ev, 0, s.view(), 224);
    check(b.localized && b.count == 2 && band(b, 0, 20, 30) &&
              band(b, 1, 150, 165),
          "register, VSRAM and hscroll writes give their bands, merged where "
          "they overlap");
  }
  {
    // A DMA into CRAM is journaled like a CPU write.
    const std::array<Event, 2> ev{Event{48, kCram, 5, 0x0222},
                                  Event{60, kCram, 5, 0x0EEE}};
    const rb::RasterBands b =
        rb::raster_bands(kCram | kDma, ev, 0, s.view(), 224);
    check(b.localized && band(b, 0, 48, 60), "a DMA into CRAM is localized");
  }
  {
    // Pattern writes are not journaled: without the lines where the frame
    // differs from its recomposition, the frame cannot be localized.
    const rb::RasterBands b =
        rb::raster_bands(kVram | kDma, {}, 0, s.view(), 224);
    check(!b.localized,
          "a mid-screen VRAM pattern write without a recomposition is "
          "whole-frame");
  }
  {
    // With them, the pattern writes touch exactly the lines where the core's
    // image differs from the frame recomposed from the final state.
    std::vector<std::uint8_t> diff(224, 0);
    for (int y = 48; y < 117; ++y)
      diff[static_cast<std::size_t>(y)] = 1;
    const rb::RasterBands b =
        rb::raster_bands(kVram, {}, 0, s.view(), 224, diff);
    check(b.localized && b.count == 1 && band(b, 0, 48, 117),
          "a VRAM pattern write is localized to the lines that differ from "
          "the recomposition");
    const std::array<Event, 2> ev{Event{150, kCram, 5, 0x0222},
                                  Event{160, kCram, 5, 0x0EEE}};
    const rb::RasterBands u =
        rb::raster_bands(kVram | kCram, ev, 0, s.view(), 224, diff);
    check(u.localized && u.count == 2 && band(u, 0, 48, 117) &&
              band(u, 1, 150, 160),
          "pattern and palette writes together give both bands");
  }
  {
    // The lines where two RGB565 images differ.
    std::vector<std::uint16_t> a(320 * 4, 0x1234);
    std::vector<std::uint16_t> c(330 * 4, 0x1234); // pitch 330 px
    c[2 * 330 + 319] = 0x0000;
    std::vector<std::uint8_t> lines(4, 9);
    rb::differing_lines(a.data(), 320, c.data(), 330, 320, 4, lines);
    check(lines[0] == 0 && lines[1] == 0 && lines[2] == 1 && lines[3] == 0,
          "differing_lines flags exactly the lines with a different pixel");
  }
  {
    const std::array<Event, 1> ev{Event{48, kCram, 5, 0x0222}};
    check(!rb::raster_bands(kCram, ev, 3, s.view(), 224).localized,
          "a journal with dropped events is whole-frame");
    check(!rb::raster_bands(kCram | rb::kRasterReasonJournalOverflow, ev, 0,
                            s.view(), 224)
               .localized,
          "an overflowed journal is whole-frame");
    check(!rb::raster_bands(kCram, {}, 0, s.view(), 224).localized,
          "raster writes without a journal are whole-frame");
  }
  {
    // More runs than bands: the nearest are merged, nothing is dropped.
    std::vector<Event> ev;
    for (int i = 0; i < 40; ++i) {
      ev.push_back(Event{static_cast<std::uint16_t>(i * 5), kCram, 5, 0x0222});
      ev.push_back(
          Event{static_cast<std::uint16_t>(i * 5 + 2), kCram, 5, 0x0EEE});
    }
    const rb::RasterBands b = rb::raster_bands(kCram, ev, 0, s.view(), 224);
    bool covered = b.localized && b.count <= rb::kMaxRasterBands;
    for (int i = 0; covered && i < 40; ++i) {
      bool in = false;
      for (std::uint32_t k = 0; k < b.count; ++k)
        in = in || (b.bands[k].y0 <= i * 5 && i * 5 + 2 <= b.bands[k].y1);
      covered = in;
    }
    check(covered, "more runs than bands merge without dropping a line");
  }
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
