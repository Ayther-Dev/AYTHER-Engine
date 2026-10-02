#pragma once

#include <ayther/engine/music_event_normalization.hpp>
#include <ayther/engine/music_sequence.hpp>
#include <ayther/engine/music_sequence_validation.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ayther::engine {

struct AnalysisParameters {
  double minimum_score{};
  std::uint32_t frame_tolerance{};
};

struct AnalysisInputs {
  std::string take;
  std::vector<MusicEvent> events;
  EventRange range{};
  std::vector<MusicIdentityId> catalog;
  SequenceAssetCatalog assets;
  std::vector<SequencePoint> points;
  std::uint64_t catalog_revision{};
  std::uint64_t asset_revision{};
  std::uint64_t point_revision{};
  AnalysisParameters parameters{};
  std::uint32_t format_version{};
  std::uint64_t authored_revision{};
};

struct AnalysisToken {
  std::uint64_t value{};
  friend constexpr bool operator==(AnalysisToken, AnalysisToken) noexcept = default;
};

struct ProposalId {
  std::uint64_t value{};
  friend constexpr bool operator==(ProposalId, ProposalId) noexcept = default;
};

struct AnalysisProposal {
  ProposalId id{};
  std::string kind;
};

struct AuthorDecision {
  ProposalId proposal{};
  std::uint64_t decision_revision{};
};

enum class PublishResult : std::uint8_t { published, cancelled, obsolete };
enum class AnalysisResultState : std::uint8_t {
  missing,
  cancelled,
  current,
  stale,
};

struct AnalysisQuery {
  AnalysisResultState state{AnalysisResultState::missing};
  std::vector<AnalysisProposal> proposals;
};

class AnalysisCoordinator {
public:
  [[nodiscard]] std::optional<AnalysisToken> begin(AnalysisInputs inputs) {
    const std::scoped_lock lock{mutex_};
    if (active_)
      return std::nullopt;
    const AnalysisToken token{++serial_};
    active_ = ActiveRequest{token, std::move(inputs), false};
    return token;
  }

  [[nodiscard]] std::optional<AnalysisInputs>
  snapshot(AnalysisToken token) const {
    const std::scoped_lock lock{mutex_};
    if (!active_ || active_->token != token)
      return std::nullopt;
    return active_->inputs;
  }

  [[nodiscard]] bool cancel(AnalysisToken token) {
    const std::scoped_lock lock{mutex_};
    if (!active_ || active_->token != token)
      return false;
    cancelled_token_ = token;
    active_.reset();
    return true;
  }

  [[nodiscard]] PublishResult
  publish(AnalysisToken token, std::vector<AnalysisProposal> proposals) {
    const std::scoped_lock lock{mutex_};
    if (cancelled_token_ && *cancelled_token_ == token)
      return PublishResult::cancelled;
    if (!active_ || active_->token != token)
      return PublishResult::obsolete;
    published_ = PublishedResult{token, active_->inputs.authored_revision,
                                 std::move(proposals)};
    active_.reset();
    return PublishResult::published;
  }

  [[nodiscard]] AnalysisQuery query(AnalysisToken token,
                                    std::uint64_t authored_revision) const {
    const std::scoped_lock lock{mutex_};
    if (cancelled_token_ && *cancelled_token_ == token)
      return {AnalysisResultState::cancelled, {}};
    if (!published_ || published_->token != token)
      return {};
    return {published_->authored_revision == authored_revision
                ? AnalysisResultState::current
                : AnalysisResultState::stale,
            published_->proposals};
  }

  void accept(AuthorDecision decision) {
    const std::scoped_lock lock{mutex_};
    accepted_.push_back(decision);
  }

  [[nodiscard]] std::vector<AuthorDecision> accepted_decisions() const {
    const std::scoped_lock lock{mutex_};
    return accepted_;
  }

private:
  struct ActiveRequest {
    AnalysisToken token{};
    AnalysisInputs inputs;
    bool cancellation_requested{};
  };
  struct PublishedResult {
    AnalysisToken token{};
    std::uint64_t authored_revision{};
    std::vector<AnalysisProposal> proposals;
  };

  mutable std::mutex mutex_;
  std::uint64_t serial_{};
  std::optional<ActiveRequest> active_;
  std::optional<AnalysisToken> cancelled_token_;
  std::optional<PublishedResult> published_;
  std::vector<AuthorDecision> accepted_;
};

} // namespace ayther::engine
