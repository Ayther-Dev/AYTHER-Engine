// Spec 002, DI-17 (RF-10.1, RF-10.3): the line bands of a frame with raster
// writes in mid-screen. The scene is composed with the VDP state at the end
// of the frame; a line is drawn with another state only when a journaled
// write left a register, a CRAM entry, a VSRAM word or an hscroll word with a
// value other than the final one while that line was drawn. Those lines are
// the bands; the rest of the frame composes as usual. Pure: the session feeds
// it the core's raster journal, final VDP memories and exact per-line hscroll
// layouts. Unobserved prefixes are never inferred from a previous frame.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ayther::session {

/// AYTHER_RASTER_REASON_* bits of the core (`fallback_reasons`).
inline constexpr std::uint32_t kRasterReasonReg = 1U << 0;
inline constexpr std::uint32_t kRasterReasonCram = 1U << 1;
inline constexpr std::uint32_t kRasterReasonVsram = 1U << 2;
inline constexpr std::uint32_t kRasterReasonHscroll = 1U << 3;
inline constexpr std::uint32_t kRasterReasonDma = 1U << 4;
inline constexpr std::uint32_t kRasterReasonVram = 1U << 6;
inline constexpr std::uint32_t kRasterReasonJournalOverflow = 1U << 7;

inline constexpr std::uint32_t kMaxRasterBands = 16;

/// One journaled write: `line` is the first display line drawn after it.
struct RasterEvent {
  std::uint16_t line = 0;
  std::uint16_t reason = 0;
  std::uint16_t address = 0;
  std::uint16_t data = 0;
};

/// Exact horizontal-scroll layout consumed by one visible scanline, copied
/// from AYTHER_REGION_LINE_REGS of the same frame generation.
struct RasterLineLayout {
  std::uint8_t hscroll_mode = 0;
  std::uint16_t hscroll_base = 0;
};

/// The VDP memories at the end of the frame, in the core's layout.
struct RasterFinalState {
  std::span<const std::uint8_t> regs;
  std::span<const std::uint8_t> cram;
  std::span<const std::uint8_t> vsram;
  std::span<const std::uint8_t> vram;
  /// VDP mode reported by the core (5 = Mega Drive Mode 5). The SAT layout
  /// below is Mode-5-specific; unknown and Mode 4 must not use it.
  std::uint8_t vdp_mode = 5;
  /// 0 = progressive, 1 = interlace mode 1, 2 = interlace mode 2. Mode 2
  /// uses ten-bit SAT Y coordinates and needs a separate identity matcher.
  std::uint8_t interlace = 0;
  std::span<const RasterLineLayout> line_layouts;
};

/// A sprite the VDP parsed while drawing the frame.  Mid-frame SAT rewrites
/// can leave this historical occurrence different from the same slot in the
/// frame-final VRAM used by the scene recomposition.
struct RasterSprite {
  std::uint8_t slot = 0xFF;
  std::int16_t x = 0;
  std::int16_t y = 0;
  std::uint8_t w_tiles = 1;
  std::uint8_t h_tiles = 1;
  /// Complete visual SAT attribute: pattern, flips, palette and priority.
  std::uint16_t attr = 0;
  /// Position in the parsed SAT link chain (0 = slot drawn first).
  std::uint8_t chain = 0xFF;
  /// True only when the complete parsed-SAT identity (attribute and chain
  /// rank) was joined to this occurrence. Geometry alone is insufficient.
  bool identity_known = false;
  bool visible = true;
};

struct RasterBand {
  std::uint16_t y0 = 0; ///< first line
  std::uint16_t y1 = 0; ///< one past the last line
};

struct RasterBands {
  /// False when the writes cannot be placed on lines (a reason the journal
  /// does not record, a dropped event, no journal): the whole frame is
  /// non-composable.
  bool localized = false;
  std::uint32_t count = 0;
  std::array<RasterBand, kMaxRasterBands> bands{};
};

