#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <ayther/engine/native_emulation.hpp>
#include <doctest/doctest.h>
#include <picosha2.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace {
namespace native = ayther::engine;
using Bytes = std::vector<std::uint8_t>;
using View = std::span<const std::uint8_t>;

void put(Bytes &out, std::uint64_t value, unsigned count) {
  for (unsigned byte = 0; byte < count; ++byte)
    out.push_back(static_cast<std::uint8_t>(value >> (byte * 8)));
}
void field(Bytes &out, std::uint32_t tag, std::uint32_t encoding, View value) {
  put(out, tag, 4);
  put(out, encoding, 4);
  put(out, value.size(), 8);
  out.insert(out.end(), value.begin(), value.end());
}
void scalar(Bytes &out, std::uint32_t tag, std::uint32_t encoding,
            std::uint64_t value) {
  Bytes data;
  put(data, value, encoding <= 2 ? 4 : 8);
  field(out, tag, encoding, data);
}
Bytes array(const std::vector<Bytes> &values) {
  Bytes out;
  put(out, values.size(), 8);
  for (const auto &value : values) {
    put(out, value.size(), 8);
    out.insert(out.end(), value.begin(), value.end());
  }
  return out;
}
Bytes sha(View value) {
  Bytes out(32);
  picosha2::hash256(value.begin(), value.end(), out.begin(), out.end());
  return out;
}
Bytes text(std::string_view value) { return {value.begin(), value.end()}; }
std::uint64_t read(View data, std::size_t offset, unsigned count) {
  REQUIRE(offset <= data.size());
  REQUIRE(count <= data.size() - offset);
  std::uint64_t result = 0;
  for (unsigned byte = 0; byte < count; ++byte)
    result |= std::uint64_t{data[offset + byte]} << (byte * 8);
  return result;
}
native::NativeConfig config(bool complete = true, unsigned flags = 0) {
  std::vector<native::NativeBackendInfo> backends;
  REQUIRE(native::NativeEmulation::backends(backends).code == 0);
  native::NativeConfig result;
  result.backend_id = complete ? "synthetic.complete" : "synthetic.minimal";
  const auto found =
      std::find_if(backends.begin(), backends.end(), [&](const auto &value) {
        return value.id == result.backend_id;
      });
  REQUIRE(found != backends.end());
  result.content = text("TSSYN001");
  for (const auto value : {complete ? 2U : 1U, 7U, flags, 0U})
    put(result.content, value, 4);
  for (unsigned index = 0; index < 16; ++index)
    result.initial_persistent_memory.push_back(
        static_cast<std::uint8_t>(3 + 17 * index));
  scalar(result.config, 1, 1, 0);
  field(result.config, 2, 6, text(result.backend_id));
  field(result.config, 3, 5, found->build_id);
  scalar(result.config, 4, 1, 0);
  scalar(result.config, 5, 1, 48000);
  std::vector<Bytes> controls;
  for (unsigned index = 0; index < 8; ++index) {
    Bytes item;
    scalar(item, 1, 1, 0);
    scalar(item, 2, 1, index);
    scalar(item, 3, 1, 1);
    scalar(item, 4, 2, 0);
    scalar(item, 5, 2, 1);
    controls.push_back(std::move(item));
  }
  field(result.config, 6, 8, array(controls));
  field(result.config, 7, 5, sha(result.initial_persistent_memory));
  return result;
}
std::unique_ptr<native::NativeEmulation>
create(const native::NativeConfig &configuration) {
  std::unique_ptr<native::NativeEmulation> result;
  REQUIRE(native::NativeEmulation::create(configuration, result).code == 0);
  REQUIRE(result != nullptr);
  return result;
}
std::array<native::NativeInputAssignment, 8> inputs(unsigned mask = 0) {
  std::array<native::NativeInputAssignment, 8> result{};
  for (std::uint16_t index = 0; index < 8; ++index)
    result[index] = {0, index, static_cast<std::int32_t>((mask >> index) & 1U)};
  return result;
}
native::NativeResult step(native::NativeEmulation &emulation,
                          unsigned mask = 0) {
  native::NativeSessionInfo info;
  REQUIRE(emulation.info(info).code == 0);
  const auto assignments = inputs(mask);
  return emulation.step(
      {info.frame_id.epoch, info.frame_id.frame + 1, assignments});
}
Bytes trace(const native::NativeConfig &configuration, std::uint64_t count) {
  Bytes result = text("TSTRACE1");
  field(result, 1, 5, sha(configuration.content));
  field(result, 2, 5, sha(configuration.config));
  scalar(result, 3, 3, count);
  std::vector<Bytes> frames;
  for (std::uint64_t frame = 1; frame <= count; ++frame) {
    std::vector<Bytes> values;
    for (const auto assignment : inputs(32)) {
      Bytes item;
      scalar(item, 1, 1, assignment.port);
      scalar(item, 2, 1, assignment.control);
      scalar(item, 3, 2, static_cast<std::uint32_t>(assignment.value));
      values.push_back(std::move(item));
    }
    Bytes item;
    scalar(item, 1, 3, frame);
    field(item, 2, 8, array(values));
    frames.push_back(std::move(item));
  }
  field(result, 4, 8, array(frames));
  return result;
}
native::NativeResult observe(native::NativeEmulation &emulation,
                             std::uint64_t transaction = 7) {
  native::NativeSessionInfo info;
  REQUIRE(emulation.info(info).code == 0);
  const std::array<native::NativeControlItem, 2> items{
      {{1, 1, 31}, {2, 1, 31}}};
  return emulation.submit_controls(
      {info.frame_id.epoch, info.frame_id.frame + 1, transaction, items});
}
Bytes state(native::NativeEmulation &emulation) {
  Bytes result;
  REQUIRE(emulation.state(result).code == 0);
  return result;
}
} // namespace

