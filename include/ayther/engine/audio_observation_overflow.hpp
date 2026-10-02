#pragma once

#include <ayther/engine/audio_observer.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

namespace ayther::engine::audio_observation {

enum class ObservationStream : std::uint8_t { fact, pcm };

struct ObservationOverflowSnapshot {
  std::uint64_t count = 0;
  bool has_first = false;
  ObservationStream first_stream = ObservationStream::fact;
  FactId first_fact{};
  SampleRange first_pcm_range{};
  bool first_timeline_complete = true;
};

// Single-producer overflow state stored outside the saturated payload queue.
// The first affected item is immutable after the release publication of count.
class ObservationOverflowCounter {
public:
  ObservationOverflowCounter() = default;
  ObservationOverflowCounter(const ObservationOverflowCounter &) = delete;
  ObservationOverflowCounter &
  operator=(const ObservationOverflowCounter &) = delete;
  ObservationOverflowCounter(ObservationOverflowCounter &&) = delete;
  ObservationOverflowCounter &operator=(ObservationOverflowCounter &&) = delete;
  ~ObservationOverflowCounter() = default;

  void record(const FactView &fact) noexcept {
    const auto count = count_.load(std::memory_order_relaxed);
    if (count == 0) {
      first_stream_ = ObservationStream::fact;
      first_fact_ = fact.id;
      first_pcm_range_ = {};
      first_timeline_complete_ = true;
    }
    publish_increment(count);
  }

  void record(const PcmView &pcm) noexcept {
    const auto count = count_.load(std::memory_order_relaxed);
    if (count == 0) {
      first_stream_ = ObservationStream::pcm;
      first_fact_ = {};
      first_pcm_range_ = pcm.range;
      if (pcm.range.timeline.size() <= first_timeline_.size()) {
        if (!pcm.range.timeline.empty()) {
          std::memcpy(first_timeline_.data(), pcm.range.timeline.data(),
                      pcm.range.timeline.size());
        }
        first_pcm_range_.timeline = {first_timeline_.data(),
                                     pcm.range.timeline.size()};
        first_timeline_complete_ = true;
      } else {
        first_pcm_range_.timeline = {};
        first_timeline_complete_ = false;
      }
    }
    publish_increment(count);
  }

  [[nodiscard]] ObservationOverflowSnapshot snapshot() const noexcept {
    const auto count = count_.load(std::memory_order_acquire);
    if (count == 0) {
      return {};
    }
    return {count,
            true,
            first_stream_,
            first_fact_,
            first_pcm_range_,
            first_timeline_complete_};
  }

private:
  void publish_increment(std::uint64_t count) noexcept {
    if (count != std::numeric_limits<std::uint64_t>::max()) {
      count_.store(count + 1, std::memory_order_release);
    }
  }

  std::array<char, max_text_bytes> first_timeline_{};
  ObservationStream first_stream_ = ObservationStream::fact;
  FactId first_fact_{};
  SampleRange first_pcm_range_{};
  bool first_timeline_complete_ = true;
  alignas(64) std::atomic<std::uint64_t> count_{0};
};

} // namespace ayther::engine::audio_observation
