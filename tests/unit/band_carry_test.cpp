// Spec 002, DI-20 (RF-10.1, RF-10.3, CV-5): inside a raster band, a pixel the
// core drew exactly as in the previous frame keeps the previous frame's
// composed HD; every other pixel of the band shows the core's image, as
// DI-17 does. session/band_carry.h splits the band lines into runs of each
// kind, and refuses (the band stays wholly the core's) when the runs would
// exceed their bound or the inputs do not describe the same geometry.
#include "session/band_carry.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

namespace s = ayther::session;

struct Frame {
  int width;
  int height;
  std::vector<std::uint16_t> pixels;
  Frame(int w, int h, std::uint16_t fill)
      : width(w), height(h),
        pixels(static_cast<std::size_t>(w) * static_cast<std::size_t>(h),
               fill) {}
  void set(int x, int y, std::uint16_t v) {
    pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x)] = v;
  }
  s::CoreImage view() const {
    return {reinterpret_cast<const std::uint8_t *>(pixels.data()), width,
            height, static_cast<std::size_t>(width) * 2U, 2};
  }
};

bool covers(const s::BandCarry &carry, int y, int width) {
  int next = 0;
  for (std::uint32_t i = 0; i < carry.count; ++i)
    if (carry.runs[i].y == y) {
      if (carry.runs[i].x0 != next)
        return false;
      next = carry.runs[i].x1;
    }
  return next == width;
}
} // namespace

int main() try {
  std::printf("=== band_carry_test (spec 002 DI-20) ===\n");
  const std::array<s::LineBand, 1> band{{{40, 44}}};

  // Static background; the game draws a text box's edge on line 42 only.
  Frame previous(256, 224, 0x1234);
  Frame current = previous;
  for (int x = 30; x < 90; ++x)
    current.set(x, 42, 0xFFE0);
  {
    const s::BandCarry carry =
        s::band_carry(previous.view(), current.view(), band);
    check(carry.usable,
          "DI-20: a band over an unchanged background can keep HD");
    bool whole = true;
    for (int y = 40; y < 44; ++y)
      whole = whole && covers(carry, y, 256);
    check(whole, "DI-20: the runs cover every pixel of every band line once");
    std::uint32_t carried_lines = 0;
    bool edge_from_core = false;
    for (std::uint32_t i = 0; i < carry.count; ++i) {
      const s::CarryRun &r = carry.runs[i];
      if (r.from_previous && r.x0 == 0 && r.x1 == 256)
        ++carried_lines;
      if (r.y == 42 && !r.from_previous && r.x0 == 29 && r.x1 == 91)
        edge_from_core = true;
    }
    check(
        carried_lines == 1,
        "RF-10.3: a band line away from the write keeps the previous HD whole");
    check(edge_from_core && carry.count == 10,
          "RF-10.1: the pixels the write changed show the core's image");
  }

  // Toma 3 frame 1645: an HD letter of the previous page is slightly larger
  // than its original pixels. The neighbours of a changed pixel come from
  // the core too, so no fringe of the previous HD survives around it.
  {
    Frame letter = previous;
    letter.set(100, 41, 0x0001);
    const s::BandCarry carry =
        s::band_carry(previous.view(), letter.view(), band);
    const auto from_core = [&](int x, int y) {
      for (std::uint32_t i = 0; i < carry.count; ++i) {
        const s::CarryRun &r = carry.runs[i];
        if (r.y == y && r.x0 <= x && x < r.x1)
          return !r.from_previous;
      }
      return false;
    };
    check(carry.usable && from_core(99, 40) && from_core(101, 42) &&
              from_core(100, 41) && !from_core(98, 41) && !from_core(100, 43),
          "DI-20: a changed pixel and its 8 neighbours show the core's image");
  }

  // Nothing equal to the previous frame: no run carries HD; the band stays
  // the core's image (DI-17).
  {
    Frame changed(256, 224, 0x0F0F);
    const s::BandCarry carry =
        s::band_carry(previous.view(), changed.view(), band);
    check(!carry.usable,
          "DI-17: with no unchanged pixel the band stays the core's image");
  }

  // Toma 3 frame 1645: the game writes the text inside the box — 24 band
  // lines with about 40 changed letters each. That band still keeps HD.
  {
    Frame text = previous;
    const std::array<s::LineBand, 3> boxes{{{64, 72}, {80, 88}, {96, 104}}};
    for (const s::LineBand &b : boxes)
      for (int y = b.y0; y < b.y1; ++y)
        for (int x = 20; x < 240; x += 11)
          for (int w = 0; w < 4; ++w)
            text.set(x + w, y, 0x0001);
    const s::BandCarry carry =
        s::band_carry(previous.view(), text.view(), boxes);
    check(carry.usable && carry.count > 512,
          "CV-5: a band with the box's text written keeps HD around it");
  }

  // A noisy band would need more runs than the bound: refused, never
  // truncated.
  {
    Frame noisy = previous;
    const std::array<s::LineBand, 1> tall{{{40, 72}}};
    for (int y = 40; y < 72; ++y)
      for (int x = 0; x < 256; x += 2)
        noisy.set(x, y, 0x0001);
    const s::BandCarry carry =
        s::band_carry(previous.view(), noisy.view(), tall);
    check(!carry.usable && carry.count == 0,
          "DI-20: above kMaxCarryRuns the band stays the core's image");
  }

  // Different geometry or format: the previous frame is not comparable.
  {
    Frame narrow(320, 224, 0x1234);
    check(!s::band_carry(previous.view(), narrow.view(), band).usable,
          "DI-20: a geometry change refuses the carry");
    s::CoreImage wide = current.view();
    wide.bytes_per_pixel = 4;
    check(!s::band_carry(previous.view(), wide, band).usable,
          "DI-20: a format change refuses the carry");
    check(!s::band_carry({}, current.view(), band).usable,
          "DI-20: without a previous frame there is nothing to carry");
  }

  // Bands are clamped to the image; an empty band contributes no run.
  {
    const std::array<s::LineBand, 2> edges{{{220, 230}, {10, 10}}};
    const s::BandCarry carry =
        s::band_carry(previous.view(), previous.view(), edges);
    bool in_image = carry.usable && carry.count == 4;
    for (std::uint32_t i = 0; i < carry.count; ++i)
      in_image = in_image && carry.runs[i].y >= 220 && carry.runs[i].y < 224;
    check(in_image, "DI-20: band lines are clamped to the image height");
  }

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