namespace raster_bands_detail {
inline bool read16(std::span<const std::uint8_t> m, std::size_t o,
                   std::uint16_t &out) noexcept {
  if (o + 1 >= m.size())
    return false;
  // The core stores CRAM, VSRAM and VRAM words as host 16-bit values (a
  // little-endian host); its raster replay writes them the same way.
  out = static_cast<std::uint16_t>(m[o] | (m[o + 1] << 8));
  return true;
}

/// The pinned fork has legacy Z80/DMA paths that journal a single VSRAM or
/// VRAM byte in the word-shaped event ABI. With no width bit, an aligned
/// payload whose high byte is zero is ambiguous too: it can be a real word or
/// one byte of a word. Localizing either case by inventing the neighbour would
/// violate O1, so only an aligned payload that necessarily contains two bytes
/// is accepted until the producer normalizes these events.
inline bool has_unambiguous_word_payload(const RasterEvent &event) noexcept {
  if (event.reason != kRasterReasonVsram &&
      event.reason != kRasterReasonHscroll)
    return true;
  return (event.address & 1U) == 0 && event.data > 0x00FFU;
}

/// The value the frame ends with at the event's address, as the event's
/// `data` is written.
inline bool final_value(const RasterEvent &e, const RasterFinalState &s,
                        std::uint16_t &out) noexcept {
  switch (e.reason) {
  case kRasterReasonReg:
    if (e.address >= s.regs.size())
      return false;
    out = s.regs[e.address];
    return true;
  case kRasterReasonCram:
    return read16(s.cram, static_cast<std::size_t>(e.address & 0x3F) * 2, out);
  case kRasterReasonVsram:
    return read16(s.vsram, e.address & 0x7E, out);
  case kRasterReasonHscroll:
    return read16(s.vram, e.address & 0xFFFE, out);
  default:
    return false;
  }
}

/// True when `address` is one of the two horizontal-scroll words consumed on
/// `line` in the exact layout captured by the core while drawing that line.
/// The VDP modes use one entry for the screen, repeat the first eight, select
/// an eight-line cell, or select a line respectively.
inline bool hscroll_word_on_line(std::uint16_t address,
                                 const RasterFinalState &s, int line) noexcept {
  if (line < 0 || static_cast<std::size_t>(line) >= s.line_layouts.size())
    return false;
  constexpr std::array<std::uint16_t, 4> kLineMasks{0x00U, 0x07U, 0xF8U, 0xFFU};
  const RasterLineLayout &layout =
      s.line_layouts[static_cast<std::size_t>(line)];
  const std::uint16_t line_mask = kLineMasks[layout.hscroll_mode & 0x03U];
  const std::uint16_t base = layout.hscroll_base;
  const std::uint16_t entry = static_cast<std::uint16_t>(
      base + ((static_cast<std::uint16_t>(line) & line_mask) << 2U));
  const std::uint16_t word = static_cast<std::uint16_t>(address & 0xFFFEU);
  return word == entry || word == static_cast<std::uint16_t>(entry + 2U);
}

inline bool sprite_matches_final_sat_impl(const RasterSprite &sprite,
                                          const RasterFinalState &s) noexcept {
  constexpr std::size_t kSatBaseRegister = 5;
  constexpr std::size_t kModeRegister = 12;
  constexpr std::size_t kSatEntryBytes = 8;
  if (s.vdp_mode != 5 || s.interlace == 2 || !sprite.identity_known ||
      sprite.chain == 0xFF)
    return false;
  if (s.regs.size() <= kModeRegister)
    return false;
  const std::size_t max_sprites =
      (s.regs[kModeRegister] & 0x01U) != 0 ? 80U : 64U;
  if (sprite.slot >= max_sprites)
    return false;
  const std::uint8_t base_mask =
      (s.regs[kModeRegister] & 0x01U) != 0 ? 0x7EU : 0x7FU;
  const std::size_t base =
      static_cast<std::size_t>(s.regs[kSatBaseRegister] & base_mask) << 9U;
  const std::size_t offset = base + sprite.slot * kSatEntryBytes;
  if (offset + kSatEntryBytes > s.vram.size())
    return false;
  // The core exposes VRAM as host words on little-endian Windows: logical
  // bus byte `address` is stored at `address ^ 1` in this byte span.
  const auto bus_byte = [&](std::size_t address) {
    return s.vram[address ^ 1U];
  };
  const auto bus_word = [&](std::size_t address) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bus_byte(address)) << 8U) |
        bus_byte(address + 1U));
  };
  std::uint8_t final_chain = 0xFF;
  std::array<bool, 80> seen{};
  std::size_t slot = 0;
  for (std::size_t rank = 0; rank < max_sprites; ++rank) {
    if (slot >= max_sprites || seen[slot])
      break;
    seen[slot] = true;
    if (slot == sprite.slot) {
      final_chain = static_cast<std::uint8_t>(rank);
      break;
    }
    const std::size_t link_offset = base + slot * kSatEntryBytes + 2U;
    if (link_offset + 2U > s.vram.size())
      return false;
    const std::uint16_t link_word = bus_word(link_offset);
    const std::size_t next = link_word & 0x7FU;
    if (next == 0)
      break;
    slot = next;
  }
  if (final_chain == 0xFF)
    return false;
  if (sprite.chain != 0xFF && final_chain != sprite.chain)
    return false;
  const std::int16_t y = static_cast<std::int16_t>(
      static_cast<int>(bus_word(offset) & 0x01FFU) - 128);
  const std::uint16_t size_link = bus_word(offset + 2U);
  const std::uint8_t w_tiles =
      static_cast<std::uint8_t>(((size_link >> 10U) & 0x03U) + 1U);
  const std::uint8_t h_tiles =
      static_cast<std::uint8_t>(((size_link >> 8U) & 0x03U) + 1U);
  const std::uint16_t attr = bus_word(offset + 4U);
  const std::int16_t x = static_cast<std::int16_t>(
      static_cast<int>(bus_word(offset + 6U) & 0x01FFU) - 128);
  return x == sprite.x && y == sprite.y && w_tiles == sprite.w_tiles &&
         h_tiles == sprite.h_tiles && attr == sprite.attr;
}

