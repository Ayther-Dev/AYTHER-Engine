// Spec 008 (TP-301.2, D-3): TesseraBackend as an ObservationSource.
//
// 1. synthetic.complete serves the 26 regions of ABI 1.10 with null content
//    and no subscriptions. The ObservationSource helpers over it must answer
//    exactly as they do over an ayther_interface_v1 table that serves the
//    same null scene with ABI semantics: same sizes, statuses (translated
//    from Tessera's), generations, reads and controls. That is the adapter's
//    contract; FrameView equality with a real scene is TP-301.3 / TP-209.1.
// 2. The native megadrive machine: the surface it announces and the
//    translation of Tessera's own statuses (NOT_SUBSCRIBED -28 -> -9).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../../tools/common/synth_rom.h"
#include "../support/tessera_native_config.hpp"
#include "tessera_backend.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace ayther::test::tessera;
using ayther::InterfaceObservationSource;
using ayther::ObservationFn;
using ayther::ObservationSource;

// Region sizes of ABI 1.10 (index = region id).
constexpr std::uint32_t kSizes[27] = {
    0,  65536, 128,   32,    128,  1,  16,  512, 3072,
    1,  1,     65536, 4,     1280, 1,  4,   4,   71680,
    44, 7192,  152,   59160, 2064, 56, 512, 80,  8192};

constexpr ObservationFn kAllFns[] = {ObservationFn::query_region,
                                     ObservationFn::read_region,
                                     ObservationFn::write_control,
                                     ObservationFn::capture_snapshot,
                                     ObservationFn::recompose_frame,
                                     ObservationFn::poll_audio_events,
                                     ObservationFn::get_audio_transport_stats,
                                     ObservationFn::get_subscriptions,
                                     ObservationFn::set_subscriptions,
                                     ObservationFn::poll_frame_delta,
                                     ObservationFn::get_recompose_stats,
                                     ObservationFn::recompose_multilayer,
                                     ObservationFn::frame_delta_since};

// --- The null scene, served through an ayther_interface_v1 table ----------
struct NullScene {
  std::uint64_t generation = 0;
} scene;

int32_t AYTHER_CALL null_query(uint32_t id, ayther_region_info_v1 *out,
                               uint32_t out_size) {
  if (id < 1 || id > 26)
    return AYTHER_STATUS_INVALID_ARGUMENT;
  if (!out || out_size < sizeof(*out))
    return AYTHER_STATUS_INVALID_ARGUMENT;
  // Only the fields the Engine reads; the rest is compared separately.
  out->region_id = id;
  out->byte_size = kSizes[id];
  return AYTHER_STATUS_OK;
}
int32_t AYTHER_CALL null_read(uint32_t id, uint32_t offset, void *out,
                              uint32_t bytes, uint64_t expected,
                              uint64_t *actual) {
  if (id < 1 || id > 26 || (!out && bytes) ||
      uint64_t{offset} + bytes > kSizes[id])
    return AYTHER_STATUS_INVALID_ARGUMENT;
  if (expected != AYTHER_GENERATION_ANY && expected != scene.generation)
    return AYTHER_STATUS_STALE_GENERATION;
  if (bytes)
    std::memset(out, 0, bytes);
  if (actual)
    *actual = scene.generation;
  return AYTHER_STATUS_OK;
}
int32_t AYTHER_CALL null_write(uint32_t id, uint32_t, const void *data,
                               uint32_t bytes, uint64_t expected,
                               uint64_t *new_generation) {
  if (id >= 6 && id <= 9)
    return AYTHER_STATUS_UNSUPPORTED; // suppression is out of the contract
  if (id != 5 && id != 10 && id != 15)
    return AYTHER_STATUS_INVALID_ARGUMENT;
  if (!data || !bytes)
    return AYTHER_STATUS_INVALID_ARGUMENT;
  if (expected != AYTHER_GENERATION_ANY && expected != scene.generation)
    return AYTHER_STATUS_STALE_GENERATION;
  if (new_generation)
    *new_generation = scene.generation;
  return AYTHER_STATUS_OK;
}

ayther_interface_v1 null_table(std::uint64_t capabilities) {
  ayther_interface_v1 table{};
  table.abi_version = AYTHER_ABI_VERSION_1_10;
  table.struct_size = sizeof(table);
  table.capabilities = capabilities;
  table.query_region = null_query;
  table.read_region = null_read;
  table.write_control = null_write;
  return table;
}

std::unique_ptr<native::NativeEmulation> open(const native::NativeConfig &c) {
  std::unique_ptr<native::NativeEmulation> emulation;
  const auto created = native::NativeEmulation::create(c, emulation);
  REQUIRE(created.code == 0);
  REQUIRE(emulation);
  return emulation;
}

} // namespace

