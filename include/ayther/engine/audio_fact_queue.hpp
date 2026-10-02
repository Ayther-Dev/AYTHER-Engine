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

enum class FactPushResult : std::uint8_t { accepted, full, invalid, closed };

namespace detail {

template <std::size_t MaxCauses = max_causes,
          std::size_t MaxStateOrders = max_state_orders,
          std::size_t MaxFields = max_fields,
          std::size_t MaxTextBytes = max_text_bytes>
struct OwnedFactSlot {
  FactId id{};
  FramePosition frame{};
  std::array<Cause, MaxCauses> causes{};
  std::array<StateOrder, MaxStateOrders> state_orders{};
  std::array<FieldView, MaxFields> fields{};
  std::array<char, MaxTextBytes> text{};
  std::size_t cause_count = 0;
  std::size_t state_order_count = 0;
  std::size_t field_count = 0;
  std::size_t text_size = 0;
  std::string_view kind{};

  [[nodiscard]] bool assign(const FactView &source) noexcept {
    if (source.causes.size() > causes.size() ||
        source.state_orders.size() > state_orders.size() ||
        source.fields.size() > fields.size()) {
      return false;
    }

    text_size = 0;
    id = source.id;
    frame = source.frame;
    cause_count = source.causes.size();
    state_order_count = source.state_orders.size();
    field_count = source.fields.size();
    if (!copy_text(source.kind, kind) ||
        !copy_text(source.frame.unavailable_reason, frame.unavailable_reason)) {
      return false;
    }

    for (std::size_t index = 0; index < cause_count; ++index) {
      causes[index] = source.causes[index];
      if (auto *context = std::get_if<PreexistingContext>(&causes[index]);
          context != nullptr &&
          !copy_text(context->state_id, context->state_id)) {
        return false;
      }
    }
    for (std::size_t index = 0; index < state_order_count; ++index) {
      state_orders[index] = source.state_orders[index];
      if (!copy_text(source.state_orders[index].state_id,
                     state_orders[index].state_id)) {
        return false;
      }
    }
    for (std::size_t index = 0; index < field_count; ++index) {
      fields[index] = source.fields[index];
      if (!copy_text(source.fields[index].name, fields[index].name) ||
          !copy_text(source.fields[index].unavailable_reason,
                     fields[index].unavailable_reason)) {
        return false;
      }
      if (const auto *value =
              std::get_if<std::string_view>(&source.fields[index].value);
          value != nullptr) {
        std::string_view owned_value;
        if (!copy_text(*value, owned_value)) {
          return false;
        }
        fields[index].value = owned_value;
      }
    }
    return true;
  }

  [[nodiscard]] FactView view() const noexcept {
    return {id,
            kind,
            frame,
            std::span{causes}.first(cause_count),
            std::span{state_orders}.first(state_order_count),
            std::span{fields}.first(field_count)};
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

// Fixed-capacity single-producer/single-consumer handoff. The producer owns
// every nested view before publishing and never allocates, waits, or does I/O.
// The consumed view remains valid only for the synchronous callback.
template <std::size_t Capacity, std::size_t MaxCauses = max_causes,
          std::size_t MaxStateOrders = max_state_orders,
          std::size_t MaxFields = max_fields,
          std::size_t MaxTextBytes = max_text_bytes>
class BoundedFactQueue {
  static_assert(Capacity > 0 && MaxCauses > 0 && MaxFields > 0 &&
                MaxTextBytes > 0);
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

public:
  explicit BoundedFactQueue(
      ObservationOverflowCounter *overflow = nullptr) noexcept
      : overflow_(overflow) {}
  BoundedFactQueue(const BoundedFactQueue &) = delete;
  BoundedFactQueue &operator=(const BoundedFactQueue &) = delete;
  BoundedFactQueue(BoundedFactQueue &&) = delete;
  BoundedFactQueue &operator=(BoundedFactQueue &&) = delete;
  ~BoundedFactQueue() = default;

  [[nodiscard]] FactPushResult try_push(const FactView &fact) noexcept {
    if (progress_.closed()) {
      return FactPushResult::closed;
    }
    progress_.record_emitted(fact.id);
    const auto write = write_sequence_.load(std::memory_order_relaxed);
    const auto read = read_sequence_.load(std::memory_order_acquire);
    if (write - read >= Capacity) {
      if (overflow_ != nullptr) {
        overflow_->record(fact);
      }
      return FactPushResult::full;
    }
    auto &slot = slots_[static_cast<std::size_t>(write % Capacity)];
    if (!slot.assign(fact)) {
      return FactPushResult::invalid;
    }
    write_sequence_.store(write + 1, std::memory_order_release);
    return FactPushResult::accepted;
  }

  [[nodiscard]] bool try_consume(void *context,
                                 FactCallback callback) noexcept {
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
  std::array<
      detail::OwnedFactSlot<MaxCauses, MaxStateOrders, MaxFields, MaxTextBytes>,
      Capacity>
      slots_{};
  ObservationOverflowCounter *overflow_ = nullptr;
  detail::ObservationQueueProgress progress_{};
  alignas(64) std::atomic<std::uint64_t> write_sequence_{0};
  alignas(64) std::atomic<std::uint64_t> read_sequence_{0};
};

} // namespace ayther::engine::audio_observation
