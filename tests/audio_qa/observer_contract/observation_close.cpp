#include <ayther/engine/audio_fact_queue.hpp>
#include <ayther/engine/audio_pcm_queue.hpp>

#include <array>
#include <cstddef>

namespace obs = ayther::engine::audio_observation;

namespace {

void consume_fact(void *, const obs::FactView &) noexcept {}
void consume_pcm(void *, const obs::PcmView &) noexcept {}

bool complete_fact_close() {
  obs::ObservationOverflowCounter overflow;
  obs::BoundedFactQueue<2> queue{&overflow};
  const obs::FactView first{{2, 1}, "first", {}, {}, {}, {}};
  const obs::FactView second{{2, 2}, "second", {}, {}, {}, {}};
  if (queue.try_push(first) != obs::FactPushResult::accepted ||
      queue.try_push(second) != obs::FactPushResult::accepted) {
    return false;
  }
  queue.close();
  const auto before_drain = queue.close_snapshot();
  if (!before_drain.closed || before_drain.last_emitted != second.id ||
      before_drain.last_consumed.sequence != 0 || before_drain.complete()) {
    return false;
  }
  if (!queue.try_consume(nullptr, consume_fact) ||
      !queue.try_consume(nullptr, consume_fact)) {
    return false;
  }
  const auto closed = queue.close_snapshot();
  return closed.closed && closed.last_emitted == second.id &&
         closed.last_consumed == second.id && closed.overflow_count == 0 &&
         closed.complete() &&
         queue.try_push({{2, 3}, "late", {}, {}, {}, {}}) ==
             obs::FactPushResult::closed;
}

bool detects_final_fact_loss() {
  obs::ObservationOverflowCounter overflow;
  obs::BoundedFactQueue<1> queue{&overflow};
  const obs::FactView retained{{3, 10}, "retained", {}, {}, {}, {}};
  const obs::FactView lost{{3, 11}, "lost", {}, {}, {}, {}};
  if (queue.try_push(retained) != obs::FactPushResult::accepted ||
      queue.try_push(lost) != obs::FactPushResult::full) {
    return false;
  }
  queue.close();
  if (!queue.try_consume(nullptr, consume_fact)) {
    return false;
  }
  const auto closed = queue.close_snapshot();
  return closed.closed && closed.last_emitted == lost.id &&
         closed.last_consumed == retained.id && closed.overflow_count == 1 &&
         !closed.complete();
}

bool complete_pcm_close() {
  obs::ObservationOverflowCounter overflow;
  obs::BoundedPcmQueue<1> queue{&overflow};
  const std::array<std::byte, 4> bytes{};
  const obs::PcmView pcm{
      {4, 20}, "postmix", {"output", 44100, 0, 1}, obs::PcmFormat::s16_le, 2,
      bytes,   {}};
  if (queue.try_push(pcm) != obs::PcmPushResult::accepted) {
    return false;
  }
  queue.close();
  if (!queue.try_consume(nullptr, consume_pcm)) {
    return false;
  }
  const auto closed = queue.close_snapshot();
  return closed.complete() && closed.last_emitted == pcm.id &&
         closed.last_consumed == pcm.id &&
         queue.try_push(pcm) == obs::PcmPushResult::closed;
}

} // namespace

int main() {
  return complete_fact_close() && detects_final_fact_loss() &&
                 complete_pcm_close()
             ? 0
             : 1;
}