struct SatLayout {
  std::size_t base = 0;
  std::size_t max_sprites = 0;
};

inline constexpr SatLayout sat_layout(std::uint8_t register5,
                                      std::uint8_t register12) noexcept {
  const bool h40 = (register12 & 0x01U) != 0;
  const std::uint8_t base_mask = h40 ? 0x7EU : 0x7FU;
  return {static_cast<std::size_t>(register5 & base_mask) << 9U,
          h40 ? 80U : 64U};
}

inline constexpr bool same_sat_layout(const SatLayout &left,
                                      const SatLayout &right) noexcept {
  return left.base == right.base && left.max_sprites == right.max_sprites;
}

/// True when a visible interval used another effective SAT base or slot
/// domain. Register 5 bit 0 is ignored in H40 but significant in H32;
/// register 12 bit 0 switches the H32/H40 domain. Other raw bits do not alter
/// this identity lookup.
inline bool sat_layout_changed(std::span<const RasterEvent> events,
                               const RasterFinalState &state,
                               int height) noexcept {
  constexpr std::size_t kSatBaseRegister = 5;
  constexpr std::size_t kModeRegister = 12;
  if (height <= 0 || state.regs.size() <= kModeRegister)
    return false;

  bool writes_base = false;
  bool writes_mode = false;
  for (const RasterEvent &event : events)
    if (event.reason == kRasterReasonReg) {
      writes_base = writes_base || event.address == kSatBaseRegister;
      writes_mode = writes_mode || event.address == kModeRegister;
    }
  if (!writes_base && !writes_mode)
    return false;

  const std::uint8_t final_base = state.regs[kSatBaseRegister];
  const std::uint8_t final_mode = state.regs[kModeRegister];
  const SatLayout final = sat_layout(final_base, final_mode);
  std::uint8_t current_base = final_base;
  std::uint8_t current_mode = final_mode;
  bool base_known = !writes_base;
  bool mode_known = !writes_mode;
  int line = 0;
  for (const RasterEvent &event : events) {
    if (event.reason != kRasterReasonReg ||
        (event.address != kSatBaseRegister && event.address != kModeRegister))
      continue;
    const int boundary = (std::clamp)(static_cast<int>(event.line), 0, height);
    if (boundary < line)
      return true;
    if (boundary > line &&
        (!base_known || !mode_known ||
         !same_sat_layout(sat_layout(current_base, current_mode), final)))
      return true;
    line = boundary;
    if (event.address == kSatBaseRegister) {
      current_base = static_cast<std::uint8_t>(event.data);
      base_known = true;
    } else {
      current_mode = static_cast<std::uint8_t>(event.data);
      mode_known = true;
    }
  }
  return line < height &&
         (!base_known || !mode_known ||
          !same_sat_layout(sat_layout(current_base, current_mode), final));
}

