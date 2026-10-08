#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ayther/audio_playback_policy.h"

namespace ayther::engine::audio_observation {

struct AudioHdStateVersion {
  std::uint16_t major = 0;
  std::uint16_t minor = 0;

  friend constexpr bool operator==(AudioHdStateVersion,
                                   AudioHdStateVersion) = default;
};

// 1.1 adds AudioHdVoiceState::category so a paused/restored voice keeps its
// logical bus. A 1.0 payload is rejected transactionally: silently defaulting
// it would route music/ambient as effects after resume.
inline constexpr AudioHdStateVersion kAudioHdStateVersion{1, 1};
inline constexpr std::size_t kAudioHdStateIdentityLimit = 256;
inline constexpr std::size_t kAudioHdDetectorStateLimit = 1024 * 1024;
inline constexpr std::size_t kAudioHdStateCollectionLimit = 4096;
inline constexpr std::size_t kAudioHdVoiceLimit = 256;
inline constexpr std::size_t kAudioHdVoicePcmByteLimit = 64 * 1024 * 1024;
inline constexpr std::size_t kAudioHdPendingPcmByteLimit = 16 * 1024 * 1024;
inline constexpr std::size_t kAudioHdAssetPathLimit = 4096;

enum class AudioHdStateSection : std::uint32_t {
  detector = 1U << 0,
  windows = 1U << 1,
  voices = 1U << 2,
  requests = 1U << 3,
  pending_audio = 1U << 4
};

[[nodiscard]] constexpr std::uint32_t
audio_hd_state_section(AudioHdStateSection section) noexcept {
  return static_cast<std::uint32_t>(section);
}

inline constexpr std::uint32_t kAudioHdStateRequiredSections =
    audio_hd_state_section(AudioHdStateSection::detector) |
    audio_hd_state_section(AudioHdStateSection::windows) |
    audio_hd_state_section(AudioHdStateSection::voices) |
    audio_hd_state_section(AudioHdStateSection::requests) |
    audio_hd_state_section(AudioHdStateSection::pending_audio);

struct AudioHdStateHeader {
  AudioHdStateVersion version = kAudioHdStateVersion;
  std::string game_state_identity;
  std::uint64_t emulation_frame = 0;
  std::uint32_t sections = 0;
};

enum class AudioHdStateValidationCode : std::uint8_t {
  compatible,
  unsupported_version,
  missing_expected_identity,
  missing_identity,
  identity_too_long,
  identity_mismatch,
  frame_mismatch,
  missing_sections,
  unknown_sections
};

struct AudioHdStateValidation {
  AudioHdStateValidationCode code =
      AudioHdStateValidationCode::unsupported_version;
  std::uint32_t missing_sections = 0;
  std::uint32_t unknown_sections = 0;

  [[nodiscard]] constexpr bool compatible() const noexcept {
    return code == AudioHdStateValidationCode::compatible;
  }
};

enum class AudioHdWindowKind : std::uint8_t { sequence, live_sequence };

struct AudioHdWindowState {
  AudioHdWindowKind kind = AudioHdWindowKind::sequence;
  std::uint64_t key = 0;
  std::uint64_t signature = 0;
  std::uint64_t start_frame = 0;
  std::uint64_t end_frame = 0;
  std::uint64_t last_seen_frame = 0;
  std::uint32_t channel_mask = 0;
};

struct AudioHdNextAnchorState {
  std::uint64_t key = 0;
  std::uint64_t frame = 0;
};

struct AudioHdLearnedSignatureState {
  std::uint64_t signature = 0;
  std::uint64_t instrument = 0;
  std::uint8_t pitch = 0xFF;
  bool assigned_instrument = false;
};

/// Complete state needed for detector continuity and window decisions. The
/// detector payload is opaque to C++ and interpreted by the versioned core.
struct AudioHdDetectorWindowsState {
  std::vector<std::uint8_t> detector;
  std::vector<std::uint64_t> previous_active_signatures;
  std::vector<AudioHdWindowState> windows;
  std::vector<AudioHdNextAnchorState> next_anchors;
  std::vector<AudioHdLearnedSignatureState> learned_signatures;
  std::uint32_t runtime_channel_mask = 0;
  bool complete = true;
};

/// A state-local PCM identity. Several voices may refer to one asset without
/// duplicating its samples in the restored state.
struct AudioHdPcmAssetState {
  std::uint64_t identity = 0;
  std::vector<std::int16_t> samples;

