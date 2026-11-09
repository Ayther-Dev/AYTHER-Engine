#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <ayther/ayther_session.h>
#include <doctest/doctest.h>

#include "../support/tessera_native_config.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {
using namespace ayther::test::tessera;
std::unique_ptr<ayther::AytherSession> open_native(std::string_view backend_id,
                                                   std::uint32_t flags = 0) {
  ayther::AytherSession::Config config;
  config.provider = ayther::EmulationProvider::tessera_native;
  config.native_config = configuration(backend_id, flags);
  config.enable_audio = false;
  config.derive_core_pack = false;
  auto created = ayther::AytherSession::create(config);
  REQUIRE(created);
  return std::move(*created);
}
std::array<native::NativeInputAssignment, 8> neutral() {
  std::array<native::NativeInputAssignment, 8> result{};
  for (std::uint16_t index = 0; index < result.size(); ++index)
    result[index] = {0, index, 0};
  return result;
}
} // namespace

TEST_CASE(
    "Engine policy activates only on a matching native frame confirmation") {
  auto selected = open_native("synthetic.complete");
  auto plain = open_native("synthetic.complete");
  auto &machine = *selected->native_emulation();
  const auto before = selected->native_presentation_policy();
  CHECK_FALSE(before.configured);
  CHECK(selected->native_submit_presentation_policy(32, 31).code == -1);
  CHECK_FALSE(selected->native_presentation_policy().configured);
  REQUIRE(selected->native_submit_presentation_policy(31, 31).code == 0);
  const auto queued = selected->native_presentation_policy();
  CHECK(queued.configured);
  CHECK_FALSE(queued.confirmed);
  CHECK(queued.pending_transaction != 0);
  CHECK(queued.active_regions == 0);
  CHECK(selected->native_submit_presentation_policy(0, 0).code == -19);
  CHECK(selected->native_presentation_policy().pending_transaction ==
        queued.pending_transaction);
  REQUIRE(machine.pause().code == 0);
  CHECK(selected->native_presentation_policy().active_regions == 0);
  auto input = neutral();
  CHECK(selected->native_step_frame({1, 1, std::span{input}.first(7)}).code ==
        -15);
  CHECK(selected->native_step_frame({2, 1, input}).code == -11);
  CHECK(selected->native_presentation_policy().pending_transaction ==
        queued.pending_transaction);
  const auto applied = selected->native_step_frame({1, 1, input});
  REQUIRE(applied.code == 0);
  CHECK((applied.flags & native::native_controls_applied) != 0);
  CHECK(applied.transaction == queued.pending_transaction);
  const auto active = selected->native_presentation_policy();
  CHECK(active.confirmed);
  CHECK(active.pending_transaction == 0);
  CHECK(active.active_regions == 31);
  CHECK(active.active_events == 31);
  REQUIRE(plain->native_step_frame({1, 1, input}).code == 0);
  std::vector<std::uint8_t> observed_state, plain_state;
  REQUIRE(machine.state(observed_state).code == 0);
  REQUIRE(plain->native_emulation()->state(plain_state).code == 0);
  CHECK(observed_state == plain_state);
  native::NativeFrame observed_original, plain_original;
  REQUIRE(machine.original(observed_original).code == 0);
  REQUIRE(plain->native_emulation()->original(plain_original).code == 0);
  CHECK(std::ranges::equal(observed_original.pixels, plain_original.pixels));
  CHECK(std::ranges::equal(observed_original.pcm, plain_original.pcm));
  const auto next = selected->native_step_frame({1, 2, input});
  REQUIRE(next.code == 0);
  CHECK((next.flags & native::native_controls_applied) == 0);
  CHECK(next.transaction == 0);
  CHECK(selected->native_presentation_policy().active_regions == 31);
  REQUIRE(plain->native_step_frame({1, 2, input}).code == 0);
  REQUIRE(machine.state(observed_state).code == 0);
  REQUIRE(plain->native_emulation()->state(plain_state).code == 0);
  CHECK(observed_state == plain_state);
  selected->reset();
  const auto rearmed = selected->native_presentation_policy();
  CHECK(rearmed.configured);
  CHECK_FALSE(rearmed.confirmed);
  CHECK(rearmed.active_regions == 0);
  CHECK(rearmed.pending_transaction != 0);
  CHECK(rearmed.pending_transaction != queued.pending_transaction);
  CHECK(rearmed.pending_frame.epoch == 2);
  const auto again = selected->native_step_frame({2, 1, input});
  REQUIRE(again.code == 0);
  CHECK(again.transaction == rearmed.pending_transaction);
  CHECK(selected->native_presentation_policy().confirmed);
}

TEST_CASE("Engine policy rejects unsupported native capabilities atomically") {
  auto minimal = open_native("synthetic.minimal");
  const auto rejected = minimal->native_submit_presentation_policy(31, 31);
  CHECK(rejected.code == -6);
  CHECK_FALSE(minimal->native_presentation_policy().configured);
  CHECK(minimal->native_presentation_policy().active_regions == 0);
}

