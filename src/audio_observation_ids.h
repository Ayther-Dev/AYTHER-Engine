#pragma once

#include <ayther/engine/audio_observer.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>

namespace ayther::audio_qa {

namespace observation = engine::audio_observation;

// Numeric producer identities are part of observation API 1.1. Each lane has
// one or more non-blocking writers; distinct lanes may be advanced
// concurrently. These IDs identify emission sites, not arrival order or an
// inferred causal relation.
enum class Producer : std::uint32_t {
  session = 1,
  pack = 2,
  detector_input = 3,
  detector = 4,
  selection = 5,
  mixer = 6,
  main_output = 7,
  postmix = 8,
  auxiliary_output = 9
};

// Internal primitive. Zero is permanently exhausted, never a valid identity.
// An explicit first value supports boundary tests without billions of calls.
class Sequence final {
public:
  constexpr Sequence() noexcept = default;
  explicit constexpr Sequence(std::uint64_t first) noexcept : next_(first) {}
  Sequence(const Sequence &) = delete;
  Sequence &operator=(const Sequence &) = delete;
  Sequence(Sequence &&) = delete;
  Sequence &operator=(Sequence &&) = delete;
  ~Sequence() = default;

  [[nodiscard]] std::optional<std::uint64_t> next() noexcept {
    if (next_ == 0) {
      return std::nullopt;
    }
    const auto result = next_;
    next_ =
        next_ == (std::numeric_limits<std::uint64_t>::max)() ? 0 : next_ + 1;
    return result;
  }

  void advance_past(std::uint64_t used) noexcept {
    if (next_ == 0 || used < next_)
      return;
    next_ = used == (std::numeric_limits<std::uint64_t>::max)() ? 0 : used + 1;
  }

private:
  std::uint64_t next_ = 1;
};

// Facts from one producer can cross the session and audio-device threads.
// Allocation stays non-blocking and preserves the permanent exhausted state.
class AtomicSequence final {
public:
  AtomicSequence() noexcept = default;
  AtomicSequence(const AtomicSequence &) = delete;
  AtomicSequence &operator=(const AtomicSequence &) = delete;

  [[nodiscard]] std::optional<std::uint64_t> next() noexcept {
    auto current = next_.load(std::memory_order_relaxed);
    while (current != 0) {
      const auto following =
          current == (std::numeric_limits<std::uint64_t>::max)()
              ? std::uint64_t{0}
              : current + 1;
      if (next_.compare_exchange_weak(current, following,
                                      std::memory_order_relaxed,
                                      std::memory_order_relaxed)) {
        return current;
      }
    }
    return std::nullopt;
  }

private:
  std::atomic<std::uint64_t> next_{1};
};

// Owned once by one QA run. Do not replace/reset while that run exists, even
// after disabling observation: reattachment must continue the same sequences.
// The Runtime bridge adds the run identity; another run has a different prefix.
// No heap, locks, I/O, clocks or audio business keys participate in allocation.
class IdentitySource final {
public:
  IdentitySource() = default;
  IdentitySource(const IdentitySource &) = delete;
  IdentitySource &operator=(const IdentitySource &) = delete;
  IdentitySource(IdentitySource &&) = delete;
  IdentitySource &operator=(IdentitySource &&) = delete;
  ~IdentitySource() = default;

  [[nodiscard]] std::optional<observation::FactId>
  next_fact(Producer producer) noexcept {
    const auto number = static_cast<std::uint32_t>(producer);
    if (number == 0 || number > facts_.size()) {
      return std::nullopt;
    }
    const auto sequence = facts_[number - 1].next();
    if (!sequence) {
      return std::nullopt;
    }
    return observation::FactId{number, *sequence};
  }

  // Session-owner thread only. Every new voice gets a new identity, including
  // coexistence/retrigger with the same key. A seek inside a voice retains it.
  [[nodiscard]] std::optional<observation::OccurrenceId>
  next_occurrence() noexcept {
    const auto sequence = occurrences_.next();
    if (!sequence) {
      return std::nullopt;
    }
    return observation::OccurrenceId{*sequence};
  }

  void reserve_occurrences_through(std::uint64_t used) noexcept {
    occurrences_.advance_past(used);
  }

private:
  std::array<AtomicSequence, 9> facts_{};
  Sequence occurrences_;
};

} // namespace ayther::audio_qa
