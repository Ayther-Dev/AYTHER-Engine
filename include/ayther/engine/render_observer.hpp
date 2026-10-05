#pragma once

// Engine-owned render observation contract (spec 002, contracts.md C3).
//
// Per produced frame it tells which sprite occurrences were detected, which HD
// replacement claimed each of them (with its exact members), why an assigned
// pose was not applied when that is known, how the replacement was drawn and
// whether the frame could be composed. Views borrow producer storage only for
// the duration of a synchronous callback: consumers copy what they keep.
//
// The host publishes the observation after rendering the frame, passing the
// renderer's draw report (or none when it does not render, e.g. headless), and
// before stepping the session again. With no observer installed the Engine
// builds nothing and the frame view is unchanged.
#include <ayther/engine/audio_observer.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ayther::engine::render_observation {

struct ContractVersion {
  std::uint16_t major = 1;
  std::uint16_t minor = 0;
  friend constexpr bool operator==(ContractVersion, ContractVersion) = default;
};

// Independent of the Engine release, the audio observation contract, the core
// ABI and the replay format versions. 1.1 (spec 002, DI-18) adds the layers
// of the renderer's stack, overlays included; a 1.0 consumer is served too.
inline constexpr ContractVersion contract_version{1, 1};
[[nodiscard]] constexpr bool supports(ContractVersion version) noexcept {
  return version.major == contract_version.major &&
         version.minor <= contract_version.minor;
}

// Per-frame limits (spec 002, DA-2 «Datos por frame»). Above them the spans
// stop at the limit and the *_total counters keep the real count: an excess is
// reported, never truncated silently.
inline constexpr std::size_t max_occurrences = 256;
inline constexpr std::size_t max_replacements = 256;

using audio_observation::Availability;
using audio_observation::FieldView;
using audio_observation::FramePosition;

// Why a frame could or could not be composed element by element.
enum class Composability : std::uint8_t {
  composable,
  raster_split,
  fade,
  line_hscroll,
  column_vscroll,
  other,
};

enum class OccurrenceStatus : std::uint8_t {
  replaced,             // member of a replacement that claimed it
  original_unassigned,  // drawn as the original sprite: no assignment matched
  assigned_not_applied, // an assignment exists but its replacement was not used
  hidden_by_author,     // hidden on purpose by an authoring tool
};

enum class DrawOutcome : std::uint8_t { in_pass, partitioned, lane, discarded };
enum class TextureState : std::uint8_t { ready, pending, failed };

// Unique within a frame through `index`, the position of the occurrence in the
// frame's list. SAT slot and link-chain position are kept for meaning: they
// are not unique on every producer (the VRAM fallback and the cumulative parsed
// list can repeat a slot).
struct OccurrenceId {
  std::uint16_t index = 0;
  std::uint8_t slot = 0;
  std::uint8_t chain = 0;
  friend constexpr bool operator==(OccurrenceId, OccurrenceId) = default;
};

struct OccurrenceView {
  OccurrenceId id;
  std::uint64_t identity_hash = 0; // sprite hash, current algorithm
  std::int16_t x = 0;              // native screen rectangle
  std::int16_t y = 0;
  std::uint16_t w = 0;
  std::uint16_t h = 0;
  std::uint8_t priority = 0; // VDP priority bit
  OccurrenceStatus status = OccurrenceStatus::original_unassigned;
  std::int32_t replacement = -1; // index into replacements when replaced
  // Assigned pose key as a string_view; not_applicable without assignment.
  FieldView pose;
  // texture_pending | texture_failed | frame_not_composable | member_hidden |
  // hd_off; unknown when the Engine cannot tell. Only meaningful for
  // assigned_not_applied (not_applicable otherwise).
  FieldView not_applied_reason;
};

struct ReplacementView {
  std::uint32_t index = 0;
  std::string_view kind;     // pose | sprite | plane_set | screen | panorama
  std::string_view pose_key; // empty when the kind has no pose
  std::string_view asset;    // asset path inside the pack
  // Exactly the occurrences this replacement claimed (none of another one).
  std::span<const OccurrenceId> members;
  // Known only when the host published a draw report for this frame.
  Availability render_availability = Availability::unknown;
  DrawOutcome draw = DrawOutcome::discarded;
  TextureState texture = TextureState::pending;
};

// Contract 1.1 (spec 002, DI-18): one layer of the stack the renderer drew
// the frame with, in stack order (back to front). `name` borrows the stack.
// The gate and draw fields are meaningful for overlays (Custom layers, the
// pack's Acetatos) only.
struct LayerView {
  std::string_view kind; // plane_b | plane_a | window | sprites | ... | overlay
  std::string_view name;
  std::uint32_t stack_index = 0;
  bool visible = true;
  bool overlay = false;
  bool gated = false;     // the overlay shows only while a Cuadro is present
  bool gate_open = false; // its Cuadro is present in this frame
  bool drawn = false;     // its sheet was drawn in this frame
};

struct RenderFrameView {
  FramePosition frame;
  Composability composability = Composability::composable;
  std::span<const OccurrenceView> occurrences;
  std::span<const ReplacementView> replacements;
  std::size_t occurrences_total = 0;  // > occurrences.size() only on excess
  std::size_t replacements_total = 0; // > replacements.size() only on excess
  // Contract 1.1: the layers of the stack the frame was drawn with; empty
  // when the host published no draw report.
  std::span<const LayerView> layers = {};
};

// How the renderer drew each replacement of the frame, index-aligned with the
// frame's replacements. Produced by the renderer, consumed by the session.
struct ReplacementDraw {
  DrawOutcome draw = DrawOutcome::discarded;
  TextureState texture = TextureState::pending;
};

struct DrawReport {
  std::uint64_t emulation_frame = 0;
  // False when the frame was rendered with HD off: every replacement then
  // stays unapplied with the reason hd_off.
  bool hd_enabled = true;
  std::span<const ReplacementDraw> replacements;
  // Contract 1.1: the layers of the stack, in stack order.
  std::span<const LayerView> layers = {};
};

class RenderObserver {
public:
  RenderObserver() = default;
  RenderObserver(const RenderObserver &) = delete;
  RenderObserver &operator=(const RenderObserver &) = delete;
  virtual ~RenderObserver() = default;
  // Called once per published frame, synchronously, on the session's thread.
  virtual void on_render_frame(const RenderFrameView &frame) noexcept = 0;

protected:
  RenderObserver(RenderObserver &&) = default;
  RenderObserver &operator=(RenderObserver &&) = default;
};

} // namespace ayther::engine::render_observation