/// Values as observed by the VDP for the fields whose storage contains
/// ignored bits. Register 11 bit 2 is significant (column V-scroll), while
/// bits 3-7 are reserved; register 13 uses only its low six bits. Horizontal
/// scroll is a signed ten-bit value.
inline std::uint16_t
effective_event_value(const RasterEvent &event, std::uint16_t value,
                      const RasterFinalState &state) noexcept {
  if (event.reason == kRasterReasonHscroll)
    return value & 0x03FFU;
  if (event.reason == kRasterReasonReg && event.address == 0x05U &&
      state.regs.size() > 0x0CU) {
    const std::uint16_t mask = (state.regs[0x0CU] & 0x01U) != 0 ? 0x7EU : 0x7FU;
    return value & mask;
  }
  if (event.reason == kRasterReasonReg && event.address == 0x0BU)
    return value & 0x07U;
  if (event.reason == kRasterReasonReg && event.address == 0x0DU)
    return value & 0x3FU;
  return value;
}
} // namespace raster_bands_detail

/// Whether a parsed sprite is identical to the same slot in the final SAT,
/// including its global link-chain rank. An unreachable H32/H40 slot is not a
/// final occurrence.
inline bool sprite_matches_final_sat(const RasterSprite &sprite,
                                     const RasterFinalState &state) noexcept {
  return raster_bands_detail::sprite_matches_final_sat_impl(sprite, state);
}

