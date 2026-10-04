#pragma once

// Engine-owned visual state (spec 002, contracts.md C4).
//
// What a session carries from one frame to the next to draw the following
// ones. Restoring the core state, this state and the HD audio state of frame
// k, and then producing k+1... with the recorded inputs, gives the same scene,
// replacements, claims and render observation as producing them linearly.
//
// Same pattern as the HD audio state: a versioned header names the exact core
// state (`game_state_identity`) and the frame; every section is required; a
// restore validates everything first and leaves the session untouched when
// anything is incompatible.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ayther::engine::visual_state {

struct VisualStateVersion {
  std::uint16_t major = 1;
  std::uint16_t minor = 0;
  friend constexpr bool operator==(VisualStateVersion,
                                   VisualStateVersion) = default;
};

// Independent of the Engine release and of the HD audio state version.
inline constexpr VisualStateVersion kVisualStateVersion{1, 0};

enum class VisualStateSection : std::uint32_t {
  sprite_tweens = 1U << 0,         // in-betweens in progress per instance
  screen_recognition = 1U << 1,    // Picture hysteresis
  level_camera = 1U << 2,          // level camera + Panorama vote continuity
  palette_luma = 1U << 3,          // per-palette luminance peak (E1 tint)
  previous_audio_mask = 1U << 4,   // channel mask applied to the last frame
  palette_signature = 1U << 5,     // CRAM stability and latched signatures
  animation_grouper = 1U << 6,     // SAT slot histories and groups
  plane_sequence_clocks = 1U << 7, // plane Animation clocks
  cinematic = 1U << 8,             // Kinematic in progress, video and audio
  hd_animation_phase = 1U << 9,    // phase of each Level-1 HD animation
  panorama_tint = 1U << 10,        // anchored tint reference of each Panorama
};

[[nodiscard]] constexpr std::uint32_t
visual_state_section(VisualStateSection section) noexcept {
  return static_cast<std::uint32_t>(section);
}

inline constexpr std::uint32_t kVisualStateRequiredSections = 0x7FFU;

// The Lua state of the pack's scripts is not part of the payload. A pack whose
// scripts run every frame (`ayther.on_frame`) is `not_exportable`: its takes
// are recovered by simulating from their initial state instead.
enum class ScriptState : std::uint8_t { none, not_exportable };

inline constexpr std::size_t kVisualStateIdentityLimit = 256;
inline constexpr std::size_t kVisualStatePayloadLimit = 64U * 1024U * 1024U;

struct VisualStateHeader {
  VisualStateVersion version = kVisualStateVersion;
  // Identity of the exact core state: SHA-256 (lowercase hex) of its savestate,
  // as AytherSession::game_state_identity() returns it.
  std::string game_state_identity;
  std::uint64_t emulation_frame = 0;
  std::uint32_t sections = 0; // every required section must be present
  ScriptState script_state = ScriptState::none;
};

// The payload holds the sections in increasing bit order, each one as
// u32 section bit, u32 size (little-endian) and its body.
struct VisualState {
  VisualStateHeader header;
  std::vector<std::byte> payload;
};

enum class VisualStateRestoreCode : std::uint8_t {
  restored,
  unsupported_version,
  identity_mismatch,
  frame_mismatch,
  missing_sections,
  unknown_sections,
  invalid_payload,
};

struct VisualStateRestoreResult {
  VisualStateRestoreCode code = VisualStateRestoreCode::invalid_payload;
  std::uint32_t missing_sections = 0;
  std::uint32_t unknown_sections = 0;
  [[nodiscard]] constexpr bool restored() const noexcept {
    return code == VisualStateRestoreCode::restored;
  }
};

// Header checks in the contract's order: version, identity, frame, sections.
// `restored` here means only that the header is compatible; the payload is
// validated by the restore itself.
[[nodiscard]] constexpr VisualStateRestoreResult
validate_visual_state_header(const VisualStateHeader &header,
                             std::string_view expected_identity,
                             std::uint64_t expected_frame) noexcept {
  using Code = VisualStateRestoreCode;
  if (!(header.version == kVisualStateVersion))
    return {Code::unsupported_version};
  if (expected_identity.empty() || header.game_state_identity.empty() ||
      header.game_state_identity.size() > kVisualStateIdentityLimit ||
      header.game_state_identity != expected_identity)
    return {Code::identity_mismatch};
  if (header.emulation_frame != expected_frame)
    return {Code::frame_mismatch};
  const std::uint32_t missing = kVisualStateRequiredSections & ~header.sections;
  if (missing != 0)
    return {Code::missing_sections, missing, 0};
  const std::uint32_t unknown = header.sections & ~kVisualStateRequiredSections;
  if (unknown != 0)
    return {Code::unknown_sections, 0, unknown};
  return {Code::restored};
}

} // namespace ayther::engine::visual_state
