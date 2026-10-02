#include <ayther/engine/audio_fact_queue.hpp>
#include <ayther/engine/audio_pcm_queue.hpp>

#include <array>
#include <cstddef>

namespace obs = ayther::engine::audio_observation;

namespace {

bool preserves_fact_overflow() {
  obs::ObservationOverflowCounter overflow;
  obs::BoundedFactQueue<1> queue{&overflow};
  const obs::FactView first{{3, 10}, "first", {}, {}, {}, {}};
  const obs::FactView second{{3, 11}, "second", {}, {}, {}, {}};
  const obs::FactView third{{3, 12}, "third", {}, {}, {}, {}};

  if (queue.try_push(first) != obs::FactPushResult::accepted ||
      queue.try_push(second) != obs::FactPushResult::full ||
      queue.try_push(third) != obs::FactPushResult::full) {
    return false;
  }
  const auto loss = overflow.snapshot();
  return loss.count == 2 && loss.has_first &&
         loss.first_stream == obs::ObservationStream::fact &&
         loss.first_fact == second.id;
}

bool preserves_pcm_overflow_range() {
  obs::ObservationOverflowCounter overflow;
  obs::BoundedPcmQueue<1> queue{&overflow};
  const std::array<std::byte, 4> bytes{};
  const obs::PcmView first{{4, 20},
                           "postmix",
                           {"engine_main_output", 44100, 0, 8},
                           obs::PcmFormat::s16_le,
                           2,
                           bytes,
                           {}};
  const obs::PcmView second{{4, 21},
                            "postmix",
                            {"engine_main_output", 44100, 8, 16},
                            obs::PcmFormat::s16_le,
                            2,
                            bytes,
                            {}};
  const obs::PcmView third{{4, 22},
                           "postmix",
                           {"engine_main_output", 44100, 16, 24},
                           obs::PcmFormat::s16_le,
                           2,
                           bytes,
                           {}};

  if (queue.try_push(first) != obs::PcmPushResult::accepted ||
      queue.try_push(second) != obs::PcmPushResult::full ||
      queue.try_push(third) != obs::PcmPushResult::full) {
    return false;
  }
  const auto loss = overflow.snapshot();
  return loss.count == 2 && loss.has_first &&
         loss.first_stream == obs::ObservationStream::pcm &&
         loss.first_pcm_range.timeline == "engine_main_output" &&
         loss.first_pcm_range.sample_rate == 44100 &&
         loss.first_pcm_range.begin == 8 && loss.first_pcm_range.end == 16 &&
         loss.first_timeline_complete;
}

} // namespace

int main() {
  return preserves_fact_overflow() && preserves_pcm_overflow_range() ? 0 : 1;
}