TEST_CASE("synthetic.complete answers like the ABI's null scene") {
  auto emulation = open(configuration("synthetic.complete"));
  const ObservationSource &tessera =
      ayther::engine::detail::observation_source(*emulation);

  REQUIRE(tessera.has_ayther_v1());
  CHECK(tessera.ayther_abi_version() == AYTHER_ABI_VERSION_1_10);
  CHECK(tessera.observation_build_id().starts_with(
      "tessera/synthetic.complete/"));
  CHECK(tessera.observation_build_id().size() ==
        std::string_view("tessera/synthetic.complete/").size() + 64);

  // The session's generation, as the provider reports it.
  std::array<std::uint8_t, 1> probe{};
  const auto first = tessera.read_region_v1(
      AYTHER_REGION_LAYER_MASK, probe.data(), 1, AYTHER_GENERATION_ANY);
  REQUIRE(first.ok());
  scene.generation = first.generation;

  const auto table = null_table(tessera.observation_capabilities());
  const InterfaceObservationSource abi(&table);

  CHECK(abi.observation_capabilities() == tessera.observation_capabilities());
  for (const auto fn : kAllFns)
    CHECK_MESSAGE(abi.provides(fn) == tessera.provides(fn), "provides(",
                  static_cast<int>(fn), ")");

  for (std::uint32_t id = 0; id <= 27; ++id) {
    INFO("region ", id);
    CHECK(tessera.abi_region_bytes(id) == abi.abi_region_bytes(id));
    ayther_region_info_v1 info_t{}, info_a{};
    info_t.struct_size = info_a.struct_size = sizeof(info_t);
    const auto qt = tessera.query_region(id, &info_t, sizeof(info_t));
    const auto qa = abi.query_region(id, &info_a, sizeof(info_a));
    CHECK(qt == qa);
    if (qt != AYTHER_STATUS_OK)
      continue;
    CHECK(info_t.region_id == info_a.region_id);
    CHECK(info_t.byte_size == info_a.byte_size);

    std::vector<std::uint8_t> bt(info_t.byte_size, 0xA5),
        ba(info_a.byte_size, 0x5A);
    const auto rt = tessera.read_region_v1(
        id, bt.data(), static_cast<std::uint32_t>(bt.size()),
        AYTHER_GENERATION_ANY);
    const auto ra =
        abi.read_region_v1(id, ba.data(), static_cast<std::uint32_t>(ba.size()),
                           AYTHER_GENERATION_ANY);
    CHECK(rt.status == ra.status);
    CHECK(rt.count == ra.count);
    CHECK(rt.generation == ra.generation);
    CHECK(bt == ba);

    // A stale generation is the ABI's STALE_GENERATION (Tessera's
    // STALE_EPOCH, translated).
    const auto st = tessera.read_region_v1(
        id, bt.data(), static_cast<std::uint32_t>(bt.size()),
        scene.generation + 1000);
    const auto sa =
        abi.read_region_v1(id, ba.data(), static_cast<std::uint32_t>(ba.size()),
                           scene.generation + 1000);
    CHECK(st.status == AYTHER_STATUS_STALE_GENERATION);
    CHECK(st.status == sa.status);
  }

  // Out of range: INVALID_ARGUMENT on both.
  std::array<std::uint8_t, 2> two{};
  CHECK(tessera
            .read_region_v1(AYTHER_REGION_LAYER_MASK, two.data(), 2,
                            AYTHER_GENERATION_ANY)
            .status == abi.read_region_v1(AYTHER_REGION_LAYER_MASK, two.data(),
                                          2, AYTHER_GENERATION_ANY)
                           .status);

  // Controls: accepted ones and the suppression channels (PQ-1).
  for (std::uint32_t id : {5u, 6u, 7u, 8u, 9u, 10u, 15u}) {
    INFO("control ", id);
    const std::uint32_t mute = 0;
    const std::uint8_t byte = 0;
    const void *data = id == 15 ? static_cast<const void *>(&mute) : &byte;
    const std::uint32_t size = id == 15 ? 4 : 1;
    const auto wt = tessera.write_control_v1(id, data, size);
    const auto wa = abi.write_control_v1(id, data, size);
    CHECK(wt.status == wa.status);
    if (wt.ok())
      CHECK(wt.new_generation == wa.new_generation);
  }

  // What synthetic.complete does not serve answers as an absent function.
  ayther_frame_snapshot_v1 snapshot_t{}, snapshot_a{};
  CHECK(tessera.capture_frame_snapshot(snapshot_t).status ==
        abi.capture_frame_snapshot(snapshot_a).status);
  ayther_system_v1 system_t{}, system_a{};
  CHECK(tessera.read_system_v1(system_t).status ==
        abi.read_system_v1(system_a).status);
  CHECK(tessera.subscribe_all_supported() == abi.subscribe_all_supported());
  CHECK(tessera.read_subscriptions().attempted ==
        abi.read_subscriptions().attempted);
  std::array<ayther_audio_event_v1, 4> events{};
  CHECK(tessera.read_audio_poll(events.data(), 4).outcome ==
        abi.read_audio_poll(events.data(), 4).outcome);
  CHECK(tessera.audio_events_dropped() == abi.audio_events_dropped());
  std::array<std::uint16_t, 4> px{};
  std::uint32_t w = 0, h = 0;
  CHECK(tessera.recompose_frame(px.data(), 4, 0, &w, &h) ==
        abi.recompose_frame(px.data(), 4, 0, &w, &h));
}