/// The bands of a frame whose fallback reasons are `reasons`, from its
/// journal (`events` in the order the core recorded them, `dropped` events
/// that did not fit) and the memories it ends with. The journal omits vblank
/// writes and never carries the value before an address's first active write,
/// so every prior register/CRAM/VSRAM line is touched conservatively. Hscroll
/// consumers additionally require exact same-generation LINE_STATE; no
/// unobserved state is assumed equal to the final state.
///
/// Pattern writes (VRAM) are not journaled. For them `recomposed_diff` flags
/// (non-zero) the lines where the core's image differs from the frame the
/// core recomposes from its final state: those lines were drawn with other
/// patterns. Without it a VRAM write keeps the whole frame non-composable.
/// DI-21: with it, every touched line equal to the recomposition is dropped
/// (it was drawn with the final state), and hscroll without exact LINE_STATE
/// is localized by it instead of keeping the whole frame.
[[nodiscard]] inline RasterBands
raster_bands(std::uint32_t reasons, std::span<const RasterEvent> events,
             std::uint32_t dropped, const RasterFinalState &state, int height,
             std::span<const std::uint8_t> recomposed_diff = {},
             std::span<const RasterSprite> sprites = {}) noexcept {
  using raster_bands_detail::final_value;
  RasterBands out;
  if (reasons == 0) {
    out.localized = true;
    return out;
  }
  constexpr std::uint32_t kJournaled = kRasterReasonReg | kRasterReasonCram |
                                       kRasterReasonVsram |
                                       kRasterReasonHscroll;
  constexpr std::uint32_t kLocalizable =
      kJournaled | kRasterReasonDma | kRasterReasonVram;
  const bool patterns = (reasons & kRasterReasonVram) != 0;
  // DI-21: an exact recomposition proves, line by line, which lines the
  // final state describes.
  const bool recomposed =
      height > 0 && recomposed_diff.size() >= static_cast<std::size_t>(height);
  if ((reasons & ~kLocalizable) != 0 || dropped != 0 || height <= 0 ||
      height > 1024 || ((reasons & kJournaled) != 0 && events.empty()) ||
      (patterns && !recomposed))
    return out;
  const bool exact_layouts =
      state.line_layouts.size() >= static_cast<std::size_t>(height);
  if ((reasons & kRasterReasonHscroll) != 0 && !exact_layouts && !recomposed)
    return out;
  // Without exact LINE_STATE every line may consume an hscroll word; the
  // recomposition then keeps only the lines that differ.
  const auto reads_hscroll = [&](std::uint16_t address, int y) {
    return !exact_layouts ||
           raster_bands_detail::hscroll_word_on_line(address, state, y);
  };
  // DI-21: with an exact recomposition an event whose value cannot be read
  // may have touched any line; the recomposition keeps the ones that differ.
  bool any_line = false;
  for (const RasterEvent &event : events)
    if (!raster_bands_detail::has_unambiguous_word_payload(event)) {
      if (!recomposed)
        return out;
      any_line = true;
    }
  // Sprite identity below is defined only for the normal Mega Drive Mode-5
  // SAT. Mode 4 has another layout and interlace mode 2 uses ten-bit Y
  // coordinates. Until those identities are modelled, a visible sprite in a
  // raster frame cannot authorize HD reconstruction from final-state VRAM.
  if (state.vdp_mode != 5 || state.interlace == 2)
    for (const RasterSprite &sprite : sprites)
      if (sprite.visible)
        return out;
  // The cumulative parsed-sprite list can contain an occurrence observed
  // before a mid-frame SAT rewrite or before a register selected another SAT
  // base/domain. The fork does not expose that occurrence's scanline lifetime.
  // RGB equality cannot prove its HD identity, so a visible occurrence that
  // differs from the final SAT makes the complete frame non-composable instead
  // of guessing a spatial band.
  const bool sat_identity_changed =
      raster_bands_detail::sat_layout_changed(events, state, height);
  if (patterns || sat_identity_changed)
    for (const RasterSprite &sprite : sprites)
      if (sprite.visible && !sprite_matches_final_sat(sprite, state))
        return out;
  std::array<bool, 1024> touched{};
  if (patterns)
    for (int y = 0; y < height; ++y)
      touched[static_cast<std::size_t>(y)] =
          recomposed_diff[static_cast<std::size_t>(y)] != 0;
  for (std::size_t i = 0; i < events.size(); ++i) {
    const RasterEvent &e = events[i];
    std::uint16_t final = 0;
    if (!raster_bands_detail::has_unambiguous_word_payload(e))
      continue;
    if (!final_value(e, state, final)) {
      if (!recomposed)
        return out;
      any_line = true;
      continue;
    }
    bool first = true;
    for (std::size_t j = 0; j < i && first; ++j)
      first = events[j].reason != e.reason || events[j].address != e.address;
    // Only active register writes enter the journal. The snapshot was taken
    // before the emulated frame and cannot reveal a write made in vblank, so
    // even an available previous value does not prove the state at line zero.
    if (first && e.reason == kRasterReasonReg) {
      for (int y = 0; y < e.line && y < height; ++y)
        touched[static_cast<std::size_t>(y)] = true;
    }
    // The journal carries only the value after each CRAM, VSRAM or hscroll
    // write. The unobserved value before the first write may differ from the
    // final state regardless of the new value, so prior consumer lines are
    // touched conservatively (including Toma 3 frame 4757).
    const bool equals_final =
        raster_bands_detail::effective_event_value(e, e.data, state) ==
        raster_bands_detail::effective_event_value(e, final, state);
    if (first &&
        (e.reason == kRasterReasonCram || e.reason == kRasterReasonVsram))
      for (int y = 0; y < e.line && y < height; ++y)
        touched[static_cast<std::size_t>(y)] = true;
    if (first && e.reason == kRasterReasonHscroll)
      for (int y = 0; y < e.line && y < height; ++y)
        if (reads_hscroll(e.address, y))
          touched[static_cast<std::size_t>(y)] = true;
    if (equals_final)
      continue;
    // Drawn with `e.data` from its line until the next write to the same
    // address, or to the bottom of the frame.
    int end = height;
    for (std::size_t j = i + 1; j < events.size(); ++j)
      if (events[j].reason == e.reason && events[j].address == e.address) {
        end = events[j].line;
        break;
      }
    for (int y = e.line; y < end && y < height; ++y)
      if (e.reason != kRasterReasonHscroll || reads_hscroll(e.address, y))
        touched[static_cast<std::size_t>(y)] = true;
  }
  // DI-21: a touched line the core drew exactly as its final-state
  // recomposition was drawn with that state, whatever the unobserved values
  // before the writes; only the lines that differ stay banded.
  if (any_line)
    for (int y = 0; y < height; ++y)
      touched[static_cast<std::size_t>(y)] = true;
  if (recomposed)
    for (int y = 0; y < height; ++y)
      touched[static_cast<std::size_t>(y)] =
          touched[static_cast<std::size_t>(y)] &&
          recomposed_diff[static_cast<std::size_t>(y)] != 0;
  // Runs of touched lines.
  std::array<RasterBand, 1024> runs{};
  std::size_t n = 0;
  for (int y = 0; y < height;) {
    if (!touched[static_cast<std::size_t>(y)]) {
      ++y;
      continue;
    }
    const int y0 = y;
    while (y < height && touched[static_cast<std::size_t>(y)])
      ++y;
    runs[n++] = {static_cast<std::uint16_t>(y0), static_cast<std::uint16_t>(y)};
  }
  // More runs than bands: merge the two separated by the smallest gap.
  while (n > kMaxRasterBands) {
    std::size_t best = 0;
    for (std::size_t k = 1; k + 1 < n; ++k)
      if (runs[k + 1].y0 - runs[k].y1 < runs[best + 1].y0 - runs[best].y1)
        best = k;
    runs[best].y1 = runs[best + 1].y1;
    for (std::size_t k = best + 1; k + 1 < n; ++k)
      runs[k] = runs[k + 1];
    --n;
  }
  out.localized = true;
  out.count = static_cast<std::uint32_t>(n);
  for (std::size_t k = 0; k < n; ++k)
    out.bands[k] = runs[k];
  return out;
}

/// Flags (1) every line where two RGB565 images of `width` x `height`
/// differ; `a_pitch` and `b_pitch` are in pixels.
inline void differing_lines(const std::uint16_t *a, std::size_t a_pitch,
                            const std::uint16_t *b, std::size_t b_pitch,
                            int width, int height,
                            std::span<std::uint8_t> out) noexcept {
  for (int y = 0; y < height && static_cast<std::size_t>(y) < out.size(); ++y) {
    const std::uint16_t *ra = a + static_cast<std::size_t>(y) * a_pitch;
    const std::uint16_t *rb = b + static_cast<std::size_t>(y) * b_pitch;
    std::uint8_t differs = 0;
    for (int x = 0; x < width && differs == 0; ++x)
      differs = ra[x] != rb[x] ? 1 : 0;
    out[static_cast<std::size_t>(y)] = differs;
  }
}

} // namespace ayther::session
