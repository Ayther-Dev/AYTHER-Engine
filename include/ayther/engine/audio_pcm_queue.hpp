#pragma once

#include <ayther/engine/audio_observation_close.hpp>
#include <ayther/engine/audio_observation_overflow.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <variant>

namespace ayther::engine::audio_observation {

enum class PcmPushResult : std::uint8_t { accepted, full, invalid, closed };

namespace detail {

template <std::size_t MaxPcmBytes> struct OwnedPcmSlot {
  FactId id{};
  SampleRange range{};
  PcmFormat format = PcmFormat::s16_le;
  std::uint16_t channels = 0;
  std::array<std::byte, MaxPcmBytes> bytes{};
  std::array<Cause, max_causes> causes{};
  std::array<char, max_text_bytes> text{};
  std::size_t byte_count = 0;
  std::size_t cause_count = 0;
  std::size_t text_size = 0;
  std::string_view capture_point{};

  [[nodiscard]] bool assign(const PcmView &source) noexcept {
    if (source.bytes.size() > bytes.size() ||
        source.causes.size() > causes.size()) {
      return false;
    }

    text_size = 0;
    id = source.id;
    range = source.range;
    format = source.format;
    channels = source.channels;
    byte_count = source.bytes.size();
    cause_count = source.causes.size();
    if (!copy_text(source.capture_point, capture_point) ||
        !copy_text(source.range.timeline, range.timeline)) {
      return false;
    }
    if (!source.bytes.empty()) {
      std::memcpy(bytes.data(), source.bytes.data(), source.bytes.size());
    }
    for (std::size_t index = 0; index < cause_count; ++index) {
      causes[index] = source.causes[index];
      if (auto *context = std::get_if<PreexistingContext>(&causes[index]);
          context != nullptr &&
          !copy_text(context->state_id, context->state_id)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] PcmView view() const noexcept {
    return {id,
            capture_point,
            range,
            format,
            channels,
            std::span{bytes}.first(byte_count),
            std::span{causes}.first(cause_count)};
  }

private:
  [[nodiscard]] bool copy_text(std::string_view source,
                               std::string_view &destination) noexcept {
    if (source.size() > text.size() - text_size) {
      return false;
    }
    const auto begin = text_size;
    if (!source.empty()) {
      std::memcpy(text.data() + begin, source.data(), source.size());
    }
    text_size += source.size();
    destination = {text.data() + begin, source.size()};
    return true;
  }
};

} // namespace detail

// Fixed-capacity single-producer/single-consumer PCM handoff. The producer
// copies bytes and metadata into preallocated slots before publication. It
// never allocates, waits, serializes, or performs I/O.
template <std::size_t Capacity, std::size_t MaxPcmBytes = max_pcm_bytes>
class BoundedPcmQueue {
  static_assert(Capacity > 0);
  static_assert(MaxPcmBytes > 0 && MaxPcmBytes <= max_pcm_bytes);
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

public:
  explicit BoundedPcmQueue(
      ObservationOverflowCounter *overflow = nullptr) noexcept
      : overflow_(overflow) {}
  BoundedPcmQueue(const BoundedPcmQueue &) = delete;
  BoundedPcmQueue &operator=(const BoundedPcmQueue &) = delete;
  BoundedPcmQueue(BoundedPcmQueue &&) = delete;
  BoundedPcmQueue &operator=(BoundedPcmQueue &&) = delete;
  ~BoundedPcmQueue() = default;

  [[nodiscard]] PcmPushResult try_push(const PcmView &pcm) noexcept {
    if (progress_.closed()) {
      return PcmPushResult::closed;
    }
    progress_.record_emitted(pcm.id);
    const auto write = write_sequence_.load(std::memory_order_relaxed);
    const auto read = read_sequence_.load(std::memory_order_acquire);
    if (write - read >= Capacity) {
      if (overflow_ != nullptr) {
        overflow_->record(pcm);
      }
      return PcmPushResult::full;
    }
    auto &slot = slots_[static_cast<std::size_t>(write % Capacity)];
    if (!slot.assign(pcm)) {
      return PcmPushResult::invalid;
    }
    write_sequence_.store(write + 1, std::memory_order_release);
    return PcmPushResult::accepted;
  }

  [[nodiscard]] bool try_consume(void *context, PcmCallback callback) noexcept {
    if (callback == nullptr) {
      return false;
    }
    const auto read = read_sequence_.load(std::memory_order_relaxed);
    const auto write = write_sequence_.load(std::memory_order_acquire);
    if (read == write) {
      return false;
    }
    const auto &slot = slots_[static_cast<std::size_t>(read % Capacity)];
    const auto view = slot.view();
    callback(context, view);
    progress_.record_consumed(view.id);
    read_sequence_.store(read + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] static constexpr std::size_t capacity() noexcept {
    return Capacity;
  }

  void close() noexcept { progress_.close(); }

  [[nodiscard]] ObservationQueueCloseSnapshot close_snapshot() const noexcept {
    const auto overflow_count =
        overflow_ == nullptr ? 0 : overflow_->snapshot().count;
    return progress_.snapshot(overflow_count);
  }

private:
  std::array<detail::OwnedPcmSlot<MaxPcmBytes>, Capacity> slots_{};
  ObservationOverflowCounter *overflow_ = nullptr;
  detail::ObservationQueueProgress progress_{};
  alignas(64) std::atomic<std::uint64_t> write_sequence_{0};
  alignas(64) std::atomic<std::uint64_t> read_sequence_{0};
};

} // namespace ayther::engine::audio_observation
