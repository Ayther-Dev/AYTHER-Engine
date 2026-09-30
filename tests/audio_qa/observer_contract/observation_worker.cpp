#include <ayther/engine/audio_fact_queue.hpp>
#include <ayther/engine/audio_observation_worker.hpp>

#include <atomic>
#include <chrono>
#include <thread>

namespace obs = ayther::engine::audio_observation;

namespace {

struct SlowConsumer {
  obs::BoundedFactQueue<2> queue;
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  std::atomic<bool> wrong_thread{false};
  std::atomic<unsigned> consumed{0};
  std::thread::id producer_thread{};

  static void consume(void *context, const obs::FactView &) noexcept {
    auto &self = *static_cast<SlowConsumer *>(context);
    if (std::this_thread::get_id() == self.producer_thread) {
      self.wrong_thread.store(true, std::memory_order_relaxed);
    }
    self.entered.store(true, std::memory_order_release);
    while (!self.release.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    self.consumed.fetch_add(1, std::memory_order_release);
  }

  static bool drain(void *context) noexcept {
    auto &self = *static_cast<SlowConsumer *>(context);
    return self.queue.try_consume(&self, consume);
  }
};

bool wait_until(const std::atomic<bool> &value) {
  for (unsigned attempt = 0; attempt < 2000; ++attempt) {
    if (value.load(std::memory_order_acquire)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return false;
}

bool wait_for_count(const std::atomic<unsigned> &value, unsigned expected) {
  for (unsigned attempt = 0; attempt < 2000; ++attempt) {
    if (value.load(std::memory_order_acquire) == expected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return false;
}

bool slow_consumer_does_not_block_producer() {
  SlowConsumer state;
  state.producer_thread = std::this_thread::get_id();
  obs::ObservationConsumerWorker worker{&state, SlowConsumer::drain};
  const obs::FactView first{{1, 1}, "first", {}, {}, {}, {}};
  const obs::FactView second{{1, 2}, "second", {}, {}, {}, {}};
  const obs::FactView third{{1, 3}, "third", {}, {}, {}, {}};

  if (state.queue.try_push(first) != obs::FactPushResult::accepted ||
      !worker.start() || !wait_until(state.entered)) {
    state.release.store(true, std::memory_order_release);
    return false;
  }

  // The worker is deliberately stuck inside the consumer callback. These
  // calls can complete only if the producer neither invokes nor waits for it.
  const auto second_result = state.queue.try_push(second);
  const auto third_result = state.queue.try_push(third);
  state.release.store(true, std::memory_order_release);
  const auto drained = wait_for_count(state.consumed, 2);
  worker.request_stop();
  worker.join();

  return second_result == obs::FactPushResult::accepted &&
         third_result == obs::FactPushResult::full && drained &&
         !state.wrong_thread.load(std::memory_order_relaxed) &&
         !worker.joinable();
}

} // namespace

int main() { return slow_consumer_does_not_block_producer() ? 0 : 1; }
