// The visual state payload (spec 002, contracts.md C4): framing of the
// sections and the encoding of the sections the session itself owns. Pure:
// the session copies its members into these values and back; the sections the
// Rust core owns travel as opaque bodies validated by the core.
#pragma once

#include <ayther/engine/visual_state.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ayther::session::visual {

namespace vs = engine::visual_state;

inline constexpr std::size_t kSectionCount = 11;

/// Payload position of a section (its bit index).
[[nodiscard]] std::size_t section_index(vs::VisualStateSection section);

using SectionBodies = std::array<std::span<const std::byte>, kSectionCount>;

/// Appends one section (u32 bit, u32 size, body). Sections are appended in
/// increasing bit order.
void append_section(std::vector<std::byte> &payload,
                    vs::VisualStateSection section,
                    std::span<const std::byte> body);

/// Splits `payload` into the bodies of the sections in `sections`: each one
/// exactly once, in increasing order, and nothing else. False = invalid.
[[nodiscard]] bool split_sections(std::span<const std::byte> payload,
                                  std::uint32_t sections, SectionBodies &out);

struct ScreenRecognition {
  std::uint64_t active = 0;
  std::uint64_t candidate = 0;
  std::int32_t streak = 0;
};

struct LevelCamera {
  std::array<std::int32_t, 2> cam_x{};
  std::array<std::int32_t, 2> cam_y{};
  std::array<std::int16_t, 2> prev_h{};
  std::array<std::int16_t, 2> prev_v{};
  std::uint64_t last_frame = UINT64_MAX;
  bool valid = false;
  std::uint64_t pano_last_id = 0;
  std::int32_t pano_last_x = 0;
  std::int32_t pano_last_y = 0;
};

struct PaletteLuma {
  std::array<double, 4> peak{};
};

struct PreviousAudioMask {
  std::uint32_t mask = 0;
};

struct PlaneSequenceClock {
  std::uint64_t id = 0;
  std::int64_t anchor = -1;
  std::int64_t last_seen = -1;
};

struct Cinematic {
  std::uint64_t active = 0;
  std::uint32_t step = 0;
  std::uint32_t gap = 0;
  std::int64_t last_frame = -1;
  std::uint64_t video_kinematic = 0;
  std::uint32_t video_step = 0;
  std::int64_t video_anchor = -1;
  std::uint64_t audio_kinematic = 0;
  std::int64_t audio_anchor = 0;
  bool audio_on = false;
  std::int64_t audio_last_frame = -1;
  std::int32_t audio_still = 0;
  float audio_gain = 1.0F;
};

struct HdAnimationPhase {
  std::uint64_t clip_id = 0;
  std::int32_t last_pose = -1;
  std::uint64_t pose_start_frame = 0;
};

/// The tint reference a Panorama captures while anchored (peak-hold).
struct PanoramaTint {
  std::uint64_t id = 0;
  double ref_luma = 0.0;
  std::array<double, 4> ref_w{};
  std::array<double, 3> ref_ch{};
  bool ref_chroma = false;
  double ref_peak = 0.0;
};

/// Most entries a list section may carry.
inline constexpr std::size_t kListLimit = 4096;

[[nodiscard]] std::vector<std::byte> encode(const ScreenRecognition &value);
[[nodiscard]] std::vector<std::byte> encode(const LevelCamera &value);
[[nodiscard]] std::vector<std::byte> encode(const PaletteLuma &value);
[[nodiscard]] std::vector<std::byte> encode(const PreviousAudioMask &value);
[[nodiscard]] std::vector<std::byte>
encode(std::span<const PlaneSequenceClock> clocks);
[[nodiscard]] std::vector<std::byte> encode(const Cinematic &value);
[[nodiscard]] std::vector<std::byte>
encode(std::span<const HdAnimationPhase> phases);
[[nodiscard]] std::vector<std::byte>
encode(std::span<const PanoramaTint> tints);

// Decoders accept a body only when it is complete and consumed exactly. Lists
// must be sorted by id without repeats, as the encoders write them.
[[nodiscard]] bool decode(std::span<const std::byte> body,
                          ScreenRecognition &out);
[[nodiscard]] bool decode(std::span<const std::byte> body, LevelCamera &out);
[[nodiscard]] bool decode(std::span<const std::byte> body, PaletteLuma &out);
[[nodiscard]] bool decode(std::span<const std::byte> body,
                          PreviousAudioMask &out);
[[nodiscard]] bool decode(std::span<const std::byte> body,
                          std::vector<PlaneSequenceClock> &out);
[[nodiscard]] bool decode(std::span<const std::byte> body, Cinematic &out);
[[nodiscard]] bool decode(std::span<const std::byte> body,
                          std::vector<HdAnimationPhase> &out);
[[nodiscard]] bool decode(std::span<const std::byte> body,
                          std::vector<PanoramaTint> &out);

} // namespace ayther::session::visual