  friend bool operator==(const AudioHdPcmAssetState &,
                         const AudioHdPcmAssetState &) = default;
};

/// Spec 002 (D-6b): one active voice's source PCM, shared with the mixer
/// instead of copied. Produced by the shared export of `AudioHdVoicesState`,
/// whose matching `pcm_assets` entry then carries the identity only.
struct AudioHdSharedPcmAsset {
  std::uint64_t identity = 0;
  std::shared_ptr<const std::vector<std::int16_t>> samples;
};

struct AudioHdVoiceState {
  std::uint64_t occurrence = 0;
  std::uint32_t cause_producer = 0;
  std::uint64_t cause_sequence = 0;
  std::uint64_t business_key = 0;
  std::uint64_t pcm_identity = 0;
  std::uint64_t source_position = 0;
  std::uint64_t output_start = 0;
  std::uint64_t output_end = 0;
  bool output_end_known = false;
  float gain = 1.0F;
  bool looping = false;
  bool event = false;
  std::uint64_t end_frame = (std::numeric_limits<std::uint64_t>::max)();
  std::uint64_t cut_frame = (std::numeric_limits<std::uint64_t>::max)();
  std::uint32_t fade_remaining = 0;
  std::uint32_t fade_span = 0;
  std::uint32_t fade_frames = 0;
  bool tail_active = false;
  bool fade_effect_active = false;
  std::uint64_t loop_begin = 0;
  std::uint64_t loop_end = 0;
  std::uint64_t late_samples = 0;
  /// Logical mix bus. It is part of the resumable voice identity/policy, not
  /// recomputed from a mutable assignment after a pause.
  ayther::AudioCategory category = ayther::AudioCategory::effect;

  friend bool operator==(const AudioHdVoiceState &,
                         const AudioHdVoiceState &) = default;
};

struct AudioHdVoicesState {
  std::vector<AudioHdPcmAssetState> pcm_assets;
  std::vector<AudioHdVoiceState> voices;
  std::uint64_t started = 0;
  std::uint64_t mixed_samples = 0;
  std::uint64_t skew_samples = 0;
  std::uint64_t max_skew_samples = 0;
  bool complete = true;

  friend bool operator==(const AudioHdVoicesState &,
                         const AudioHdVoicesState &) = default;
};

struct AudioHdRequestState {
  std::uint64_t business_key = 0;
  std::string asset;
  std::uint64_t start_frame = 0;
  std::uint64_t end_frame = (std::numeric_limits<std::uint64_t>::max)();
  std::uint64_t cut_frame = (std::numeric_limits<std::uint64_t>::max)();
  std::uint32_t channel_mask = 0;
  float gain = 1.0F;
  bool looping = false;
  bool sequence_substitution = false;
  std::uint64_t occurrence = 0;

  friend bool operator==(const AudioHdRequestState &,
                         const AudioHdRequestState &) = default;
};

enum class AudioHdFiredRequestKind : std::uint8_t { sequence, event };

struct AudioHdFiredRequestState {
  AudioHdFiredRequestKind kind = AudioHdFiredRequestKind::sequence;
  std::uint64_t business_key = 0;
  std::uint32_t start_frame_plus_one = 0;

  friend bool operator==(const AudioHdFiredRequestState &,
                         const AudioHdFiredRequestState &) = default;
};

struct AudioHdPendingBatchState {
  std::uint64_t source_hash = 0;
  std::uint64_t frame_offset = 0;
  std::uint64_t frames = 0;

  friend bool operator==(const AudioHdPendingBatchState &,
                         const AudioHdPendingBatchState &) = default;
};

struct AudioHdPendingOriginalState {
  std::uint64_t source_hash = 0;
  std::uint64_t frame_offset = 0;
  std::uint64_t frames = 0;
  std::uint64_t emulation_frame = 0;
  std::uint64_t frame_boundary = 0;
  bool frame_known = false;
  bool hash_mute_applied = true;

