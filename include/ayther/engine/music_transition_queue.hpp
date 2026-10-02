#pragma once

#include <ayther/engine/music_recognition_candidate.hpp>
#include <ayther/engine/music_sequence.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace ayther::engine {

struct QueuedMusicTransition {
  std::uint64_t event_id{};
  SequenceNodeId destination{};
  std::int32_t priority{};
  std::uint64_t generation{};
  AppearanceId appearance{};
  bool condition{};
};

enum class TransitionQueueStatus : std::uint8_t {
  none,
  queued,
  replaced_recent,
  replaced_priority,
  conflict,
  ignored,
  invalidated,
  timed_out,
  selected,
};

struct TransitionQueueResult {
  TransitionQueueStatus status{TransitionQueueStatus::none};
  SequenceNodeId destination{};
  std::string diagnostic;
};

class MusicTransitionQueue {
public:
  void begin_link() noexcept { link_active_ = true; }

  void cancel() noexcept {
    pending_.reset();
    link_active_ = false;
  }

  [[nodiscard]] TransitionQueueResult
  submit(QueuedMusicTransition request, std::uint64_t music_time_ns) {
    if (!link_active_)
      return {TransitionQueueStatus::ignored, {}, {}};
    if (!pending_) {
      pending_ = request;
      deadline_music_ns_ = music_time_ns + recognition_candidate_timeout_ns;
      return {TransitionQueueStatus::queued, request.destination, {}};
    }
    if (request.priority < pending_->priority)
      return {TransitionQueueStatus::ignored, pending_->destination, {}};
    if (request.priority == pending_->priority &&
        request.destination != pending_->destination)
      return {TransitionQueueStatus::conflict, {}, "transition_conflict"};
    const auto status = request.priority > pending_->priority
                            ? TransitionQueueStatus::replaced_priority
                            : TransitionQueueStatus::replaced_recent;
    pending_ = request;
    return {status, request.destination, {}};
  }

  [[nodiscard]] TransitionQueueResult
  finish_link(std::uint64_t generation, AppearanceId appearance,
              bool condition, std::uint64_t music_time_ns) {
    if (!link_active_ || !pending_)
      return {};
    link_active_ = false;
    const auto request = *pending_;
    pending_.reset();
    if (music_time_ns >= deadline_music_ns_)
      return {TransitionQueueStatus::timed_out, {}, "recognition_timeout"};
    if (request.generation != generation || request.appearance != appearance ||
        !request.condition || !condition)
      return {TransitionQueueStatus::invalidated, {}, {}};
    return {TransitionQueueStatus::selected, request.destination, {}};
  }

  [[nodiscard]] std::size_t pending_count() const noexcept {
    return pending_ ? 1U : 0U;
  }
  [[nodiscard]] std::uint64_t pending_event_id() const noexcept {
    return pending_ ? pending_->event_id : 0;
  }
  [[nodiscard]] std::uint64_t deadline_music_ns() const noexcept {
    return deadline_music_ns_;
  }

private:
  std::optional<QueuedMusicTransition> pending_;
  std::uint64_t deadline_music_ns_{};
  bool link_active_{};
};

} // namespace ayther::engine
