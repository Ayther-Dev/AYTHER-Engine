#include <ayther/engine/audio_fact_queue.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace obs = ayther::engine::audio_observation;

namespace {

struct Consumer {
  std::array<std::uint64_t, 3> sequences{};
  std::array<std::string, 3> kinds{};
  std::array<std::string, 3> values{};
  std::array<std::string, 3> contexts{};
  std::size_t count = 0;
  bool valid = true;

  static void receive(void *context, const obs::FactView &fact) noexcept {
    auto &self = *static_cast<Consumer *>(context);
    try {
      if (self.count >= self.sequences.size() || fact.fields.size() != 1 ||
          fact.causes.size() != 1 || fact.state_orders.size() != 1) {
        self.valid = false;
        return;
      }
      const auto *value = std::get_if<std::string_view>(&fact.fields[0].value);
      const auto *initial =
          std::get_if<obs::PreexistingContext>(&fact.causes[0]);
      if (value == nullptr || initial == nullptr) {
        self.valid = false;
        return;
      }
      self.sequences[self.count] = fact.id.sequence;
      self.kinds[self.count] = fact.kind;
      self.values[self.count] = *value;
      self.contexts[self.count] = initial->state_id;
      ++self.count;
    } catch (...) {
      self.valid = false;
    }
  }
};

struct FactInput {
  std::uint64_t sequence = 0;
  std::string_view kind;
  std::string_view value;
  std::string_view context;
};

obs::FactView make_fact(const FactInput input,
                        std::array<obs::Cause, 1> &causes,
                        std::array<obs::StateOrder, 1> &orders,
                        std::array<obs::FieldView, 1> &fields) {
  causes = {obs::PreexistingContext{input.context}};
  orders = {obs::StateOrder{input.context, input.sequence}};
  fields = {obs::FieldView{
      "track", obs::Availability::known, obs::Unit::none, input.value, {}}};
  return {{7, input.sequence},
          input.kind,
          {obs::Availability::known, input.sequence, {}},
          causes,
          orders,
          fields};
}

bool bounded_owned_and_ordered() {
  obs::BoundedFactQueue<2> queue;
  Consumer consumer;
  std::array<obs::Cause, 1> causes{};
  std::array<obs::StateOrder, 1> orders{};
  std::array<obs::FieldView, 1> fields{};

  std::string kind = "voice_started";
  std::string value = "music";
  std::string context = "fresh_audio";
  const auto first =
      make_fact({1, kind, value, context}, causes, orders, fields);
  if (queue.try_push(first) != obs::FactPushResult::accepted) {
    return false;
  }
  kind.assign(kind.size(), 'x');
  value.assign(value.size(), 'x');
  context.assign(context.size(), 'x');

  const auto second =
      make_fact({2, "voice_stopped", "music", "mixer"}, causes, orders, fields);
  const auto rejected =
      make_fact({3, "mix_span", "effect", "output"}, causes, orders, fields);
  if (queue.try_push(second) != obs::FactPushResult::accepted ||
      queue.try_push(rejected) != obs::FactPushResult::full ||
      !queue.try_consume(&consumer, Consumer::receive)) {
    return false;
  }

  if (queue.try_push(rejected) != obs::FactPushResult::accepted ||
      !queue.try_consume(&consumer, Consumer::receive) ||
      !queue.try_consume(&consumer, Consumer::receive) ||
      queue.try_consume(&consumer, Consumer::receive)) {
    return false;
  }
  const std::array<std::uint64_t, 3> expected_sequences{1, 2, 3};
  return consumer.valid && consumer.count == 3 &&
         consumer.sequences == expected_sequences &&
         consumer.kinds[0] == "voice_started" &&
         consumer.values[0] == "music" &&
         consumer.contexts[0] == "fresh_audio" &&
         consumer.kinds[2] == "mix_span";
}

struct CountConsumer {
  std::size_t count = 0;
  static void receive(void *context, const obs::FactView &) noexcept {
    ++static_cast<CountConsumer *>(context)->count;
  }
};

bool rejects_oversized_without_consuming_capacity() {
  obs::BoundedFactQueue<1> queue;
  std::string oversized(obs::max_text_bytes + 1, 'x');
  const obs::FactView invalid{{1, 1}, oversized, {}, {}, {}, {}};
  const obs::FactView valid{{1, 2}, "valid", {}, {}, {}, {}};
  CountConsumer consumer;
  return queue.try_push(invalid) == obs::FactPushResult::invalid &&
         queue.try_push(valid) == obs::FactPushResult::accepted &&
         queue.try_consume(&consumer, CountConsumer::receive) &&
         consumer.count == 1;
}

bool compact_schema_limits_are_enforced() {
  obs::BoundedFactQueue<2, 1, 0, 2, 32> queue;
  const std::array<obs::Cause, 1> cause{obs::FactId{1, 1}};
  const std::array<obs::FieldView, 2> fields{
      obs::FieldView{"kind",
                     obs::Availability::known,
                     obs::Unit::none,
                     std::string_view{"chip_write"},
                     {}},
      obs::FieldView{"value",
                     obs::Availability::known,
                     obs::Unit::none,
                     std::uint64_t{7},
                     {}}};
  const obs::FactView valid{{3, 1}, "input", {}, cause, {}, fields};
  const std::array<obs::StateOrder, 1> order{obs::StateOrder{"state", 1}};
  const obs::FactView invalid{{3, 2}, "ordered", {}, cause, order, fields};
  CountConsumer consumer;
  return queue.try_push(valid) == obs::FactPushResult::accepted &&
         queue.try_push(invalid) == obs::FactPushResult::invalid &&
         queue.try_consume(&consumer, CountConsumer::receive) &&
         consumer.count == 1;
}

} // namespace

int main() { // NOLINT(bugprone-exception-escape) -- Test allocation failure is
             // fatal.
  static_assert(obs::BoundedFactQueue<2>::capacity() == 2);
  return bounded_owned_and_ordered() &&
                 rejects_oversized_without_consuming_capacity() &&
                 compact_schema_limits_are_enforced()
             ? 0
             : 1;
}
