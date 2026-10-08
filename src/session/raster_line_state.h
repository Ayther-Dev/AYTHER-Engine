// Spec 002, DI-17 (RF-3.5, O1): validate the frame-scoped LINE_STATE ABI
// before it is allowed to localize horizontal-scroll raster writes.
#pragma once

#include "libretro_host/ayther_api.h"
#include "session/raster_bands.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace ayther::session {

/// LINE_STATE is consumed as an array of native C records. The descriptor is
/// therefore part of the parsing contract, not merely a source of byte_size.
[[nodiscard]] inline bool
line_state_region_is_compatible(const ayther_region_info_v1 &info,
                                std::size_t required_lines) noexcept {
  constexpr std::uint32_t kRequiredAccess = AYTHER_REGION_ACCESS_READ |
                                            AYTHER_REGION_FRAME_SCOPED |
                                            AYTHER_REGION_NATIVE_ENDIAN;
  constexpr std::uint32_t kForbiddenAccess =
      AYTHER_REGION_ACCESS_CONTROL_WRITE | AYTHER_REGION_WORD_SWAPPED_LE;

  if (info.struct_size < sizeof(info) ||
      info.region_id != AYTHER_REGION_LINE_REGS ||
      info.data_version != AYTHER_LAYOUT_LINE_REGS_V1 ||
      info.element_size != sizeof(ayther_line_regs_v1) ||
      (info.access_flags & kRequiredAccess) != kRequiredAccess ||
      (info.access_flags & kForbiddenAccess) != 0 ||
      required_lines > info.capacity)
    return false;

  if (info.byte_size < sizeof(ayther_line_header_v1))
    return false;
  const std::size_t payload_bytes =
      static_cast<std::size_t>(info.byte_size) - sizeof(ayther_line_header_v1);
  return required_lines <= payload_bytes / sizeof(ayther_line_regs_v1);
}

/// A successful frame-scoped read is usable only when the core reports the
/// exact generation requested. AYTHER_GENERATION_ANY cannot establish this.
[[nodiscard]] inline bool
frame_region_read_is_coherent(std::int32_t status,
                              std::uint64_t actual_generation,
                              std::uint64_t expected_generation) noexcept {
  return status == AYTHER_STATUS_OK &&
         expected_generation != AYTHER_GENERATION_ANY &&
         actual_generation == expected_generation;
}

/// Validate both the snapshot coherence and the journal's self-description
/// before any event is traversed.
[[nodiscard]] inline bool
raster_journal_is_coherent(std::int32_t status, std::uint64_t actual_generation,
                           std::uint64_t expected_generation,
                           const ayther_journal_v1 &journal) noexcept {
  constexpr std::size_t kEventsOffset = offsetof(ayther_journal_v1, events);
  if (!frame_region_read_is_coherent(status, actual_generation,
                                     expected_generation) ||
      journal.layout_version != AYTHER_LAYOUT_JOURNAL_V1 ||
      journal.struct_size < kEventsOffset ||
      journal.count > AYTHER_JOURNAL_MAX_EVENTS)
    return false;

  const std::size_t declared_event_bytes =
      static_cast<std::size_t>(journal.struct_size) - kEventsOffset;
  return journal.count <=
         declared_event_bytes / sizeof(ayther_journal_event_v1);
}

/// A pending SYSTEM geometry describes the emitted frame while VDP_REGS has
/// already advanced to the next one. Returning the unknown-mode sentinel keeps
/// SAT matching from interpreting that future frame's R12 as H32/H40 evidence.
[[nodiscard]] inline bool
raster_state_matches_emitted_frame(const ayther_system_v1 &system) noexcept {
  return (system.flags & AYTHER_SYSTEM_GEOMETRY_PENDING) == 0;
}

[[nodiscard]] inline std::uint8_t
raster_identity_vdp_mode(const ayther_system_v1 &system) noexcept {
  return raster_state_matches_emitted_frame(system) ? system.vdp_mode
                                                    : std::uint8_t{0};
}

/// Parse AYTHER_REGION_LINE_REGS into the small layout view needed by raster
/// bands. A malformed, incomplete or stale region is no evidence: callers
/// must keep the complete core frame in that case.
[[nodiscard]] inline std::optional<std::size_t> parse_raster_line_layouts(
    std::span<const std::uint8_t> bytes, std::uint64_t expected_generation,
    std::size_t required_lines, std::span<RasterLineLayout> out) noexcept {
  if (bytes.size() < sizeof(ayther_line_header_v1) ||
      required_lines > out.size())
    return std::nullopt;

  ayther_line_header_v1 header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  if (header.struct_size < sizeof(header) ||
      header.struct_size > bytes.size() ||
      header.entry_size < sizeof(ayther_line_regs_v1) ||
      header.lines < required_lines ||
      (header.flags & AYTHER_LINES_OVERFLOW) != 0 ||
      header.frame_generation != expected_generation)
    return std::nullopt;

  const std::size_t payload = bytes.size() - header.struct_size;
  if (header.entry_size == 0 || payload / header.entry_size < header.lines)
    return std::nullopt;

  for (std::size_t line = 0; line < required_lines; ++line) {
    ayther_line_regs_v1 entry{};
    const std::size_t offset =
        header.struct_size + line * static_cast<std::size_t>(header.entry_size);
    std::memcpy(&entry, bytes.data() + offset, sizeof(entry));
    out[line] = {static_cast<std::uint8_t>(entry.reg11 & 0x03U), entry.hscb};
  }
  return required_lines;
}

} // namespace ayther::session