TEST_CASE("native provider identifies backends and preserves an existing owner "
          "on creation failure") {
  const auto configuration = config();
  native::NativeDetection detection;
  REQUIRE(native::NativeEmulation::detect(configuration.content, 0, detection)
              .code == 0);
  CHECK(detection.outcome == 1);
  CHECK(detection.system == 0);
  const auto expected_sha = sha(configuration.content);
  CHECK(std::equal(expected_sha.begin(), expected_sha.end(),
                   detection.content_sha256.begin()));
  auto owner = create(configuration);
  const auto previous = owner.get();
  const auto initial = state(*owner);
  auto invalid = configuration;
  invalid.config.back() ^= 1;
  CHECK(native::NativeEmulation::create(invalid, owner).code == -1);
  CHECK(owner.get() == previous);
  CHECK(state(*owner) == initial);
}

TEST_CASE("native original views expose exact RGBA8 and PCM48k without "
          "changing originals") {
  auto configuration = config();
  auto emulation = create(configuration);
  std::fill(configuration.content.begin(), configuration.content.end(), 0);
  std::fill(configuration.initial_persistent_memory.begin(),
            configuration.initial_persistent_memory.end(), 0);
  native::NativeFrame frame;
  CHECK(emulation->original(frame).code == -5);
  REQUIRE(step(*emulation, 32).code == 0);
  REQUIRE(emulation->original(frame).code == 0);
  CHECK(frame.width == 16);
  CHECK(frame.height == 16);
  CHECK(frame.row_bytes == 64);
  CHECK(frame.video_format == 1);
  REQUIRE(frame.pixels.size() == 1024);
  REQUIRE(frame.pcm.size() == 3200);
  CHECK(frame.sample_rate == 48000);
  CHECK(frame.channels == 2);
  CHECK(frame.pcm_frames == 800);
  CHECK(frame.tick_begin == 0);
  CHECK(frame.tick_end == 1000);
  CHECK(frame.pixels[(8 + 16 * 8) * 4] == 40);
  CHECK(frame.pixels[(8 + 16 * 8) * 4 + 1] == 48);
  CHECK(frame.pixels[(8 + 16 * 8) * 4 + 2] == 5);
  CHECK(read(frame.pcm, 0, 2) == 1128);
  CHECK(read(frame.pcm, 2, 2) == 64408);
  const auto before = state(*emulation);
  native::NativeFrame again;
  REQUIRE(emulation->original(again).code == 0);
  CHECK(again.pixels.data() == frame.pixels.data());
  CHECK(again.pcm.data() == frame.pcm.data());
  CHECK(state(*emulation) == before);
}

TEST_CASE(
    "native input failures preserve machine and a unique pending transaction") {
  auto emulation = create(config());
  REQUIRE(observe(*emulation).code == 0);
  CHECK(observe(*emulation, 8).code == -19);
  const auto before = state(*emulation);
  native::NativeSessionInfo info;
  REQUIRE(emulation->info(info).code == 0);
  auto assignment = inputs();
  assignment[7].value = 2;
  CHECK(emulation->step({info.frame_id.epoch, 1, assignment}).code == -1);
  CHECK(emulation->step({info.frame_id.epoch, 2, inputs()}).code == -13);
  CHECK(emulation->step({info.frame_id.epoch + 1, 1, inputs()}).code == -11);
  CHECK(state(*emulation) == before);
  REQUIRE(emulation->info(info).code == 0);
  CHECK(info.pending_transaction == 7);
  const auto applied = step(*emulation, 32);
  CHECK(applied.code == 0);
  CHECK(applied.transaction == 7);
  CHECK(applied.flags == 1);
  CHECK(applied.frame_id.frame == 1);
  CHECK(emulation->last_result().code == 0);
}

