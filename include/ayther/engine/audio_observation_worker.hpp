#pragma once

#include <chrono>
#include <stop_token>
#include <thread>

namespace ayther::engine::audio_observation {

using ObservationDrainCallback = bool (*)(void *context) noexcept;

// Runs a host-owned drain callback away from observation producers. The host
// starts and joins this worker outside real-time callbacks. Serialization and
// I/O belong in the drain callback, never in queue producers.
class ObservationConsumerWorker {
public:
  ObservationConsumerWorker(void *context,
                            ObservationDrainCallback drain) noexcept
      : context_(context), drain_(drain) {}
  ObservationConsumerWorker(const ObservationConsumerWorker &) = delete;
  ObservationConsumerWorker &
  operator=(const ObservationConsumerWorker &) = delete;
  ObservationConsumerWorker(ObservationConsumerWorker &&) = delete;
  ObservationConsumerWorker &operator=(ObservationConsumerWorker &&) = delete;

  ~ObservationConsumerWorker() {
    request_stop();
    join();
  }

  [[nodiscard]] bool start() {
    if (drain_ == nullptr || worker_.joinable()) {
      return false;
    }
    worker_ =
        std::jthread{[this](std::stop_token stop) noexcept { run(stop); }};
    return true;
  }

  void request_stop() noexcept {
    if (worker_.joinable()) {
      worker_.request_stop();
    }
  }

  void join() noexcept {
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  [[nodiscard]] bool joinable() const noexcept { return worker_.joinable(); }

private:
  void run(std::stop_token stop) noexcept {
    while (!stop.stop_requested()) {
      if (!drain_(context_)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
    }
  }

  void *context_ = nullptr;
  ObservationDrainCallback drain_ = nullptr;
  std::jthread worker_{};
};

} // namespace ayther::engine::audio_observation
