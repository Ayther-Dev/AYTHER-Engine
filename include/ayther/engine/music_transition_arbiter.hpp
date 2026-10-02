#pragma once

#include <ayther/engine/music_sequence.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ayther::engine {

enum class OutputAuthority : std::uint8_t {
  hd_region,
  game_pattern,
  explicit_order,
};

struct MusicTransitionRequest {
  std::uint64_t event_id{};
  SequenceNodeId destination{};
  std::int32_t priority{};
  OutputAuthority authority{OutputAuthority::game_pattern};
};

enum class TransitionChoiceStatus : std::uint8_t {
  none,
  selected,
  conflict,
  already_consumed,
  delegate_to_mix,
};

struct MusicTransitionChoice {
  TransitionChoiceStatus status{TransitionChoiceStatus::none};
  SequenceNodeId destination{};
  OutputAuthority authority{OutputAuthority::game_pattern};
  std::string diagnostic;
};

enum class MusicBoundary : std::uint8_t {
  restore_or_session_close,
  explicit_cancel_restart_replace,
  pause_change,
  internal_transition,
  loop_or_exhaustion,
  new_entry,
};

[[nodiscard]] constexpr bool boundary_precedes(MusicBoundary left,
                                               MusicBoundary right) noexcept {
  return static_cast<std::uint8_t>(left) < static_cast<std::uint8_t>(right);
}

class MusicTransitionArbiter {
public:
  [[nodiscard]] MusicTransitionChoice
  choose(const std::vector<MusicTransitionRequest> &requests) {
    std::vector<const MusicTransitionRequest *> eligible;
    for (const auto &request : requests)
      if (std::ranges::find(consumed_events_, request.event_id) ==
          consumed_events_.end())
        eligible.push_back(&request);
    if (eligible.empty())
      return {requests.empty() ? TransitionChoiceStatus::none
                               : TransitionChoiceStatus::already_consumed,
              {},
              OutputAuthority::game_pattern,
              {}};
    const auto highest =
        (*std::ranges::max_element(eligible, {}, [](const auto *request) {
          return request->priority;
        }))->priority;
    const MusicTransitionRequest *winner = nullptr;
    for (const auto *request : eligible) {
      if (request->priority != highest)
        continue;
      if (winner && winner->destination != request->destination)
        return {TransitionChoiceStatus::conflict,
                {},
                OutputAuthority::game_pattern,
                "transition_conflict"};
      winner = request;
    }
    if (!winner)
      return {};
    consumed_events_.push_back(winner->event_id);
    return {TransitionChoiceStatus::selected,
            winner->destination,
            winner->authority,
            {}};
  }

  void set_pending(MusicTransitionRequest request) {
    pending_ = std::move(request);
  }
  [[nodiscard]] const std::optional<MusicTransitionRequest> &
  pending() const noexcept {
    return pending_;
  }
  void invalidate(MusicBoundary boundary) noexcept {
    if (boundary == MusicBoundary::restore_or_session_close ||
        boundary == MusicBoundary::explicit_cancel_restart_replace)
      pending_.reset();
  }
  [[nodiscard]] MusicTransitionChoice source_exhausted() const {
    return {TransitionChoiceStatus::delegate_to_mix,
            {},
            OutputAuthority::hd_region,
            "source_exhausted_before_transition"};
  }

private:
  std::vector<std::uint64_t> consumed_events_;
  std::optional<MusicTransitionRequest> pending_;
};

} // namespace ayther::engine