TEST_CASE(
    "native snapshots own independent references after close and destruction") {
  auto emulation = create(config());
  REQUIRE(observe(*emulation).code == 0);
  REQUIRE(step(*emulation, 32).code == 0);
  native::NativeSnapshot first, second;
  REQUIRE(emulation->snapshot_acquire(first).code == 0);
  REQUIRE(first.retain(second).code == 0);
  native::NativeSnapshotInfo info;
  REQUIRE(first.info(info).code == 0);
  CHECK(info.complete_mask == 31);
  CHECK(info.region_count == 5);
  native::NativeRegion memory;
  REQUIRE(first.region(0, memory).code == 0);
  const auto saved = Bytes(memory.data.begin(), memory.data.end());
  REQUIRE(observe(*emulation, 9).code == 0);
  const auto closed = emulation->close();
  CHECK(closed.code == 0);
  CHECK(closed.transaction == 9);
  CHECK(closed.cancel_reason == 3);
  CHECK(closed.flags == 2);
  emulation.reset();
  REQUIRE(second.region(0, memory).code == 0);
  CHECK(Bytes(memory.data.begin(), memory.data.end()) == saved);
  REQUIRE(first.release().code == 0);
  CHECK(first.release().code == -4);
  REQUIRE(second.info(info).code == 0);
  REQUIRE(second.release().code == 0);
}

TEST_CASE(
    "native reset preserves failed state and reports successful cancellation") {
  auto emulation = create(config(true, 1));
  const auto initial = state(*emulation);
  REQUIRE(step(*emulation, 32).code == 0);
  REQUIRE(observe(*emulation).code == 0);
  native::NativeSessionInfo before;
  REQUIRE(emulation->info(before).code == 0);
  const auto advanced = state(*emulation);
  const auto failed = emulation->reset();
  CHECK(failed.code == -27);
  CHECK(failed.phase == 5);
  CHECK(state(*emulation) == advanced);
  native::NativeSessionInfo after;
  REQUIRE(emulation->info(after).code == 0);
  CHECK(after.frame_id.epoch == before.frame_id.epoch);
  CHECK(after.pending_transaction == 7);
  const auto reset = emulation->reset();
  CHECK(reset.code == 0);
  CHECK(reset.transaction == 7);
  CHECK(reset.cancel_reason == 1);
  CHECK(reset.flags == 2);
  REQUIRE(emulation->info(after).code == 0);
  CHECK(after.frame_id.epoch != before.frame_id.epoch);
  CHECK(after.frame_id.frame == 0);
  CHECK(after.logical_tick == 0);
  CHECK(after.next_replay_frame == 1);
  CHECK(state(*emulation) == initial);
  REQUIRE(step(*emulation).code == 0);
}

TEST_CASE("native replay EOF and buffers retain public semantics and "
          "unsupported capabilities") {
  const auto configuration = config();
  auto emulation = create(configuration);
  REQUIRE(emulation->bind_trace(trace(configuration, 2)).code == 0);
  REQUIRE(emulation->replay_next().code == 0);
  REQUIRE(emulation->replay_next().code == 0);
  const auto before = state(*emulation);
  REQUIRE(observe(*emulation).code == 0);
  CHECK(emulation->replay_next().code == 1);
  CHECK(state(*emulation) == before);
  native::NativeSessionInfo info;
  REQUIRE(emulation->info(info).code == 0);
  CHECK(info.frame_id.frame == 2);
  CHECK(info.pending_transaction == 7);
  Bytes checkpoint{1, 2, 3};
  CHECK(emulation->checkpoint(checkpoint).code == -6);
  CHECK(checkpoint == Bytes{1, 2, 3});
  CHECK(emulation->restore_checkpoint({}, {}).code == -6);
  REQUIRE(step(*emulation).code == 0);
  std::uint64_t required = 0, next = 0;
  CHECK(emulation->events(0, {}, required, next).code == -3);
  REQUIRE(required > 0);
  Bytes output(static_cast<std::size_t>(required), 0xCC);
  auto short_output = std::span(output).first(output.size() - 1);
  CHECK(emulation->events(0, short_output, required, next).code == -3);
  CHECK(std::all_of(output.begin(), output.end(),
                    [](auto value) { return value == 0xCC; }));
  REQUIRE(emulation->events(0, output, required, next).code == 0);
  CHECK(next > 0);
  std::uint64_t losses_bytes = 0;
  CHECK(emulation->losses({}, losses_bytes).code == -3);
  CHECK(losses_bytes > 0);
  auto minimal = create(config(false));
  REQUIRE(step(*minimal).code == 0);
  native::NativeSnapshot absent;
  CHECK(minimal->snapshot_acquire(absent).code == -6);
  CHECK(observe(*minimal).code == -6);
}