TEST_CASE(
    "Failed reset keeps a pending Engine policy; successful reset rearms it") {
  auto session = open_native("synthetic.complete", 1);
  auto &machine = *session->native_emulation();
  const auto input = neutral();
  REQUIRE(session->native_submit_presentation_policy(31, 31).code == 0);
  REQUIRE(session->native_step_frame({1, 1, input}).code == 0);
  REQUIRE(session->native_presentation_policy().confirmed);
  REQUIRE(session->native_submit_presentation_policy(0, 0).code == 0);
  const auto pending = session->native_presentation_policy();
  std::vector<std::uint8_t> state_before, state_after;
  REQUIRE(machine.state(state_before).code == 0);
  session->reset();
  CHECK(session->native_presentation_policy().pending_transaction ==
        pending.pending_transaction);
  CHECK(session->native_presentation_policy().active_regions == 31);
  REQUIRE(machine.state(state_after).code == 0);
  CHECK(state_after == state_before);
  native::NativeSessionInfo after_failure;
  REQUIRE(machine.info(after_failure).code == 0);
  CHECK(after_failure.frame_id.epoch == 1);
  CHECK(after_failure.frame_id.frame == 1);
  CHECK(after_failure.pending_transaction == pending.pending_transaction);
  session->reset();
  const auto rearmed = session->native_presentation_policy();
  CHECK(rearmed.pending_transaction != 0);
  CHECK(rearmed.pending_transaction != pending.pending_transaction);
  CHECK(rearmed.pending_frame.epoch == 2);
  CHECK_FALSE(rearmed.confirmed);
  CHECK(rearmed.active_regions == 0);
  const auto applied = session->native_step_frame({2, 1, input});
  REQUIRE(applied.code == 0);
  CHECK(applied.transaction == rearmed.pending_transaction);
  CHECK(session->native_presentation_policy().active_regions == 0);
  CHECK(session->native_presentation_policy().confirmed);
}

TEST_CASE("Successful close cancels the pending transaction and clears Engine "
          "policy") {
  auto session = open_native("synthetic.complete");
  REQUIRE(session->native_submit_presentation_policy(31, 31).code == 0);
  const auto pending = session->native_presentation_policy();
  const auto closed = session->native_close();
  REQUIRE(closed.code == 0);
  CHECK((closed.flags & native::native_control_cancelled) != 0);
  CHECK(closed.transaction == pending.pending_transaction);
  CHECK(closed.cancel_reason == native::native_cancel_close);
  CHECK_FALSE(session->native_presentation_policy().configured);
  CHECK(session->native_presentation_policy().pending_transaction == 0);
}

TEST_CASE("Native restore keeps effective controls and cancels only a pending "
          "request") {
  auto session = open_native("synthetic.complete");
  auto &machine = *session->native_emulation();
  const auto input = neutral();
  REQUIRE(session->native_submit_presentation_policy(31, 31).code == 0);
  REQUIRE(session->native_step_frame({1, 1, input}).code == 0);
  std::vector<std::uint8_t> checkpoint;
  REQUIRE(machine.checkpoint(checkpoint).code == 0);
  REQUIRE(session->native_submit_presentation_policy(0, 0).code == 0);
  const auto pending = session->native_presentation_policy();
  std::vector<std::uint8_t> state_before, state_after;
  REQUIRE(machine.state(state_before).code == 0);
  CHECK(session->native_restore_checkpoint({}).code != 0);
  CHECK(session->native_presentation_policy().pending_transaction ==
        pending.pending_transaction);
  REQUIRE(machine.state(state_after).code == 0);
  CHECK(state_after == state_before);
  const auto restored = session->native_restore_checkpoint(checkpoint);
  REQUIRE(restored.code == 0);
  CHECK((restored.flags & native::native_control_cancelled) != 0);
  CHECK(restored.transaction == pending.pending_transaction);
  CHECK(restored.cancel_reason == native::native_cancel_restore);
  const auto policy = session->native_presentation_policy();
  CHECK(policy.pending_transaction == 0);
  CHECK(policy.configured);
  CHECK(policy.confirmed);
  CHECK(policy.active_regions == 31);
  CHECK(policy.active_events == 31);
  CHECK(policy.requested_regions == 31);
  CHECK(policy.requested_events == 31);
  native::NativeSessionInfo info;
  REQUIRE(machine.info(info).code == 0);
  CHECK(info.frame_id.epoch == 2);
  CHECK(info.frame_id.frame == 1);
  CHECK(info.pending_transaction == 0);
  const auto continued = session->native_step_frame({2, 2, input});
  REQUIRE(continued.code == 0);
  CHECK((continued.flags & native::native_controls_applied) == 0);
  CHECK(session->native_presentation_policy().active_regions == 31);
}
