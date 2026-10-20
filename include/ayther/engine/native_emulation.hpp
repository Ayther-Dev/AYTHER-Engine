#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ayther::engine {

enum class NativeStatus : std::int32_t {
  ok = 0,
  end_of_trace = 1,
  invalid_argument = -1,
  abi_incompatible = -2,
  buffer_too_small = -3,
  invalid_handle = -4,
  wrong_state = -5,
  unsupported_capability = -6,
  unknown_content = -7,
  ambiguous_content = -8,
  invalid_content = -9,
  implementation_unavailable = -10,
  stale_epoch = -11,
  input_late = -12,
  input_out_of_order = -13,
  input_duplicate = -14,
  input_incomplete = -15,
  input_contradictory = -16,
  invalid_trace = -17,
  budget_exceeded = -18,
  control_pending = -19,
  checkpoint_incompatible = -20,
  checkpoint_invalid = -21,
  io_no_space = -22,
  io_access_denied = -23,
  io_publish_failed = -24,
  io_other = -25,
  out_of_memory = -26,
  backend_failure = -27
};

inline constexpr std::uint64_t native_cap_frame = 1;
inline constexpr std::uint64_t native_cap_input = 2;
inline constexpr std::uint64_t native_cap_video = 4;
inline constexpr std::uint64_t native_cap_pcm = 8;
inline constexpr std::uint64_t native_cap_memory = 16;
inline constexpr std::uint64_t native_cap_palette = 32;
inline constexpr std::uint64_t native_cap_sprites = 64;
inline constexpr std::uint64_t native_cap_geometry = 128;
inline constexpr std::uint64_t native_cap_events = 256;
inline constexpr std::uint64_t native_cap_checkpoint = 512;
inline constexpr std::uint64_t native_cap_safe_controls = 1024;
inline constexpr std::uint64_t native_cap_temporal_observation = 2048;
inline constexpr std::uint64_t native_cap_canonical_state = 4096;
inline constexpr std::uint32_t native_controls_applied = 1;
inline constexpr std::uint32_t native_control_cancelled = 2;
inline constexpr std::uint32_t native_cancel_reset = 1;
inline constexpr std::uint32_t native_cancel_restore = 2;
inline constexpr std::uint32_t native_cancel_close = 3;
inline constexpr std::uint32_t native_observation_region_mask = 1;
inline constexpr std::uint32_t native_observation_event_mask = 2;
inline constexpr std::uint32_t native_mask64 = 1;
inline constexpr std::uint32_t native_video_rgba8 = 1;

struct NativeFrameId {
  std::uint64_t session{}, epoch{}, frame{};
};
struct NativeResult {
  std::int32_t code{};
  std::uint32_t phase{}, os_domain{}, os_code{};
  NativeFrameId frame_id;
  std::uint64_t transaction{};
  std::uint32_t cancel_reason{}, flags{};
};
struct NativeConfig {
  std::string backend_id;
  std::vector<std::uint8_t> content, config, initial_persistent_memory;
  std::uint32_t system_override{};
};
struct NativeBackendInfo {
  std::uint32_t kind{}, system{};
  std::uint64_t capabilities{};
  std::string id, version;
  std::array<std::uint8_t, 32> build_id{};
};
struct NativeCandidate {
  std::uint32_t system{}, evidence_class{};
  std::uint64_t evidence{};
};
struct NativeDetection {
  std::uint32_t outcome{}, system{};
  std::uint64_t evidence{}, evidence_offset{}, evidence_bytes{};
  std::array<std::uint8_t, 32> content_sha256{};
  std::vector<NativeCandidate> candidates;
};
struct NativeSessionInfo {
  NativeFrameId frame_id;
  std::uint64_t generation{}, logical_tick{}, tick_hz_num{}, tick_hz_den{},
      capabilities{};
  std::uint32_t state{}, system{};
  std::array<std::uint8_t, 32> content_sha256{};
  std::uint64_t pending_transaction{}, next_replay_frame{};
  std::array<std::uint8_t, 32> backend_build_id{};
};
struct NativeInputAssignment {
  std::uint16_t port{}, control{};
  std::int32_t value{};
};
struct NativeInputFrame {
  std::uint64_t epoch{}, frame{};
  std::span<const NativeInputAssignment> assignments;
};
struct NativeControlItem {
  std::uint32_t id{}, value_type{};
  std::uint64_t value{};
};
struct NativeControlTransaction {
  std::uint64_t epoch{}, frame{}, id{};
  std::span<const NativeControlItem> items;
};

