#pragma once

#include <ayther/engine/audio_observer.hpp>

#include <atomic>
#include <cstdint>

namespace ayther::engine::audio_observation {

struct ObservationQueueCloseSnapshot {
  bool closed = false;
  FactId last_emitted{};
  FactId last_consumed{};
  std::uint64_t overflow_count = 0;

  [[nodiscard]] bool complete() const noexcept {
    return closed && last_emitted == last_consumed && overflow_count == 0;
  }
};

namespace detail {

class ObservationQueueProgress {
public:
  void record_emitted(FactId id) noexcept {
    emitted_producer_.store(id.producer, std::memory_order_relaxed);
    emitted_sequence_.store(id.sequence, std::memory_order_release);
  }

  void record_consumed(FactId id) noexcept {
    consumed_producer_.store(id.producer, std::memory_order_relaxed);
    consumed_sequence_.store(id.sequence, std::memory_order_release);
  }

  void close() noexcept { closed_.store(true, std::memory_order_release); }

  [[nodiscard]] bool closed() const noexcept {
    return closed_.load(std::memory_order_acquire);
  }

  [[nodiscard]] ObservationQueueCloseSnapshot
  snapshot(std::uint64_t overflow_count) const noexcept {
    const auto emitted_sequence =
        emitted_sequence_.load(std::memory_order_acquire);
    const auto consumed_sequence =
        consumed_sequence_.load(std::memory_order_acquire);
    return {
        closed(),
        {emitted_producer_.load(std::memory_order_relaxed), emitted_sequence},
        {consumed_producer_.load(std::memory_order_relaxed), consumed_sequence},
        overflow_count};
  }

private:
  std::atomic<std::uint32_t> emitted_producer_{0};
  std::atomic<std::uint64_t> emitted_sequence_{0};
  std::atomic<std::uint32_t> consumed_producer_{0};
  std::atomic<std::uint64_t> consumed_sequence_{0};
  std::atomic<bool> closed_{false};
};

} // namespace detail

} // namespace ayther::engine::audio_observation
