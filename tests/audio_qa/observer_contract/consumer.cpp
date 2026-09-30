#include <ayther/engine/audio_observer.hpp>

#include <array>

namespace obs = ayther::engine::audio_observation;

namespace {
struct Consumer {
  unsigned facts = 0;
  unsigned blocks = 0;
  bool valid = true;

  static void fact(void *context, const obs::FactView &value) noexcept {
    auto &self = *static_cast<Consumer *>(context);
    ++self.facts;
    self.valid = self.valid && value.id == obs::FactId{1, 1} &&
                 value.kind == "playback_started" && value.causes.size() == 2 &&
                 value.fields.size() == 2 && value.state_orders.size() == 1;
    if (value.fields.size() == 2) {
      const auto *occurrence =
          std::get_if<obs::OccurrenceId>(&value.fields[0].value);
      self.valid = self.valid && occurrence != nullptr &&
                   occurrence->value == 3 &&
                   value.fields[1].availability == obs::Availability::unknown &&
                   value.fields[1].unavailable_reason == "not_observed";
    }
  }

  static void pcm(void *context, const obs::PcmView &value) noexcept {
    auto &self = *static_cast<Consumer *>(context);
    ++self.blocks;
    self.valid = self.valid && value.range.begin == 0 && value.range.end == 1 &&
                 value.channels == 2 && value.bytes.size() == 4;
  }
};
} // namespace

int main() {
  Consumer consumer;
  const obs::Observer observer{&consumer, Consumer::fact, Consumer::pcm};
  const std::array<obs::Cause, 2> causes{
      obs::FactId{2, 1}, obs::PreexistingContext{"initial-detector"}};
  const std::array orders{obs::StateOrder{"mixer", 1}};
  const std::array fields{obs::FieldView{"occurrence",
                                         obs::Availability::known,
                                         obs::Unit::none,
                                         obs::OccurrenceId{3},
                                         {}},
                          obs::FieldView{"position", obs::Availability::unknown,
                                         obs::Unit::sample_frame,
                                         std::monostate{}, "not_observed"}};
  const obs::FactView fact{{1, 1},
                           "playback_started",
                           {obs::Availability::known, 0, {}},
                           causes,
                           orders,
                           fields};
  const std::array<std::byte, 4> samples{};
  const obs::PcmView pcm{{3, 1},
                         "session_postmix",
                         {"output", 44100, 0, 1},
                         obs::PcmFormat::s16_le,
                         2,
                         samples,
                         causes};
  observer.observe(fact);
  observer.observe(pcm);
  obs::Observer{}.observe(fact);
  obs::Observer{}.observe(pcm);
  return consumer.valid && consumer.facts == 1 && consumer.blocks == 1 ? 0 : 1;
}