// Views expire only at the next successful step/reset/restore/close. Queries
// and failed operations do not invalidate them. Neither view transfers
// allocation.
struct NativeFrame {
  NativeFrameId frame_id;
  std::uint32_t width{}, height{}, row_bytes{}, video_format{}, aspect_num{},
      aspect_den{};
  std::span<const std::uint8_t> pixels, pcm;
  std::uint32_t sample_rate{}, channels{};
  std::uint64_t pcm_frames{}, tick_begin{}, tick_end{};
};
struct NativeSnapshotInfo {
  NativeFrameId frame_id;
  std::uint64_t generation{}, region_mask{}, complete_mask{}, retained_bytes{},
      logical_tick{};
  std::uint32_t region_count{};
};
struct NativeRegion {
  std::uint32_t kind{}, encoding{};
  std::uint64_t address{}, byte_count{};
  std::uint32_t element_bytes{}, element_count{};
  std::span<const std::uint8_t> data;
  std::uint32_t bank{}, flags{};
};

// Each object owns one independent reference. Historical bytes remain readable
// after the session closes. A caller retains a second reference before giving
// it to another thread; only retained snapshot reads/reference operations are
// thread-safe. Never free these views through the caller's allocator.
class NativeSnapshot {
public:
  NativeSnapshot() noexcept = default;
  ~NativeSnapshot();
  NativeSnapshot(const NativeSnapshot &) = delete;
  NativeSnapshot &operator=(const NativeSnapshot &) = delete;
  NativeSnapshot(NativeSnapshot &&other) noexcept;
  NativeSnapshot &operator=(NativeSnapshot &&other) noexcept;
  [[nodiscard]] NativeResult retain(NativeSnapshot &out) const noexcept;
  [[nodiscard]] NativeResult info(NativeSnapshotInfo &out) const noexcept;
  [[nodiscard]] NativeResult region(std::uint32_t index,
                                    NativeRegion &out) const noexcept;
  [[nodiscard]] NativeResult release() noexcept;
  [[nodiscard]] NativeResult last_result() const noexcept;

private:
  friend class NativeEmulation;
  std::uint64_t handle_{};
  mutable NativeResult last_;
};

namespace detail {
class TesseraBackend;
}

// Calls are serialized by the owner. Output objects are preserved on failure,
// except required byte counts and the documented semantic detection outputs.
// Buffers/required/next must not overlap. BUFFER_TOO_SMALL writes no payload
// and never consumes an event cursor. EOF is explicit and does not advance.
class NativeEmulation {
public:
  ~NativeEmulation();
  NativeEmulation(const NativeEmulation &) = delete;
  NativeEmulation &operator=(const NativeEmulation &) = delete;
  [[nodiscard]] static NativeResult
  create(const NativeConfig &configuration,
         std::unique_ptr<NativeEmulation> &out) noexcept;
  [[nodiscard]] static NativeResult
  backends(std::vector<NativeBackendInfo> &out) noexcept;
  [[nodiscard]] static NativeResult
  detect(std::span<const std::uint8_t> content, std::uint32_t system_override,
         NativeDetection &out) noexcept;
  [[nodiscard]] NativeResult info(NativeSessionInfo &out) noexcept;
  [[nodiscard]] NativeResult step(const NativeInputFrame &input) noexcept;
  [[nodiscard]] NativeResult pause() noexcept;
  [[nodiscard]] NativeResult reset() noexcept;
  [[nodiscard]] NativeResult close() noexcept;
  [[nodiscard]] NativeResult
  bind_trace(std::span<const std::uint8_t> trace) noexcept;
  [[nodiscard]] NativeResult replay_next() noexcept;
  [[nodiscard]] NativeResult
  submit_controls(const NativeControlTransaction &controls) noexcept;
  /// Engine presentation policy is an indivisible pair of observation masks.
  [[nodiscard]] NativeResult
  submit_presentation_policy(std::uint64_t epoch, std::uint64_t frame,
                             std::uint64_t transaction, std::uint64_t regions,
                             std::uint64_t events) noexcept;
  [[nodiscard]] NativeResult original(NativeFrame &out) noexcept;
  [[nodiscard]] NativeResult state(std::vector<std::uint8_t> &out) noexcept;
  [[nodiscard]] NativeResult events(std::uint64_t cursor,
                                    std::span<std::uint8_t> out,
                                    std::uint64_t &required,
                                    std::uint64_t &next) noexcept;
  [[nodiscard]] NativeResult losses(std::span<std::uint8_t> out,
                                    std::uint64_t &required) noexcept;
  [[nodiscard]] NativeResult snapshot_acquire(NativeSnapshot &out) noexcept;
  [[nodiscard]] NativeResult
  checkpoint(std::vector<std::uint8_t> &out) noexcept;
  [[nodiscard]] NativeResult
  restore_checkpoint(std::span<const std::uint8_t> checkpoint,
                     std::span<const std::uint8_t> trace) noexcept;
  [[nodiscard]] NativeResult last_result() const noexcept;

private:
  NativeEmulation();
  std::unique_ptr<detail::TesseraBackend> backend_;
};

} // namespace ayther::engine
