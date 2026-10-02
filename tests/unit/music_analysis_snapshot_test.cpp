#include <ayther/engine/music_analysis_snapshot.hpp>

#include <cstdio>
#include <atomic>
#include <thread>

namespace {
void check(bool value, const char *message, int &failures) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}
} // namespace

int main() {
  using namespace ayther::engine;
  int failures = 0;

  AnalysisInputs inputs;
  inputs.take = "golden-axe-a18-part-1";
  inputs.events = {{EventId{1}, 10, 2, 10, 1, 60,
                    EventProvenance{"take", 2, 1}}};
  inputs.range = {0, 100};
  inputs.catalog_revision = 3;
  inputs.asset_revision = 4;
  inputs.point_revision = 5;
  inputs.parameters = {.minimum_score = 0.90, .frame_tolerance = 2};
  inputs.format_version = 1;
  inputs.authored_revision = 7;

  AnalysisCoordinator coordinator;
  const auto token = coordinator.begin(inputs);
  check(token.has_value(), "the first request starts", failures);
  inputs.events.clear();
  inputs.catalog_revision = 99;
  const auto snapshot = coordinator.snapshot(*token);
  check(snapshot && snapshot->events.size() == 1 &&
            snapshot->catalog_revision == 3 && snapshot->asset_revision == 4 &&
            snapshot->point_revision == 5 && snapshot->format_version == 1,
        "request inputs are fixed by value", failures);

  check(coordinator.cancel(*token), "cancellation invalidates the token", failures);
  check(coordinator.publish(*token, {{ProposalId{1}, "loop"}}) ==
            PublishResult::cancelled,
        "a late completion cannot publish after cancellation", failures);

  const auto next = coordinator.begin(inputs);
  check(next.has_value(), "a request can start after cancellation", failures);
  check(coordinator.publish(*next, {{ProposalId{2}, "intro"}}) ==
            PublishResult::published,
        "completion wins when it is effective before cancellation", failures);
  check(!coordinator.cancel(*next),
        "late cancellation cannot rewrite a published result", failures);

  coordinator.accept(AuthorDecision{ProposalId{2}, 11});
  check(coordinator.query(*next, 7).state == AnalysisResultState::current,
        "same authored revision is current", failures);
  check(coordinator.query(*next, 8).state == AnalysisResultState::stale,
        "editing a dependency makes the result stale", failures);
  check(coordinator.accepted_decisions().size() == 1 &&
            coordinator.accepted_decisions()[0].decision_revision == 11,
        "staleness does not erase accepted author decisions", failures);

  for (int iteration = 0; iteration < 32; ++iteration) {
    const auto racing = coordinator.begin(inputs);
    check(racing.has_value(), "race fixture starts", failures);
    if (!racing)
      break;
    std::atomic<bool> go{false};
    bool cancelled = false;
    PublishResult published = PublishResult::obsolete;
    std::thread cancel_thread([&] {
      while (!go.load(std::memory_order_acquire)) {
      }
      cancelled = coordinator.cancel(*racing);
    });
    std::thread publish_thread([&] {
      while (!go.load(std::memory_order_acquire)) {
      }
      published = coordinator.publish(*racing, {{ProposalId{3}, "race"}});
    });
    go.store(true, std::memory_order_release);
    cancel_thread.join();
    publish_thread.join();
    check((cancelled && published == PublishResult::cancelled) ||
              (!cancelled && published == PublishResult::published),
          "completion and cancellation have one effective winner", failures);
  }

  return failures == 0 ? 0 : 1;
}