TEST_CASE("the megadrive machine announces the r4 surface and translates") {
  std::ifstream in(ayther::synth::canonical_rom_path(), std::ios::binary);
  REQUIRE(in);
  Bytes rom{std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
  auto emulation =
      open(native_configuration("megadrive", std::move(rom), Bytes(65536, 0),
                                1 /* TESSERA_SYSTEM_MEGA_DRIVE */));
  ObservationSource &tessera =
      ayther::engine::detail::observation_source(*emulation);

  REQUIRE(tessera.has_ayther_v1());
  // gpgx r4's 0xEFFFF, FRAME_SNAPSHOT included since v0.2.0-rc.3.
  CHECK(tessera.observation_capabilities() == UINT64_C(0xEFFFF));
  CHECK(tessera.provides(ObservationFn::capture_snapshot));
  CHECK(tessera.provides(ObservationFn::get_subscriptions));
  CHECK(tessera.observation_build_id().starts_with("tessera/megadrive/"));

  // SYSTEM is always readable and has the ABI layout.
  ayther_system_v1 system{};
  REQUIRE(tessera.read_system_v1(system).ok());
  CHECK(system.lines_per_frame != 0);

  // Before any subscription, a gated region is the ABI's NOT_SUBSCRIBED
  // (Tessera -28 -> -9).
  std::vector<std::uint8_t> vram(tessera.abi_region_bytes(AYTHER_REGION_VRAM));
  REQUIRE(vram.size() == 65536);
  CHECK(tessera
            .read_region_v1(AYTHER_REGION_VRAM, vram.data(),
                            static_cast<std::uint32_t>(vram.size()),
                            AYTHER_GENERATION_ANY)
            .status == AYTHER_STATUS_NOT_SUBSCRIBED);

  // The snapshot needs no subscription and carries the generation region
  // reads check (TP-302). Below its size it is BUFFER_TOO_SMALL with `out`
  // untouched.
  ayther_frame_snapshot_v1 snapshot{};
  snapshot.struct_size = sizeof(snapshot);
  REQUIRE(tessera.capture_frame_snapshot(snapshot).ok());
  CHECK(snapshot.snapshot_version == 1);
  CHECK((snapshot.flags & AYTHER_SNAPSHOT_CONTENT_LOADED) != 0);
  CHECK((snapshot.flags & AYTHER_SNAPSHOT_FRAME_ACTIVE) == 0);
  std::array<std::uint8_t, 1> mask{};
  const auto layer = tessera.read_region_v1(
      AYTHER_REGION_LAYER_MASK, mask.data(), 1, snapshot.snapshot_generation);
  CHECK(layer.ok());
  CHECK(layer.generation == snapshot.snapshot_generation);
  ayther_frame_snapshot_v1 small{};
  small.snapshot_version = 0xA5A5A5A5u;
  CHECK(tessera.capture_snapshot(&small, sizeof(small) - 1) ==
        AYTHER_STATUS_BUFFER_TOO_SMALL);
  CHECK(small.snapshot_version == 0xA5A5A5A5u);

  // The Engine's subscriptions, bounded by what the machine supports.
  const auto subscribed = tessera.subscribe_all_supported();
  CHECK(subscribed == (ObservationSource::kEngineSubscriptions & 0xFFFu));
  const auto state = tessera.read_subscriptions();
  CHECK(state.attempted);
  CHECK(state.status == AYTHER_STATUS_OK);
  CHECK(state.state.requested_mask == subscribed);
  // An unknown bit is INVALID_ARGUMENT, as in the ABI.
  CHECK(tessera.set_subscriptions(0x80000000u) ==
        AYTHER_STATUS_INVALID_ARGUMENT);
}