  friend bool operator==(const AudioHdPendingOriginalState &,
                         const AudioHdPendingOriginalState &) = default;
};

/// Canonical pending audio at a replay boundary. SDL stream queues are not
/// represented because reading them would consume playback; a capture with
/// queued stream bytes is therefore incomplete and cannot be restored.
struct AudioHdPendingAudioState {
  std::vector<std::int16_t> main_pcm;
  std::vector<AudioHdPendingBatchState> main_batches;
  std::vector<AudioHdPendingOriginalState> original_batches;
  std::vector<std::uint64_t> frame_mute_hashes;
  std::vector<std::uint64_t> user_mute_hashes;
  std::uint64_t timeline_samples = 0;
  std::uint64_t frame_mark = 0;
  std::uint64_t current_emulation_frame = 0;
  std::uint64_t current_frame_boundary = 0;
  std::uint64_t main_output_samples = 0;
  std::uint64_t auxiliary_input_samples = 0;
  std::uint64_t auxiliary_consumed_input = 0;
  std::uint64_t auxiliary_discard_before = 0;
  std::uint64_t auxiliary_submission = 0;
  std::uint64_t auxiliary_resample_phase_q32 = 0;
  bool current_frame_known = false;
  bool original_batches_overflow = false;
  bool complete = true;

  friend bool operator==(const AudioHdPendingAudioState &,
                         const AudioHdPendingAudioState &) = default;
};

struct AudioHdRequestsPendingState {
  std::vector<AudioHdRequestState> requests;
  std::vector<AudioHdFiredRequestState> fired_requests;
  AudioHdPendingAudioState pending_audio;
  bool complete = true;

  friend bool operator==(const AudioHdRequestsPendingState &,
                         const AudioHdRequestsPendingState &) = default;
};

enum class AudioHdRestoreCode : std::uint8_t {
  restored,
  incompatible_header,
  incomplete_payload,
  invalid_detector,
  invalid_window,
  invalid_pcm,
  invalid_voice,
  invalid_request,
  invalid_pending_audio,
  duplicate_value,
  resource_limit
};

struct AudioHdRestoreResult {
  AudioHdRestoreCode code = AudioHdRestoreCode::incomplete_payload;
  AudioHdStateValidation header{};

  [[nodiscard]] constexpr bool restored() const noexcept {
    return code == AudioHdRestoreCode::restored;
  }
};

/// Validates only the envelope needed before restoration. The opaque identity
/// must name the exact restored game state, not merely a title or a run.
[[nodiscard]] inline AudioHdStateValidation
validate_audio_hd_state(const AudioHdStateHeader &state,
                        std::string_view expected_identity,
                        std::uint64_t expected_frame) noexcept {
  if (state.version != kAudioHdStateVersion)
    return {AudioHdStateValidationCode::unsupported_version};
  if (expected_identity.empty())
    return {AudioHdStateValidationCode::missing_expected_identity};
  if (state.game_state_identity.empty())
    return {AudioHdStateValidationCode::missing_identity};
  if (state.game_state_identity.size() > kAudioHdStateIdentityLimit ||
      expected_identity.size() > kAudioHdStateIdentityLimit)
    return {AudioHdStateValidationCode::identity_too_long};
  if (state.game_state_identity != expected_identity)
    return {AudioHdStateValidationCode::identity_mismatch};
  if (state.emulation_frame != expected_frame)
    return {AudioHdStateValidationCode::frame_mismatch};

  const auto missing = kAudioHdStateRequiredSections & ~state.sections;
  if (missing != 0)
    return {AudioHdStateValidationCode::missing_sections, missing, 0};
  const auto unknown = state.sections & ~kAudioHdStateRequiredSections;
  if (unknown != 0)
    return {AudioHdStateValidationCode::unknown_sections, 0, unknown};
  return {AudioHdStateValidationCode::compatible};
}

} // namespace ayther::engine::audio_observation
