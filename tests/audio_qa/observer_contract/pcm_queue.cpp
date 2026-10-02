#include <ayther/engine/audio_pcm_queue.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace obs = ayther::engine::audio_observation;

namespace {

struct Consumer {
  std::array<std::uint64_t, 3> sequences{};
  std::array<obs::PcmFormat, 3> formats{};
  std::array<std::uint64_t, 3> begins{};
  std::array<std::byte, 3> first_bytes{};
  std::array<std::string, 3> timelines{};
  std::array<std::string, 3> contexts{};
  std::size_t count = 0;
  bool valid = true;

  static void receive(void *context, const obs::PcmView &pcm) noexcept {
    auto &self = *static_cast<Consumer *>(context);
    try {
      if (self.count >= self.sequences.size() || pcm.bytes.empty() ||
          pcm.causes.size() != 1) {
        self.valid = false;
        return;
      }
      const auto *initial =
          std::get_if<obs::PreexistingContext>(&pcm.causes[0]);
      if (initial == nullptr) {
        self.valid = false;
        return;
      }
      self.sequences[self.count] = pcm.id.sequence;
      self.formats[self.count] = pcm.format;
      self.begins[self.count] = pcm.range.begin;
      self.first_bytes[self.count] = pcm.bytes.front();
      self.timelines[self.count] = pcm.range.timeline;
      self.contexts[self.count] = initial->state_id;
      ++self.count;
    } catch (...) {
      self.valid = false;
    }
  }
};

struct PcmLabels {
  std::string_view timeline;
  std::string_view context;
};

obs::PcmView make_pcm(std::uint64_t sequence, std::uint64_t begin,
                      obs::PcmFormat format, const PcmLabels labels,
                      std::span<const std::byte> bytes,
                      std::array<obs::Cause, 1> &causes) {
  causes = {obs::PreexistingContext{labels.context}};
  return {{4, sequence}, "postmix", {labels.timeline, 44100, begin, begin + 2},
          format,        2,         bytes,
          causes};
}

bool bounded_owned_and_ordered() {
  obs::BoundedPcmQueue<2> queue;
  Consumer consumer;
  std::array<obs::Cause, 1> causes{};
  std::array<std::byte, 4> bytes{std::byte{1}, std::byte{2}, std::byte{3},
                                 std::byte{4}};
  std::string timeline = "engine_main_output";
  std::string context = "fresh_audio";
  const auto first = make_pcm(1, 0, obs::PcmFormat::s16_le, {timeline, context},
                              bytes, causes);
  if (queue.try_push(first) != obs::PcmPushResult::accepted) {
    return false;
  }
  bytes.fill(std::byte{0});
  timeline.assign(timeline.size(), 'x');
  context.assign(context.size(), 'x');

  const std::array<std::byte, 4> second_bytes{std::byte{5}, std::byte{6},
                                              std::byte{7}, std::byte{8}};
  const std::array<std::byte, 4> third_bytes{std::byte{9}, std::byte{10},
                                             std::byte{11}, std::byte{12}};
  const auto second = make_pcm(2, 2, obs::PcmFormat::f32_le,
                               {"output", "mixer"}, second_bytes, causes);
  const auto third = make_pcm(3, 4, obs::PcmFormat::s24_le, {"output", "mixer"},
                              third_bytes, causes);
  if (queue.try_push(second) != obs::PcmPushResult::accepted ||
      queue.try_push(third) != obs::PcmPushResult::full ||
      !queue.try_consume(&consumer, Consumer::receive)) {
    return false;
  }
  if (queue.try_push(third) != obs::PcmPushResult::accepted ||
      !queue.try_consume(&consumer, Consumer::receive) ||
      !queue.try_consume(&consumer, Consumer::receive) ||
      queue.try_consume(&consumer, Consumer::receive)) {
    return false;
  }

  const std::array<std::uint64_t, 3> expected_sequences{1, 2, 3};
  return consumer.valid && consumer.count == 3 &&
         consumer.sequences == expected_sequences &&
         consumer.formats[0] == obs::PcmFormat::s16_le &&
         consumer.formats[1] == obs::PcmFormat::f32_le &&
         consumer.formats[2] == obs::PcmFormat::s24_le &&
         consumer.begins == std::array<std::uint64_t, 3>{0, 2, 4} &&
         consumer.first_bytes == std::array<std::byte, 3>{std::byte{1},
                                                          std::byte{5},
                                                          std::byte{9}} &&
         consumer.timelines[0] == "engine_main_output" &&
         consumer.contexts[0] == "fresh_audio";
}

struct CountConsumer {
  std::size_t count = 0;
  static void receive(void *context, const obs::PcmView &) noexcept {
    ++static_cast<CountConsumer *>(context)->count;
  }
};

bool rejects_oversized_without_consuming_capacity() {
  obs::BoundedPcmQueue<1> queue;
  std::vector<std::byte> oversized(obs::max_pcm_bytes + 1, std::byte{1});
  const obs::PcmView invalid{
      {1, 1},    "postmix", {"output", 44100, 0, 1}, obs::PcmFormat::s16_le, 2,
      oversized, {}};
  const std::array<std::byte, 4> bytes{};
  const obs::PcmView valid{
      {1, 2}, "postmix", {"output", 44100, 0, 1}, obs::PcmFormat::s16_le, 2,
      bytes,  {}};
  CountConsumer consumer;
  return queue.try_push(invalid) == obs::PcmPushResult::invalid &&
         queue.try_push(valid) == obs::PcmPushResult::accepted &&
         queue.try_consume(&consumer, CountConsumer::receive) &&
         consumer.count == 1;
}

bool supports_smaller_per_slot_payload_limits() {
  obs::BoundedPcmQueue<1, 8> queue;
  const std::array<std::byte, 9> oversized{};
  const std::array<std::byte, 8> accepted{};
  const auto make_view = [](const std::uint64_t sequence,
                            const std::span<const std::byte> bytes) {
    return obs::PcmView{{1, sequence},
                        "postmix",
                        {"output", 44100, sequence - 1, sequence},
                        obs::PcmFormat::f32_le,
                        2,
                        bytes,
                        {}};
  };
  CountConsumer consumer;
  return queue.try_push(make_view(1, oversized)) ==
             obs::PcmPushResult::invalid &&
         queue.try_push(make_view(2, accepted)) ==
             obs::PcmPushResult::accepted &&
         queue.try_consume(&consumer, CountConsumer::receive) &&
         consumer.count == 1;
}

} // namespace

int main() { // NOLINT(bugprone-exception-escape) -- Test allocation failure is
             // fatal.
  static_assert(obs::BoundedPcmQueue<2>::capacity() == 2);
  return bounded_owned_and_ordered() &&
                 rejects_oversized_without_consuming_capacity() &&
                 supports_smaller_per_slot_payload_limits()
             ? 0
             : 1;
}
