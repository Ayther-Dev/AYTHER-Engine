#pragma once

// Engine-owned, transport-independent audio observation contract.
// Views borrow producer storage only for the duration of a synchronous
// callback. This header declares an interface; it does not advertise
// implemented producers.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>

namespace ayther::engine::audio_observation {

struct ContractVersion {
  std::uint16_t major = 1;
  std::uint16_t minor = 0;
  friend constexpr bool operator==(ContractVersion, ContractVersion) = default;
};

// Independent of the Engine release, core ABI, replay and HD state versions.
inline constexpr ContractVersion contract_version{1, 0};
[[nodiscard]] constexpr bool supports(ContractVersion version) noexcept {
  return version == contract_version;
}

inline constexpr std::size_t max_causes = 256;
inline constexpr std::size_t max_state_orders = 256;
inline constexpr std::size_t max_fields = 128;
inline constexpr std::size_t max_text_bytes = 4096;
inline constexpr std::size_t max_encoded_fact_bytes = std::size_t{64} * 1024;
inline constexpr std::size_t max_pcm_bytes = std::size_t{256} * 1024;
inline constexpr std::uint32_t max_sample_rate = 192000;
inline constexpr std::uint16_t max_channels = 8;

// Run-local identities. The host adds its run identity when copying facts.
// Both members are nonzero. Arrival order across producers is not causality.
struct FactId {
  std::uint32_t producer = 0;
  std::uint64_t sequence = 0;
  friend constexpr bool operator==(FactId, FactId) = default;
};

// Distinct from a business key, track or assignment, even when these are
// reused.
struct OccurrenceId {
  std::uint64_t value = 0;
  friend constexpr bool operator==(OccurrenceId, OccurrenceId) = default;
};

enum class Availability : std::uint8_t { unknown, known, not_applicable };
enum class Unit : std::uint8_t {
  none,
  emulation_frame,
  sample_frame,
  bytes,
  count,
  linear_gain,
  frames_per_second,
  nanoseconds
};

// A preexisting cause names an explicitly observed initial-state record, not
// a guessed prior event. Empty causes do not imply that observation was
// complete.
struct PreexistingContext {
  std::string_view state_id;
};
using Cause = std::variant<FactId, PreexistingContext>;

struct StateOrder {
  std::string_view state_id;
  std::uint64_t sequence = 0;
};

using Value = std::variant<std::monostate, bool, std::uint64_t, std::int64_t,
                           double, std::string_view, FactId, OccurrenceId>;

// Known fields have a non-monostate value and no unavailable_reason. Unknown
// and not-applicable fields have monostate and a nonempty technical reason.
// Producer schemas define names, value types and units under this API version.
struct FieldView {
  std::string_view name;
  Availability availability = Availability::unknown;
  Unit unit = Unit::none;
  Value value;
  std::string_view unavailable_reason;
};

struct FramePosition {
  Availability availability = Availability::unknown;
  std::uint64_t emulation_frame = 0;
  std::string_view unavailable_reason;
};

struct FactView {
  FactId id;
  std::string_view kind;
  FramePosition frame;
  std::span<const Cause> causes;
  std::span<const StateOrder> state_orders;
  std::span<const FieldView> fields;
};

enum class PcmFormat : std::uint8_t { s16_le, s24_le, s32_le, f32_le };

// A sample frame contains one sample per channel. Ranges are [begin, end).
// Different timelines/rates require an observed mapping, never arrival order.
struct SampleRange {
  std::string_view timeline;
  std::uint32_t sample_rate = 0;
  std::uint64_t begin = 0;
  std::uint64_t end = 0;
};

struct PcmView {
  FactId id;
  std::string_view capture_point;
  SampleRange range;
  PcmFormat format = PcmFormat::s16_le;
  std::uint16_t channels = 0;
  std::span<const std::byte> bytes;
  std::span<const Cause> causes;
};

using FactCallback = void (*)(void *context, const FactView &fact) noexcept;
using PcmCallback = void (*)(void *context, const PcmView &pcm) noexcept;

// Non-owning binding. The host owns context, copies ALL nested views before
// return, and retains context until producers are detached and quiescent.
// Separate producers may call concurrently; the receiver supplies bounded,
// nonblocking handoff. Callbacks must not reenter or mutate Engine, perform
// I/O, serialize, wait, throw, or allocate unbounded storage. Copy failure is
// recorded out of band as observation loss and must never change an audio
// decision. Binding changes are permitted only while all affected producers are
// stopped. Null callbacks disable the corresponding stream; context may be null
// for a stateless callback. No result feeds back into scheduling or playback
// rules.
struct Observer {
  void *context = nullptr;
  FactCallback on_fact = nullptr;
  PcmCallback on_pcm = nullptr;

  void observe(const FactView &fact) const noexcept {
    if (on_fact != nullptr) {
      on_fact(context, fact);
    }
  }

  void observe(const PcmView &pcm) const noexcept {
    if (on_pcm != nullptr) {
      on_pcm(context, pcm);
    }
  }
};

} // namespace ayther::engine::audio_observation
