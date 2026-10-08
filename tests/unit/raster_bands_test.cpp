// Spec 002, DI-17 (RF-10.1, RF-10.3): a frame with raster writes in
// mid-screen is non-composable only on the lines those writes touch. From
// the core's per-line raster journal and the VDP state at the end of the
// frame, session/raster_bands.h finds the bands of lines whose state differs
// from the one the scene is composed with; the rest of the frame keeps HD.
#include "session/raster_bands.h"
#include "session/raster_line_state.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
  mutable std::array<rb::RasterLineLayout, 224> line_layouts{};
  rb::RasterFinalState view() const {
    for (rb::RasterLineLayout &line : line_layouts) {
      line.hscroll_mode = regs[0x0B] & 0x03U;
      line.hscroll_base =
          static_cast<std::uint16_t>((regs[0x0D] & 0x3FU) << 10U);
    }
    return {regs, cram, vsram, vram, 5, 0, line_layouts};
  }
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
    constexpr std::uint64_t kFrameGeneration = 41;
    constexpr std::uint64_t kSnapshotGeneration = 97;
    ayther_region_info_v1 region{};
    region.struct_size = sizeof(region);
    region.region_id = AYTHER_REGION_LINE_REGS;
    region.data_version = AYTHER_LAYOUT_LINE_REGS_V1;
    region.element_size = sizeof(ayther_line_regs_v1);
    region.capacity = 2;
    region.byte_size =
        sizeof(ayther_line_header_v1) + 2 * sizeof(ayther_line_regs_v1);
    region.access_flags = AYTHER_REGION_ACCESS_READ |
                          AYTHER_REGION_FRAME_SCOPED |
                          AYTHER_REGION_NATIVE_ENDIAN;
    region.legacy_memory_id = AYTHER_LEGACY_MEMORY_NONE;
    check(rb::line_state_region_is_compatible(region, 2),
          "RF-3.5/O1: the declared LINE_STATE v1 contract is accepted");

    ayther_region_info_v1 incompatible = region;
    incompatible.struct_size = sizeof(incompatible) - 1;
    check(!rb::line_state_region_is_compatible(incompatible, 2),
          "RF-3.5/O1: a truncated LINE_STATE descriptor is rejected");
    incompatible = region;
    incompatible.region_id = AYTHER_REGION_LINE_CRAM;
    check(!rb::line_state_region_is_compatible(incompatible, 2),
          "RF-3.5/O1: metadata for another region cannot describe LINE_STATE");
    incompatible = region;
    incompatible.data_version = AYTHER_LAYOUT_LINE_REGS_V1 + 1;
    check(!rb::line_state_region_is_compatible(incompatible, 2),
          "RF-3.5/O1: an unknown LINE_STATE data version is rejected");
    incompatible = region;
    incompatible.element_size = sizeof(ayther_line_regs_v1) - 1;
    check(!rb::line_state_region_is_compatible(incompatible, 2),
          "RF-3.5/O1: an incompatible LINE_STATE element size is rejected");
    incompatible = region;
    incompatible.access_flags &= ~AYTHER_REGION_ACCESS_READ;
    check(!rb::line_state_region_is_compatible(incompatible, 2),
          "RF-3.5/O1: LINE_STATE must be readable");
    incompatible = region;
    incompatible.access_flags &= ~AYTHER_REGION_FRAME_SCOPED;
    check(!rb::line_state_region_is_compatible(incompatible, 2),
          "RF-3.5/O1: LINE_STATE must be frame-scoped");
    incompatible = region;
    incompatible.access_flags &= ~AYTHER_REGION_NATIVE_ENDIAN;
    check(!rb::line_state_region_is_compatible(incompatible, 2),
          "RF-3.5/O1: LINE_STATE must expose native-endian records");
    incompatible = region;
    incompatible.access_flags |=
        AYTHER_REGION_ACCESS_CONTROL_WRITE | AYTHER_REGION_WORD_SWAPPED_LE;
    check(!rb::line_state_region_is_compatible(incompatible, 2),
          "RF-3.5/O1: LINE_STATE rejects writable or word-swapped metadata");
    incompatible = region;
    incompatible.capacity = 1;
    check(!rb::line_state_region_is_compatible(incompatible, 2),
          "RF-3.5/O1: LINE_STATE capacity must cover the visible lines");
    incompatible = region;
    --incompatible.byte_size;
    check(!rb::line_state_region_is_compatible(incompatible, 2),
          "RF-3.5/O1: LINE_STATE bytes must cover its declared entries");

    ayther_line_header_v1 header{};
    header.struct_size = sizeof(header);
    header.entry_size = sizeof(ayther_line_regs_v1);
    header.lines = 2;
    header.frame_generation = kFrameGeneration;
    std::array<ayther_line_regs_v1, 2> entries{};
    entries[0].reg11 = 3;
    entries[0].hscb = 0xAC00;
    entries[1].reg11 = 0;
    entries[1].hscb = 0xFC00;
    std::vector<std::uint8_t> bytes(sizeof(header) + sizeof(entries));
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), entries.data(), sizeof(entries));
    std::array<rb::RasterLineLayout, 2> layouts{};
    const auto parsed = rb::parse_raster_line_layouts(bytes, kFrameGeneration,
                                                      2, std::span{layouts});
    check(parsed && *parsed == 2 && layouts[0].hscroll_mode == 3 &&
              layouts[0].hscroll_base == 0xAC00 &&
              layouts[1].hscroll_mode == 0 && layouts[1].hscroll_base == 0xFC00,
          "RF-3.5/O1: LINE_STATE is parsed with its declared stride");
    check(!rb::parse_raster_line_layouts(bytes, kSnapshotGeneration, 2,
                                         std::span{layouts}),
          "RF-3.5/O1: snapshot generation is not confused with the frame "
          "generation carried by LINE_STATE");
    header.flags = AYTHER_LINES_OVERFLOW;
    std::memcpy(bytes.data(), &header, sizeof(header));
    check(!rb::parse_raster_line_layouts(bytes, kFrameGeneration, 2,
                                         std::span{layouts}),
          "RF-3.5/O1: incomplete LINE_STATE is rejected");
    header.flags = 0;
    std::memcpy(bytes.data(), &header, sizeof(header));
    check(!rb::parse_raster_line_layouts(
              std::span<const std::uint8_t>{bytes}.first(bytes.size() - 1),
              kFrameGeneration, 2, std::span{layouts}),
          "RF-3.5/O1: truncated LINE_STATE is rejected");
  }

  {
    constexpr std::uint64_t kSnapshotGeneration = 97;
    ayther_journal_v1 journal{};
    journal.layout_version = AYTHER_LAYOUT_JOURNAL_V1;
    journal.struct_size = sizeof(journal);
    check(rb::raster_journal_is_coherent(AYTHER_STATUS_OK, kSnapshotGeneration,
                                         kSnapshotGeneration, journal),
          "RF-3.5/O1: a journal read from the exact snapshot is coherent");
    check(!rb::raster_journal_is_coherent(AYTHER_STATUS_OK,
                                          kSnapshotGeneration + 1,
                                          kSnapshotGeneration, journal),
          "RF-3.5/O1: a journal from another snapshot is rejected");
    check(!rb::raster_journal_is_coherent(AYTHER_STATUS_STALE_GENERATION,
                                          kSnapshotGeneration + 1,
                                          kSnapshotGeneration, journal),
          "RF-3.5/O1: a stale-generation read cannot authorize a journal");
    check(!rb::raster_journal_is_coherent(AYTHER_STATUS_OK,
                                          AYTHER_GENERATION_ANY,
                                          AYTHER_GENERATION_ANY, journal),
          "RF-3.5/O1: generation-any cannot authorize a journal");
    journal.layout_version = AYTHER_LAYOUT_JOURNAL_V1 + 1;
    check(!rb::raster_journal_is_coherent(AYTHER_STATUS_OK, kSnapshotGeneration,
                                          kSnapshotGeneration, journal),
          "RF-3.5/O1: an unknown journal layout is rejected");
    journal.layout_version = AYTHER_LAYOUT_JOURNAL_V1;
    journal.struct_size = offsetof(ayther_journal_v1, events);
    journal.count = 1;
    check(!rb::raster_journal_is_coherent(AYTHER_STATUS_OK, kSnapshotGeneration,
                                          kSnapshotGeneration, journal),
          "RF-3.5/O1: a journal count cannot exceed its declared payload");
    journal.struct_size = sizeof(journal);
    journal.count = AYTHER_JOURNAL_MAX_EVENTS + 1;
    check(!rb::raster_journal_is_coherent(AYTHER_STATUS_OK, kSnapshotGeneration,
                                          kSnapshotGeneration, journal),
          "RF-3.5/O1: a journal count cannot exceed the v1 event array");
  }

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
    check(b.localized && b.count == 1 && band(b, 0, 0, 117),
          "RF-3.5/O1: unknown CRAM state before the first write is banded "
          "conservatively");
  }
  {
    // Changed at line 100 and restored in vblank (not in the journal).  The
    // prior value is not captured, so neither side of the write can be proved
    // equal to the frame-final palette.
    const std::array<Event, 1> ev{Event{100, kCram, 5, 0x0222}};
    const rb::RasterBands b = rb::raster_bands(kCram, ev, 0, s.view(), 224);
    check(b.localized && b.count == 1 && band(b, 0, 0, 224),
          "RF-3.5/O1: an unknown prior CRAM value plus a non-final write "
          "conservatively covers the frame");
  }
  {
    // The core journals CRAM/VSRAM only when a write changes the raw value,
    // but does not include the value before the first write.  All preceding
    // lines therefore remain potentially different from the final state.
    const std::array<Event, 1> cram{Event{60, kCram, 5, 0x0EEE}};
    const rb::RasterBands cram_bands =
        rb::raster_bands(kCram, cram, 0, s.view(), 224);
    check(cram_bands.localized && cram_bands.count == 1 &&
              band(cram_bands, 0, 0, 60),
          "RF-3.5/O1: a first CRAM write to the final value bands prior lines");

    State d = s;
    d.vsram[4] = 0x10;
    d.vsram[5] = 0x01;
    const std::array<Event, 1> vsram{Event{75, kVsram, 4, 0x0110}};
    const rb::RasterBands vsram_bands =
        rb::raster_bands(kVsram, vsram, 0, d.view(), 224);
    check(
        vsram_bands.localized && vsram_bands.count == 1 &&
            band(vsram_bands, 0, 0, 75),
        "RF-3.5/O1: a first VSRAM write to the final value bands prior lines");

    const std::array<Event, 2> vsram_split{Event{20, kVsram, 4, 0x0220},
                                           Event{50, kVsram, 4, 0x0110}};
    const rb::RasterBands vsram_split_bands =
        rb::raster_bands(kVsram, vsram_split, 0, d.view(), 224);
    check(vsram_split_bands.localized && vsram_split_bands.count == 1 &&
              band(vsram_split_bands, 0, 0, 50),
          "RF-3.5/O1: unknown VSRAM state before a split is banded too");

    // The pinned core can journal a Z80 byte write without carrying a width.
    // An even address and an 8-bit payload are still ambiguous: merging the
    // final neighbouring byte would invent historical state.
    const std::array<Event, 1> byte_write{Event{60, kVsram, 4, 0x0010}};
    check(!rb::raster_bands(kVsram, byte_write, 0, d.view(), 224).localized,
          "RF-3.5/O1: an ambiguous VSRAM byte event uses the core frame");
  }
  {
    // Register, VSRAM and hscroll writes with exact line layouts.
    s.regs[0x0B] = 0x00;
    s.regs[0x0D] = 0x3F; // hscroll table at 0xFC00
    s.vsram[4] = 0x10;
    s.vsram[5] = 0x01;
    s.vram[0xFC00] = 0x34;
    s.vram[0xFC01] = 0x12;
    const std::array<Event, 6> ev{
        Event{20, kReg, 0x0B, 0x03},   Event{30, kReg, 0x0B, 0x00},
        Event{150, kVsram, 4, 0x0220}, Event{158, kHscroll, 0xFC00, 0x9999},
        Event{160, kVsram, 4, 0x0110}, Event{165, kHscroll, 0xFC00, 0x1234}};
    const rb::RasterBands b =
        rb::raster_bands(kReg | kVsram | kHscroll, ev, 0, s.view(), 224);
    check(b.localized && b.count == 1 && band(b, 0, 0, 165),
          "RF-3.5/O3: exact line layouts localize combined raster writes");
  }
  {
    // DI-21 (RF-10.3, campaign 2026-10-07): a line the core drew exactly as
    // the frame it recomposes from its final state was drawn with the final
    // state, whatever the unobserved prior value. With an exact
    // recomposition, only the touched lines that differ from it are banded.
    // Toma 3 frame 4756: a first CRAM write at line 202 banded [0, 202), and
    // the whole picture flickered to the originals for a frame.
    const std::array<Event, 1> cram{Event{202, kCram, 5, 0x0EEE}};
    std::vector<std::uint8_t> same(224, 0);
    const rb::RasterBands none =
        rb::raster_bands(kCram, cram, 0, s.view(), 224, same);
    check(none.localized && none.count == 0,
          "DI-21: conservative CRAM lines equal to the recomposition are not "
          "banded (Toma 3 frame 4756)");
    std::vector<std::uint8_t> edge(224, 0);
    for (int y = 196; y < 206; ++y)
      edge[static_cast<std::size_t>(y)] = 1;
    const rb::RasterBands some =
        rb::raster_bands(kCram, cram, 0, s.view(), 224, edge);
    check(some.localized && some.count == 1 && band(some, 0, 196, 202),
          "DI-21: only the touched lines that differ from the recomposition "
          "stay banded");
    check(band(rb::raster_bands(kCram, cram, 0, s.view(), 224), 0, 0, 202),
          "DI-17: without a recomposition the prior lines stay conservative");

    // Toma 3 frame 4757: whole-screen hscroll without exact LINE_STATE kept
    // the complete core frame. The recomposition shows which lines differ.
    State d = s;
    d.regs[0x0B] = 0x00;
    d.regs[0x0D] = 0x2B;
    d.vram[0xAC02] = 0x5F;
    d.vram[0xAC03] = 0xFE;
    rb::RasterFinalState no_line_state = d.view();
    no_line_state.line_layouts = {};
    const std::array<Event, 1> used{Event{89, kHscroll, 0xAC02, 0xFE5F}};
    std::vector<std::uint8_t> shifted(224, 0);
    for (int y = 0; y < 89; ++y)
      shifted[static_cast<std::size_t>(y)] = 1;
    const rb::RasterBands hs =
        rb::raster_bands(kHscroll, used, 0, no_line_state, 224, shifted);
    check(hs.localized && hs.count == 1 && band(hs, 0, 0, 89),
          "DI-21: hscroll without LINE_STATE is localized by the "
          "recomposition (Toma 3 frame 4757)");

    // Toma 3 frame 645: an ambiguous byte-shaped hscroll event kept the
    // complete core frame between two HD frames of the title. With an exact
    // recomposition the event may have touched any line; only the lines that
    // differ stay banded.
    const std::array<Event, 1> byte_hscroll{
        Event{60, kHscroll, 0xAC03, 0x00FE}};
    std::vector<std::uint8_t> title(224, 0);
    for (int y = 100; y < 120; ++y)
      title[static_cast<std::size_t>(y)] = 1;
    const rb::RasterBands amb =
        rb::raster_bands(kHscroll, byte_hscroll, 0, no_line_state, 224, title);
    check(amb.localized && amb.count == 1 && band(amb, 0, 100, 120),
          "DI-21: an ambiguous event is localized by the recomposition "
          "(Toma 3 frame 645)");
    check(!rb::raster_bands(kHscroll, byte_hscroll, 0, no_line_state, 224)
               .localized,
          "DI-17: without a recomposition an ambiguous event keeps the core "
          "frame");
  }
  {
    // O1 residual at Toma 3 frame 4757 (RF-3.5): in whole-screen
    // hscroll mode, the core journals only writes that change a word.  A
    // first write whose data is also the frame-final value therefore proves
    // that the lines above it used another value.  Only the two words read
    // by whole-screen mode (planes A and B) may create that leading band.
    State d = s;
    d.regs[0x0B] = 0x00; // whole-screen hscroll
    d.regs[0x0D] = 0x2B; // table at 0xAC00
    d.vram[0xAC02] = 0x5F;
    d.vram[0xAC03] = 0xFE;
    d.vram[0xAC20] = 0x5F;
    d.vram[0xAC21] = 0xFE;
    const std::array<Event, 1> used{Event{89, kHscroll, 0xAC02, 0xFE5F}};
    const rb::RasterBands b =
        rb::raster_bands(kHscroll, used, 0, d.view(), 224);
    check(b.localized && b.count == 1 && band(b, 0, 0, 89),
          "RF-3.5/O1: a first effective whole-screen hscroll write bands "
          "the lines drawn before it");

    rb::RasterFinalState missing_line_state = d.view();
    missing_line_state.line_layouts = {};
    check(
        !rb::raster_bands(kHscroll, used, 0, missing_line_state, 224).localized,
        "RF-3.5/O1: hscroll without exact per-line layout uses the core "
        "frame");

    // R11 can change and be restored entirely in vblank, so the pre-frame and
    // final registers can agree while visible lines used another layout. The
    // exact line-state region, not either snapshot, selects consumers.
    rb::RasterFinalState vblank_layout = d.view();
    std::array<rb::RasterLineLayout, 224> exact_layouts = d.line_layouts;
    for (int y = 0; y < 50; ++y)
      exact_layouts[static_cast<std::size_t>(y)].hscroll_mode = 3;
    vblank_layout.line_layouts = exact_layouts;
    const rb::RasterBands exact =
        rb::raster_bands(kHscroll, used, 0, vblank_layout, 224);
    check(exact.localized && exact.count == 2 && band(exact, 0, 0, 1) &&
              band(exact, 1, 50, 89),
          "RF-3.5/O1: exact line layouts survive an unjournaled vblank "
          "layout change");

    const std::array<Event, 1> unused{Event{89, kHscroll, 0xAC20, 0xFE5F}};
    check(rb::raster_bands(kHscroll, unused, 0, d.view(), 224).count == 0,
          "RF-3.5/O1: a write outside the whole-screen hscroll words does "
          "not create a false band");

    const std::array<Event, 1> byte_hscroll{
        Event{89, kHscroll, 0xAC02, 0x005F}};
    check(!rb::raster_bands(kHscroll, byte_hscroll, 0, d.view(), 224).localized,
          "RF-3.5/O1: an ambiguous hscroll byte event uses the core frame");

    // The same inference is address-specific in the other VDP modes:
    // mode 1 repeats the first eight entries, mode 2 selects one eight-line
    // cell, and mode 3 selects one line.  High register bits are unrelated.
    d.regs[0x0B] = 0xFDU; // mode 1
    const std::uint16_t mode1_address = 0xAC00 + 3 * 4 + 2;
    d.vram[mode1_address] = 0x5F;
    d.vram[mode1_address + 1] = 0xFE;
    const std::array<Event, 1> mode1{
        Event{20, kHscroll, mode1_address, 0xFE5F}};
    const rb::RasterBands b1 =
        rb::raster_bands(kHscroll, mode1, 0, d.view(), 224);
    check(b1.localized && b1.count == 3 && band(b1, 0, 3, 4) &&
              band(b1, 1, 11, 12) && band(b1, 2, 19, 20),
          "RF-3.5/O1: mode 1 bands only prior lines that repeat its entry");

    d.regs[0x0B] = 0xFEU; // mode 2
    const std::uint16_t mode2_address = 0xAC00 + 16 * 4;
    d.vram[mode2_address] = 0x5F;
    d.vram[mode2_address + 1] = 0xFE;
    const std::array<Event, 1> mode2{
        Event{30, kHscroll, mode2_address, 0xFE5F}};
    const rb::RasterBands b2 =
        rb::raster_bands(kHscroll, mode2, 0, d.view(), 224);
    check(b2.localized && b2.count == 1 && band(b2, 0, 16, 24),
          "RF-3.5/O1: mode 2 bands only the prior eight-line cell");

    d.regs[0x0B] = 0xFFU; // mode 3
    const std::uint16_t mode3_address = 0xAC00 + 17 * 4 + 2;
    d.vram[mode3_address] = 0x5F;
    d.vram[mode3_address + 1] = 0xFE;
    const std::array<Event, 1> mode3{
        Event{30, kHscroll, mode3_address, 0xFE5F}};
    const rb::RasterBands b3 =
        rb::raster_bands(kHscroll, mode3, 0, d.view(), 224);
    check(b3.localized && b3.count == 1 && band(b3, 0, 17, 18),
          "RF-3.5/O1: mode 3 bands only the prior line using its entry");

    // A non-final word also affects only the lines that consume its address,
    // not the complete interval between writes.
    d.regs[0x0B] = 0x01; // mode 1
    d.vram[mode1_address] = 0x34;
    d.vram[mode1_address + 1] = 0x12;
    const std::array<Event, 2> mode1_changed{
        Event{4, kHscroll, mode1_address, 0x9999},
        Event{20, kHscroll, mode1_address, 0x1234}};
    const rb::RasterBands c1 =
        rb::raster_bands(kHscroll, mode1_changed, 0, d.view(), 224);
    check(c1.localized && c1.count == 3 && band(c1, 0, 3, 4) &&
              band(c1, 1, 11, 12) && band(c1, 2, 19, 20),
          "RF-3.5/O1: an unknown prior mode-1 word bands every possible "
          "consumer before its first write");

    d.regs[0x0B] = 0x02; // mode 2
    d.vram[mode2_address] = 0x34;
    d.vram[mode2_address + 1] = 0x12;
    const std::array<Event, 2> mode2_changed{
        Event{10, kHscroll, mode2_address, 0x9999},
        Event{30, kHscroll, mode2_address, 0x1234}};
    const rb::RasterBands c2 =
        rb::raster_bands(kHscroll, mode2_changed, 0, d.view(), 224);
    check(c2.localized && c2.count == 1 && band(c2, 0, 16, 24),
          "RF-3.5/O3: a changed mode-2 word bands only its eight-line cell");

    d.regs[0x0B] = 0x03; // mode 3
    d.vram[mode3_address] = 0x34;
    d.vram[mode3_address + 1] = 0x12;
    const std::array<Event, 2> mode3_changed{
        Event{10, kHscroll, mode3_address, 0x9999},
        Event{30, kHscroll, mode3_address, 0x1234}};
    const rb::RasterBands c3 =
        rb::raster_bands(kHscroll, mode3_changed, 0, d.view(), 224);
    check(c3.localized && c3.count == 1 && band(c3, 0, 17, 18),
          "RF-3.5/O3: a changed mode-3 word bands only its addressed line");

    const std::array<Event, 1> after_consumer{
        Event{18, kHscroll, mode3_address, 0x9999}};
    const rb::RasterBands after =
        rb::raster_bands(kHscroll, after_consumer, 0, d.view(), 224);
    check(after.localized && after.count == 1 && band(after, 0, 17, 18),
          "RF-3.5/O1: a mode-3 write after its consumer cannot prove the "
          "prior value used by that line");

    // If the layout registers themselves change in the active frame, their
    // previous values are not in an hscroll event.  The affected prior lines
    // are ambiguous, so O1 requires the complete core frame.
    d.regs[0x0B] = 0x00;
    const std::array<Event, 3> layout_change{
        Event{20, kReg, 0x0B, 0x03}, Event{30, kReg, 0x0B, 0x00},
        Event{89, kHscroll, 0xAC02, 0xFE5F}};
    const rb::RasterBands stable_only =
        rb::raster_bands(kReg | kHscroll, layout_change, 0, d.view(), 224);
    check(stable_only.localized && stable_only.count == 1 &&
              band(stable_only, 0, 0, 89),
          "RF-3.5/O3: exact line layouts localize a mid-frame hscroll mode "
          "change");

    const std::array<Event, 3> masked_redundant{
        Event{20, kReg, 0x0B, 0xF8}, Event{30, kReg, 0x0D, 0x6B},
        Event{89, kHscroll, 0xAC02, 0xFE5F}};
    const rb::RasterBands masked =
        rb::raster_bands(kReg | kHscroll, masked_redundant, 0, d.view(), 224);
    check(masked.localized && masked.count == 1 && band(masked, 0, 0, 89),
          "RF-3.5/O3: redundant or high-bit-only R11/R13 writes preserve "
          "the effective hscroll layout and do not withdraw HD globally");

    d.vram[0xAC02] = 0x07;
    d.vram[0xAC03] = 0x08;
    const std::array<Event, 2> same_visible_value{
        Event{89, kHscroll, 0xAC02, 0x0407},
        Event{100, kHscroll, 0xAC02, 0x0807}};
    const rb::RasterBands semantic =
        rb::raster_bands(kHscroll, same_visible_value, 0, d.view(), 224);
    check(semantic.localized && semantic.count == 1 && band(semantic, 0, 0, 89),
          "RF-3.5/O1: hscroll equality uses the visible ten bits; a first "
          "raw change to the final visible value bands the prior lines");
  }
  {
    // A DMA into CRAM is journaled like a CPU write.
    const std::array<Event, 2> ev{Event{48, kCram, 5, 0x0222},
                                  Event{60, kCram, 5, 0x0EEE}};
    const rb::RasterBands b =
        rb::raster_bands(kCram | kDma, ev, 0, s.view(), 224);
    check(b.localized && band(b, 0, 0, 60),
          "a DMA into CRAM includes lines drawn with its unknown prior value");
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
    // DI-21: the palette's unknown prior lines that equal the recomposition
    // were drawn with the final state; only the differing lines remain.
    check(u.localized && u.count == 1 && band(u, 0, 48, 117),
          "DI-21: pattern and unknown-prior palette evidence keep only the "
          "lines that differ from the recomposition");
  }
  {
    // O1 residual at Toma 3 frame 647 (RF-10.3): the VDP parsed this sprite
    // before the game rewrote its SAT entry. The final-state recomposition
    // cannot reveal the historical occurrence's scanline lifetime or HD
    // identity, so a visible mismatch must conservatively use the core frame.
    State d = s;
    d.regs[5] = 0x6C;  // SAT at 0xD800
    d.regs[12] = 0x81; // H40: register 5 bit 0 is ignored
    constexpr std::size_t kSlot = 4;
    const std::size_t sat = 0xD800 + kSlot * 8;
    // The final link chain reaches slot 4 at rank 1: slot 0 -> slot 4 -> end.
    d.vram[0xD800 + 2] = 0x04;
    d.vram[0xD800 + 3] = 0x00;
    // The core exposes VRAM word-swapped on little-endian hosts: logical
    // byte `address` lives at `address ^ 1` in this raw buffer.
    d.vram[sat + 0] = 0xE7;
    d.vram[sat + 1] = 0x00; // logical y = 103 + 128
    d.vram[sat + 2] = 0x00;
    // Mode 5 encodes height in bits 8-9 and width in bits 10-11.
    d.vram[sat + 3] = 0x07; // logical size = 2 x 4 tiles
    d.vram[sat + 4] = 0x23;
    d.vram[sat + 5] = 0x01; // logical attr = pattern 0x0123
    d.vram[sat + 6] = 0x48;
    d.vram[sat + 7] = 0x01; // logical x = 200 + 128
    const rb::RasterSprite sprite{static_cast<std::uint8_t>(kSlot),
                                  200,
                                  103,
                                  2,
                                  4,
                                  0x0123,
                                  1,
                                  true,
                                  true};
    std::vector<std::uint8_t> diff(224, 0);
    for (int y = 96; y < 133; ++y)
      diff[static_cast<std::size_t>(y)] = 1;

    const rb::RasterBands same = rb::raster_bands(kVram, {}, 0, d.view(), 224,
                                                  diff, std::span{&sprite, 1U});
    check(same.localized && same.count == 1 && band(same, 0, 96, 133),
          "RF-10.3/O1: a sprite matching its final SAT entry does not widen "
          "the recomposition band");

    rb::RasterFinalState missing_sat_registers = d.view();
    missing_sat_registers.regs = {};
    check(!rb::sprite_matches_final_sat(sprite, missing_sat_registers),
          "RF-10.3/O1: unavailable R5/R12 cannot prove final SAT identity");
    rb::RasterFinalState truncated_sat_vram = d.view();
    truncated_sat_vram.vram =
        std::span<const std::uint8_t>(d.vram.data(), sat + 4U);
    check(!rb::sprite_matches_final_sat(sprite, truncated_sat_vram),
          "RF-10.3/O1: a truncated final SAT cannot authorize a local band");

    // The target slot can fit while an earlier link jumps beyond the
    // published VRAM span. Every traversed link must be bounds-checked, not
    // only the target entry (a sanitizer build used to catch this as OOB).
    std::array<std::uint8_t, 16> truncated_chain_vram{};
    truncated_chain_vram[2] = 79; // logical link word at bus address 2 = 0x004f
    std::array<std::uint8_t, 0x20> truncated_chain_regs = s.regs;
    truncated_chain_regs[5] = 0;
    truncated_chain_regs[12] = 0x81;
    rb::RasterFinalState truncated_chain = d.view();
    truncated_chain.regs = truncated_chain_regs;
    truncated_chain.vram = truncated_chain_vram;
    rb::RasterSprite target_after_bad_link = sprite;
    target_after_bad_link.slot = 1;
    target_after_bad_link.chain = 1;
    check(!rb::sprite_matches_final_sat(target_after_bad_link, truncated_chain),
          "RF-10.3/O1: a truncated intermediate SAT link is rejected safely");

    rb::RasterFinalState mode4 = d.view();
    mode4.vdp_mode = 4;
    check(!rb::sprite_matches_final_sat(sprite, mode4),
          "RF-10.3/O1: Mode 4 never uses the Mode 5 SAT matcher");

    ayther_system_v1 pending_system{};
    pending_system.vdp_mode = 5;
    pending_system.flags = AYTHER_SYSTEM_GEOMETRY_PENDING;
    check(!rb::raster_state_matches_emitted_frame(pending_system),
          "RF-3.5/O1: pending SYSTEM geometry rejects all raster "
          "localization based on next-frame VDP_REGS");
    rb::RasterFinalState geometry_pending = d.view();
    geometry_pending.vdp_mode = rb::raster_identity_vdp_mode(pending_system);
    check(!rb::sprite_matches_final_sat(sprite, geometry_pending),
          "RF-10.3/O1: pending H32/H40 geometry cannot authorize SAT "
          "identity from the next frame's R12");
    pending_system.flags = 0;
    check(rb::raster_state_matches_emitted_frame(pending_system),
          "RF-3.5/O3: current SYSTEM geometry permits raster evidence");
    check(rb::raster_identity_vdp_mode(pending_system) == 5,
          "RF-10.3/O3: current SYSTEM geometry preserves the SAT VDP mode");

    rb::RasterFinalState interlace2 = d.view();
    interlace2.interlace = 2;
    check(!rb::sprite_matches_final_sat(sprite, interlace2),
          "RF-10.3/O1: interlace mode 2 never uses the normal Mode 5 SAT "
          "coordinate matcher");
    const rb::RasterBands interlace2_bands = rb::raster_bands(
        kVram, {}, 0, interlace2, 224, diff, std::span{&sprite, 1U});
    check(!interlace2_bands.localized,
          "RF-10.3/O1: an interlace-2 raster sprite uses the complete core "
          "frame until its ten-bit coordinates are modelled");

    rb::RasterSprite reordered = sprite;
    reordered.chain = 2;
    check(!rb::sprite_matches_final_sat(reordered, d.view()),
          "RF-10.3/O1: a sprite whose final SAT chain rank changed is stale "
          "even when geometry and visual attributes are unchanged");

    // Residual bytes in a SAT slot do not make it a final occurrence when
    // the frame-final link chain cannot reach that slot. This remains true
    // even when the cumulative occurrence lacks a known rank/attribute.
    constexpr std::size_t kUnreachableSlot = 5;
    const std::size_t unreachable_sat = 0xD800 + kUnreachableSlot * 8;
    std::copy_n(d.vram.begin() + static_cast<std::ptrdiff_t>(sat), 8,
                d.vram.begin() + static_cast<std::ptrdiff_t>(unreachable_sat));
    rb::RasterSprite unreachable = sprite;
    unreachable.slot = static_cast<std::uint8_t>(kUnreachableSlot);
    unreachable.chain = 0xFF;
    unreachable.identity_known = false;
    check(!rb::sprite_matches_final_sat(unreachable, d.view()),
          "RF-10.3/O1: residual bytes in a disconnected SAT slot are not a "
          "final occurrence");

    rb::RasterSprite h32_out_of_range = sprite;
    h32_out_of_range.slot = 64;
    h32_out_of_range.chain = 0xFF;
    h32_out_of_range.identity_known = false;
    d.regs[12] = 0x80; // H32: only slots 0-63 are reachable.
    check(!rb::sprite_matches_final_sat(h32_out_of_range, d.view()),
          "RF-10.3/O1: H32 rejects SAT slots outside its 64-slot domain");
    d.regs[12] = 0x81;

    // A register-only raster reason can still change which SAT produced the
    // cumulative occurrence list. If R5 selected another base above the
    // write, a visible occurrence that is not in the final SAT has no known
    // scanline lifetime and must use the complete core frame.
    rb::RasterSprite inherited = sprite;
    inherited.y = 16;
    const std::array<Event, 1> sat_base_change{Event{100, kReg, 5, d.regs[5]}};
    const rb::RasterBands register_only_base = rb::raster_bands(
        kReg, sat_base_change, 0, d.view(), 224, {}, std::span{&inherited, 1U});
    check(!register_only_base.localized,
          "RF-10.3/O1: an R5 SAT-base change validates inherited scene "
          "sprites even without a VRAM raster reason");

    // R5 bit 0 is ignored in H40, but the first active write does not reveal
    // which effective SAT vblank selected for the prefix. A cumulative stale
    // occurrence therefore remains unsafe even if the event itself is final.
    const rb::RasterBands h40_ignored_base_bit = rb::raster_bands(
        kReg, sat_base_change, 0, d.view(), 224, {}, std::span{&inherited, 1U});
    check(!h40_ignored_base_bit.localized,
          "RF-10.3/O1: an active R5 write cannot prove the vblank SAT base");

    // The same R5 bit is part of the H32 base, and changing R12 between H40
    // and H32 also changes the reachable slot domain. Both are identity
    // changes even when the core reports only a register raster reason.
    d.regs[12] = 0x80;
    const rb::RasterBands h32_base_bit = rb::raster_bands(
        kReg, sat_base_change, 0, d.view(), 224, {}, std::span{&inherited, 1U});
    check(!h32_base_bit.localized,
          "RF-10.3/O1: H32 treats R5 bit 0 as part of the SAT base");

    rb::RasterSprite inherited_h40 = sprite;
    inherited_h40.slot = 64;
    inherited_h40.chain = 0;
    inherited_h40.identity_known = true;
    const std::array<Event, 1> width_mode_change{
        Event{120, kReg, 12, d.regs[12]}};
    const rb::RasterBands register_only_width =
        rb::raster_bands(kReg, width_mode_change, 0, d.view(), 224, {},
                         std::span{&inherited_h40, 1U});
    check(!register_only_width.localized,
          "RF-10.3/O1: an R12 H40-to-H32 change rejects inherited slots "
          "outside the final 64-slot domain");
    d.regs[12] = 0x81;

    // Flip, palette and priority are part of the SAT sprite's visual state.
    // A mid-frame rewrite of any one of them is stale even when geometry and
    // pattern remain unchanged.
    for (const std::uint8_t final_attr_high : {0x09U, 0x21U, 0x81U}) {
      d.vram[sat + 5] = final_attr_high;
      const rb::RasterBands stale_attr = rb::raster_bands(
          kVram, {}, 0, d.view(), 224, diff, std::span{&sprite, 1U});
      check(!stale_attr.localized,
            "RF-10.3/O1: a rewritten flip, palette or priority makes the "
            "sprite's scanline lifetime ambiguous and uses the core frame");
    }
    d.vram[sat + 5] = 0x01;

    // The SAT X coordinate uses only nine bits.  Bit 9 in the raw mirror must
    // not make an otherwise matching sprite look stale.
    d.vram[sat + 7] = 0x03;
    const rb::RasterBands ignored_coordinate_bit = rb::raster_bands(
        kVram, {}, 0, d.view(), 224, diff, std::span{&sprite, 1U});
    check(ignored_coordinate_bit.localized &&
              ignored_coordinate_bit.count == 1 &&
              band(ignored_coordinate_bit, 0, 96, 133),
          "RF-10.3/O1: SAT X ignores bits outside the VDP's nine-bit field");

    d.vram[sat + 7] = 0x01;
    d.vram[sat + 1] = 0x02;
    check(rb::sprite_matches_final_sat(sprite, d.view()),
          "RF-10.3/O3: SAT Y ignores bits outside the VDP's nine-bit field");
    d.vram[sat + 1] = 0x00;

    // Frame 647 rewrites slot 4 from y=103 to y=16.  The historical sprite
    // still reaches line 135 even though final-state recomposition does not.
    d.vram[sat + 0] = 0x90;
    d.vram[sat + 1] = 0x00; // logical final y = 16 + 128
    d.vram[sat + 7] = 0x01;
    const rb::RasterBands stale = rb::raster_bands(
        kVram, {}, 0, d.view(), 224, diff, std::span{&sprite, 1U});
    check(!stale.localized,
          "RF-10.3/O1: frame 647's historical sprite uses the complete core "
          "frame because the fork does not expose its scanline lifetime");

    // scene_inventory may know the observed geometry while lacking the raw
    // SAT copy that carries the complete attribute/rank. Geometry alone cannot
    // prove HD identity, so unknown must conservatively use the core frame.
    d.vram[sat + 0] = 0xE7;
    const rb::RasterSprite unknown_pattern{
        static_cast<std::uint8_t>(kSlot), 200, 103, 2, 4, 0, 1, false};
    const rb::RasterBands unknown = rb::raster_bands(
        kVram, {}, 0, d.view(), 224, diff, std::span{&unknown_pattern, 1U});
    check(!unknown.localized,
          "RF-10.3/O1: unavailable SAT identity cannot authorize a guessed "
          "local band");

    // A final-SAT mismatch in a visible scene occurrence is ambiguous even if
    // the recomposition difference is elsewhere. RGB equality cannot prove
    // the lifetime or HD identity of that occurrence, so no guessed band is
    // permitted.
    std::vector<std::uint8_t> upper_diff(224, 0);
    for (int y = 96; y < 120; ++y)
      upper_diff[static_cast<std::size_t>(y)] = 1;
    d.vram[sat + 0] = 0x90; // stale again: logical final y = 16 + 128
    rb::RasterSprite upper_sprite = sprite;
    const rb::RasterBands upper_only = rb::raster_bands(
        kVram, {}, 0, d.view(), 224, upper_diff, std::span{&upper_sprite, 1U});
    check(!upper_only.localized,
          "RF-10.3/O1: a stale visible occurrence never uses spatial pixel "
          "coincidence to guess a narrower lifetime");

    const std::vector<std::uint8_t> identical(224, 0);
    rb::RasterSprite invisible_sprite = sprite;
    invisible_sprite.visible = false;
    const rb::RasterBands invisible_stale =
        rb::raster_bands(kVram, {}, 0, d.view(), 224, identical,
                         std::span{&invisible_sprite, 1U});
    check(invisible_stale.localized && invisible_stale.count == 0,
          "RF-10.3/O3: an invisible stale SAT entry does not create a "
          "fallback band");

    // A difference elsewhere cannot be attributed to this sprite and cannot
    // prove its scanline lifetime. It therefore cannot authorize a local band.
    std::vector<std::uint8_t> unrelated_diff(224, 0);
    unrelated_diff[127] = 1;
    rb::RasterSprite unrelated_sprite = sprite;
    const rb::RasterBands unrelated =
        rb::raster_bands(kVram, {}, 0, d.view(), 224, unrelated_diff,
                         std::span{&unrelated_sprite, 1U});
    check(!unrelated.localized,
          "RF-10.3/O1: an unrelated difference inside a stale sprite rect "
          "cannot be misattributed as evidence of its lifetime");

    const rb::RasterSprite hidden_sprite{static_cast<std::uint8_t>(kSlot),
                                         200,
                                         103,
                                         2,
                                         4,
                                         0x0123,
                                         1,
                                         true,
                                         false};
    const rb::RasterBands hidden = rb::raster_bands(
        kVram, {}, 0, d.view(), 224, diff, std::span{&hidden_sprite, 1U});
    check(hidden.localized && hidden.count == 1 && band(hidden, 0, 96, 133),
          "RF-10.3/O3: a hidden stale scene sprite does not widen a band");

    std::vector<std::uint8_t> transparent_top_diff(224, 0);
    for (int y = 111; y < 133; ++y)
      transparent_top_diff[static_cast<std::size_t>(y)] = 1;
    const rb::RasterBands transparent_top =
        rb::raster_bands(kVram, {}, 0, d.view(), 224, transparent_top_diff,
                         std::span{&sprite, 1U});
    check(!transparent_top.localized,
          "RF-10.3/O1: transparency does not turn RGB coincidence into "
          "scanline-lifetime evidence");
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
  {
    // A one-shot write (Toma 3, frame 2): the game turns the display off at
    // line 100 and leaves it off. The active journal cannot identify the
    // value used by the preceding lines, so the prefix is conservative.
    State d = s;
    d.regs[1] = 0x34; // display off at the end of the frame
    rb::RasterFinalState view = d.view();
    const std::array<Event, 1> ev{Event{100, kReg, 1, 0x34}};
    const rb::RasterBands b = rb::raster_bands(kReg, ev, 0, view, 224);
    std::printf("  display off: localized=%d count=%u [%d,%d)\n", b.localized,
                b.count, b.count ? b.bands[0].y0 : -1,
                b.count ? b.bands[0].y1 : -1);
    check(b.localized && b.count == 1 && band(b, 0, 0, 100),
          "A: a one-shot register write touches the lines drawn before it");
  }
  {
    // A one-shot plane base change at line 120 that stays: lines 0-119 show
    // the old plane.
    State d = s;
    d.regs[2] = 0x30;
    rb::RasterFinalState view = d.view();
    const std::array<Event, 1> ev{Event{120, kReg, 2, 0x30}};
    const rb::RasterBands b = rb::raster_bands(kReg, ev, 0, view, 224);
    check(b.localized && b.count == 1 && band(b, 0, 0, 120),
          "A: a plane base moved once mid-screen bands the lines above it");
    // A split re-armed every frame: the pre-frame snapshot still cannot prove
    // that vblank left the same value at visible line zero.  The journal only
    // contains active writes, so the prefix remains conservative.
    const std::array<Event, 2> split{Event{40, kReg, 2, 0x20},
                                     Event{80, kReg, 2, 0x30}};
    const rb::RasterBands r = rb::raster_bands(kReg, split, 0, view, 224);
    check(r.localized && r.count == 1 && band(r, 0, 0, 80),
          "RF-3.5/O1: a register split includes its unobserved vblank prefix");
  }
  {
    // The display turned on mid-frame (it was off): the lines above the
    // write show the backdrop only.
    State d = s;
    d.regs[1] = 0x74;
    rb::RasterFinalState view = d.view();
    const std::array<Event, 1> ev{Event{60, kReg, 1, 0x74}};
    const rb::RasterBands b = rb::raster_bands(kReg, ev, 0, view, 224);
    check(b.localized && b.count == 1 && band(b, 0, 0, 60),
          "A: the lines drawn before the display turns on are a band");
  }
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
