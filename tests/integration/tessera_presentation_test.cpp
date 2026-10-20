#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <ayther/ayther_session.h>
#include <doctest/doctest.h>

#include "pose_pack_fixture.h"
#include "session/native_presentation.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
using ayther::engine::NativeBackendInfo;
using ayther::engine::NativeConfig;
void put(Bytes &out, std::uint64_t value, unsigned count) {
  for (unsigned index = 0; index < count; ++index)
    out.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
}
void field(Bytes &out, std::uint32_t tag, std::uint32_t encoding,
           std::span<const std::uint8_t> data) {
  put(out, tag, 4);
  put(out, encoding, 4);
  put(out, data.size(), 8);
  out.insert(out.end(), data.begin(), data.end());
}
void scalar(Bytes &out, std::uint32_t tag, std::uint32_t encoding,
            std::uint32_t value) {
  Bytes data;
  put(data, value, 4);
  field(out, tag, encoding, data);
}
Bytes text(std::string_view value) { return {value.begin(), value.end()}; }
Bytes array(const std::vector<Bytes> &values) {
  Bytes out;
  put(out, values.size(), 8);
  for (const auto &value : values) {
    put(out, value.size(), 8);
    out.insert(out.end(), value.begin(), value.end());
  }
  return out;
}
std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
  std::uint32_t value = 0xFFFFFFFFU;
  for (auto byte : bytes) {
    value ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit)
      value = (value >> 1) ^ ((value & 1U) != 0 ? 0xEDB88320U : 0U);
  }
  return ~value;
}
[[maybe_unused]] NativeConfig configuration() {
  std::vector<NativeBackendInfo> backends;
  REQUIRE(ayther::engine::NativeEmulation::backends(backends).code == 0);
  const auto found = std::ranges::find(
      backends, std::string("synthetic.complete"), &NativeBackendInfo::id);
  REQUIRE(found != backends.end());
  NativeConfig result;
  result.backend_id = found->id;
  result.content = text("TSSYN001");
  for (auto value : {2U, 7U, 0U, 0U})
    put(result.content, value, 4);
  for (unsigned index = 0; index < 16; ++index)
    result.initial_persistent_memory.push_back(
        static_cast<std::uint8_t>(3 + 17 * index));
  scalar(result.config, 1, 1, 0);
  field(result.config, 2, 6, text(result.backend_id));
  field(result.config, 3, 5, found->build_id);
  scalar(result.config, 4, 1, 0);
  scalar(result.config, 5, 1, 48000);
  std::vector<Bytes> inputs;
  for (unsigned index = 0; index < 8; ++index) {
    Bytes item;
    scalar(item, 1, 1, 0);
    scalar(item, 2, 1, index);
    scalar(item, 3, 1, 1);
    scalar(item, 4, 2, 0);
    scalar(item, 5, 2, 1);
    inputs.push_back(std::move(item));
  }
  field(result.config, 6, 8, array(inputs));
  std::array<char, 65> digest{};
  REQUIRE(ayther_sha256_hex(result.initial_persistent_memory.data(),
                            result.initial_persistent_memory.size(),
                            digest.data()));
  Bytes hash(32);
  const auto nibble = [](char value) -> std::uint8_t {
    if (value >= '0' && value <= '9')
      return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f')
      return static_cast<std::uint8_t>(value - 'a' + 10);
    return static_cast<std::uint8_t>(value - 'A' + 10);
  };
  for (std::size_t index = 0; index < 32; ++index) {
    hash[index] = static_cast<std::uint8_t>((nibble(digest[index * 2]) << 4) |
                                            nibble(digest[index * 2 + 1]));
  }
  field(result.config, 7, 5, hash);
  return result;
}
NativeConfig megadrive_configuration() {
  std::vector<NativeBackendInfo> backends;
  REQUIRE(ayther::engine::NativeEmulation::backends(backends).code == 0);
  const auto found = std::ranges::find(backends, std::string("clownmdemu"),
                                       &NativeBackendInfo::id);
  REQUIRE(found != backends.end());
  NativeConfig result;
  result.backend_id = found->id;
  result.system_override = 1;
  std::ifstream rom(std::filesystem::path(AYTHER_FX03_ROOT) / "fx03_rom.md",
                    std::ios::binary);
  REQUIRE(rom.good());
  result.content = {std::istreambuf_iterator<char>(rom),
                    std::istreambuf_iterator<char>()};
  result.initial_persistent_memory.resize(65536);
  scalar(result.config, 1, 1, 1);
  field(result.config, 2, 6, text(result.backend_id));
  field(result.config, 3, 5, found->build_id);
  scalar(result.config, 4, 1, 1);
  scalar(result.config, 5, 1, 48000);
  std::vector<Bytes> inputs;
  for (unsigned index = 0; index < 8; ++index) {
    Bytes item;
    scalar(item, 1, 1, 0);
    scalar(item, 2, 1, index);
    scalar(item, 3, 1, 1);
    scalar(item, 4, 2, 0);
    scalar(item, 5, 2, 1);
    inputs.push_back(std::move(item));
  }
  field(result.config, 6, 8, array(inputs));
  std::array<char, 65> digest{};
  REQUIRE(ayther_sha256_hex(result.initial_persistent_memory.data(),
                            result.initial_persistent_memory.size(),
                            digest.data()));
  Bytes hash(32);
  const auto nibble = [](char value) -> std::uint8_t {
    if (value >= '0' && value <= '9')
      return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f')
      return static_cast<std::uint8_t>(value - 'a' + 10);
    return static_cast<std::uint8_t>(value - 'A' + 10);
  };
  for (std::size_t index = 0; index < 32; ++index)
    hash[index] = static_cast<std::uint8_t>((nibble(digest[index * 2]) << 4) |
                                            nibble(digest[index * 2 + 1]));
  field(result.config, 7, 5, hash);
  scalar(result.config, 8, 1, 895780);
  scalar(result.config, 9, 1, 53693175);
  scalar(result.config, 10, 1, 1);
  scalar(result.config, 11, 2, 32768);
  scalar(result.config, 12, 2, 4096);
  field(result.config, 13, 6,
        text("integer-causal-ZOH-v1;CD-disabled-zero-required;zero-initialized-"
             "upstream-options"));
  return result;
}
[[maybe_unused]] std::string manifest(const NativeConfig &config) {
  char id[32];
  std::snprintf(id, sizeof(id), "crc32:%08x", crc32(config.content));
  return std::string("[pack]\nname = \"native_graphics\"\n") +
         "version = \"1.0.0\"\n" + "game_id = \"" + id + "\"\n" +
         "ayther_min = \"0.1.0\"\n" + "author = \"AYTHER Tessera\"\n" +
         "license = \"MIT\"\n" + "contributors = [\"AYTHER Tessera\"]\n" +
         "[regions]\ndefault = \"NTSC\"\nsupported = [\"NTSC\"]\n" +
         "[systems]\nincluded = [\"tiles\"]\n";
}
std::unique_ptr<ayther::AytherSession> open(const NativeConfig &native,
                                            const std::string &pack,
                                            const std::string &registry) {
  ayther::AytherSession::Config config;
  config.provider = ayther::EmulationProvider::tessera_native;
  config.native_config = native;
  config.pack_path = pack;
  config.trust_registry = registry;
  config.enable_audio = false;
  config.derive_core_pack = false;
  auto created = ayther::AytherSession::create(config);
  REQUIRE(created);
  return std::move(*created);
}
} // namespace

TEST_CASE("FX-03 composes only visible plane pixels and preserves originals") {
  const auto root = std::filesystem::path(AYTHER_FX03_ROOT);
  auto session = open(megadrive_configuration(), (root / "fx03.ay").string(),
                      (root / "trust.toml").string());
  REQUIRE(session->has_pack());
  std::array<ayther::engine::NativeInputAssignment, 8> neutral{};
  for (std::uint16_t control = 0; control < neutral.size(); ++control)
    neutral[control] = {0, control, 0};
  REQUIRE(session->native_submit_presentation_policy(16, 0).code == 0);
  REQUIRE(session->native_step_frame({1, 1, neutral}).code == 0);
  REQUIRE(session->native_presentation_policy().confirmed);
  const auto &frame = session->native_frame();
  REQUIRE(frame.native_result.code == 0);
  CHECK(frame.native_original_reason == ayther::NativeOriginalReason::none);
  REQUIRE(frame.fb_pixels != nullptr);
  const auto original = frame.native_original.pixels;
  const auto *shown = static_cast<const std::uint8_t *>(frame.fb_pixels);
  const auto pixel = [](std::uint32_t x, std::uint32_t y) {
    return (std::size_t(y) * 320 + x) * 4;
  };
  for (auto [x, y] : {std::pair{36U, 36U}, std::pair{100U, 68U}}) {
    CHECK(original[pixel(x, y)] == 255);
    CHECK(shown[pixel(x, y)] == 255);
    CHECK(shown[pixel(x, y) + 1] == 0);
    CHECK(shown[pixel(x, y) + 2] == 255);
  }
  for (auto [x, y] : {std::pair{105U, 68U}, std::pair{109U, 68U}})
    CHECK(std::equal(original.begin() + pixel(x, y),
                     original.begin() + pixel(x, y) + 4, shown + pixel(x, y)));
  for (std::uint32_t y = 0; y < 224; ++y)
    for (std::uint32_t x = 0; x < 320; ++x)
      if (!((x >= 32 && x < 40 && y >= 32 && y < 40) ||
            (x >= 96 && x < 104 && y >= 64 && y < 72)))
        CHECK(std::equal(original.begin() + pixel(x, y),
                         original.begin() + pixel(x, y) + 4,
                         shown + pixel(x, y)));

  constexpr std::uint64_t red_hash = 0x6a56a02d82cba6a1ULL;
  REQUIRE(session->native_set_tile_override(red_hash, "second.png"));
  const auto &override = session->native_frame();
  REQUIRE(override.native_original_reason ==
          ayther::NativeOriginalReason::none);
  shown = static_cast<const std::uint8_t *>(override.fb_pixels);
  CHECK(shown[pixel(36, 36)] == 0);
  CHECK(shown[pixel(36, 36) + 1] == 255);
  CHECK(shown[pixel(36, 36) + 2] == 255);
  CHECK(override.native_original.pixels[pixel(36, 36)] == 255);
  session->native_clear_tile_overrides();
  const auto &catalog = session->native_frame();
  CHECK(catalog.native_original_reason == ayther::NativeOriginalReason::none);
  shown = static_cast<const std::uint8_t *>(catalog.fb_pixels);
  CHECK(shown[pixel(36, 36)] == 255);
  CHECK(shown[pixel(36, 36) + 2] == 255);

  REQUIRE(session->native_submit_presentation_policy(0, 0).code == 0);
  REQUIRE(session->native_step_frame({1, 2, neutral}).code == 0);
  const auto &disabled = session->native_frame();
  CHECK(disabled.native_original_reason ==
        ayther::NativeOriginalReason::policy_unconfirmed);
  CHECK(disabled.fb_pixels == disabled.native_original.pixels.data());
  REQUIRE(session->native_submit_presentation_policy(16, 0).code == 0);
  REQUIRE(session->native_step_frame({1, 3, neutral}).code == 0);
  CHECK(session->native_frame().native_original_reason ==
        ayther::NativeOriginalReason::none);
  session->reset();
  CHECK(session->has_pack());
  CHECK_FALSE(session->native_presentation_policy().confirmed);
  REQUIRE(session->native_step_frame({2, 1, neutral}).code == 0);
  CHECK(session->native_presentation_policy().confirmed);
  CHECK(session->native_frame().native_original_reason ==
        ayther::NativeOriginalReason::none);
}

TEST_CASE(
    "FX-03 malformed or unsupported temporal evidence falls back atomically") {
  const auto root = std::filesystem::path(AYTHER_FX03_ROOT);
  auto session = open(megadrive_configuration(), (root / "fx03.ay").string(),
                      (root / "trust.toml").string());
  std::array<ayther::engine::NativeInputAssignment, 8> neutral{};
  for (std::uint16_t control = 0; control < neutral.size(); ++control)
    neutral[control] = {0, control, 0};
  REQUIRE(session->native_submit_presentation_policy(16, 0).code == 0);
  REQUIRE(session->native_step_frame({1, 1, neutral}).code == 0);
  auto &native = *session->native_emulation();
  ayther::engine::NativeSnapshot snapshot;
  REQUIRE(native.snapshot_acquire(snapshot).code == 0);
  ayther::engine::NativeRegion region;
  REQUIRE(snapshot.region(0, region).code == 0);
  ayther::engine::NativeFrame original;
  REQUIRE(native.original(original).code == 0);
  std::vector<std::uint8_t> output;
  std::unordered_map<std::uint64_t, std::string> overrides;
  auto *pack = ayther_pack_open_trusted((root / "fx03.ay").string().c_str(),
                                        (root / "trust.toml").string().c_str());
  REQUIRE(pack != nullptr);
  const auto compose = [&](const ayther::engine::NativeRegion &evidence,
                           const ayther::engine::NativeFrame &frame) {
    return ayther::session::compose_native_frame(frame, evidence, pack,
                                                 overrides, output);
  };
  CHECK(compose(region, original) ==
        ayther::session::NativeComposeResult::presented);
  auto damaged = region;
  damaged.flags = 0x80000000U;
  CHECK(compose(damaged, original) ==
        ayther::session::NativeComposeResult::incomplete);
  CHECK(output.empty());
  damaged = region;
  damaged.byte_count--;
  CHECK(compose(damaged, original) ==
        ayther::session::NativeComposeResult::incomplete);
  CHECK(output.empty());
  auto unsupported = original;
  unsupported.width = 256;
  CHECK(compose(region, unsupported) ==
        ayther::session::NativeComposeResult::unsupported);
  CHECK(output.empty());
  std::vector<std::uint8_t> unknown_version(region.data.begin(),
                                            region.data.end());
  unknown_version[16] = 3; // temporal record tag 1: version 3 is not supported.
  damaged = region;
  damaged.data = unknown_version;
  CHECK(compose(damaged, original) ==
        ayther::session::NativeComposeResult::incomplete);
  CHECK(output.empty());
  ayther_pack_close(pack);
}

TEST_CASE(
    "FX-03 presentation invalidates on pack and snapshot loss, then recovers") {
  const auto root = std::filesystem::path(AYTHER_FX03_ROOT);
  const auto pack_path = (root / "fx03.ay").string();
  auto session = open(megadrive_configuration(), pack_path,
                      (root / "trust.toml").string());
  std::array<ayther::engine::NativeInputAssignment, 8> neutral{};
  for (std::uint16_t control = 0; control < neutral.size(); ++control)
    neutral[control] = {0, control, 0};
  REQUIRE(session->native_submit_presentation_policy(16, 0).code == 0);
  REQUIRE(session->native_step_frame({1, 1, neutral}).code == 0);
  CHECK(session->native_frame().native_original_reason ==
        ayther::NativeOriginalReason::none);
  REQUIRE(session->set_pack(""));
  const auto &unpacked = session->native_frame();
  CHECK(unpacked.native_original_reason ==
        ayther::NativeOriginalReason::no_pack);
  CHECK(unpacked.fb_pixels == unpacked.native_original.pixels.data());
  REQUIRE(session->set_pack(pack_path));
  CHECK(session->native_frame().native_original_reason ==
        ayther::NativeOriginalReason::none);

  auto &native = *session->native_emulation();
  std::vector<ayther::engine::NativeSnapshot> retained(8);
  REQUIRE(native.snapshot_acquire(retained[0]).code == 0);
  for (std::uint64_t frame = 2; frame <= 8; ++frame) {
    REQUIRE(session->native_step_frame({1, frame, neutral}).code == 0);
    REQUIRE(native.snapshot_acquire(retained[frame - 1]).code == 0);
  }
  REQUIRE(session->native_step_frame({1, 9, neutral}).code == 0);
  const auto &lost = session->native_frame();
  CHECK(lost.native_original_reason ==
        ayther::NativeOriginalReason::capture_lost);
  CHECK(lost.fb_pixels == lost.native_original.pixels.data());
  for (auto &reader : retained)
    CHECK(reader.release().code == 0);
  REQUIRE(session->native_step_frame({1, 10, neutral}).code == 0);
  CHECK(session->native_frame().native_original_reason ==
        ayther::NativeOriginalReason::none);
}

TEST_CASE("FX-03 restore cancels pending presentation and keeps a failed "
          "restore intact") {
  const auto root = std::filesystem::path(AYTHER_FX03_ROOT);
  auto session = open(megadrive_configuration(), (root / "fx03.ay").string(),
                      (root / "trust.toml").string());
  std::array<ayther::engine::NativeInputAssignment, 8> neutral{};
  for (std::uint16_t control = 0; control < neutral.size(); ++control)
    neutral[control] = {0, control, 0};
  REQUIRE(session->native_submit_presentation_policy(16, 0).code == 0);
  REQUIRE(session->native_step_frame({1, 1, neutral}).code == 0);
  REQUIRE(session->native_frame().native_original_reason ==
          ayther::NativeOriginalReason::none);
  std::vector<std::uint8_t> checkpoint;
  REQUIRE(session->native_emulation()->checkpoint(checkpoint).code == 0);
  REQUIRE(session->native_submit_presentation_policy(0, 0).code == 0);
  const auto pending =
      session->native_presentation_policy().pending_transaction;
  CHECK(pending != 0);
  CHECK(session->native_restore_checkpoint({}).code != 0);
  CHECK(session->native_presentation_policy().pending_transaction == pending);
  CHECK(session->native_frame().native_original_reason ==
        ayther::NativeOriginalReason::none);
  const auto restored = session->native_restore_checkpoint(checkpoint);
  REQUIRE(restored.code == 0);
  CHECK(session->native_presentation_policy().pending_transaction == 0);
  CHECK(session->native_presentation_policy().confirmed);
  REQUIRE(session->native_step_frame({2, 2, neutral}).code == 0);
  CHECK(session->native_frame().native_original_reason ==
        ayther::NativeOriginalReason::none);
}

TEST_CASE(
    "FX-03 substitution leaves canonical state, video and PCM unchanged") {
  const auto root = std::filesystem::path(AYTHER_FX03_ROOT);
  const auto native_config = megadrive_configuration();
  auto observed = open(native_config, (root / "fx03.ay").string(),
                       (root / "trust.toml").string());
  auto plain = open(native_config, (root / "fx03.ay").string(),
                    (root / "trust.toml").string());
  std::array<ayther::engine::NativeInputAssignment, 8> neutral{};
  for (std::uint16_t control = 0; control < neutral.size(); ++control)
    neutral[control] = {0, control, 0};
  REQUIRE(observed->native_submit_presentation_policy(16, 0).code == 0);
  for (std::uint64_t frame = 1; frame <= 3; ++frame) {
    REQUIRE(observed->native_step_frame({1, frame, neutral}).code == 0);
    REQUIRE(plain->native_step_frame({1, frame, neutral}).code == 0);
    std::vector<std::uint8_t> left_state, right_state;
    REQUIRE(observed->native_emulation()->state(left_state).code == 0);
    REQUIRE(plain->native_emulation()->state(right_state).code == 0);
    CHECK(left_state == right_state);
    const auto &presented = observed->native_frame();
    const auto &original = plain->native_frame();
    REQUIRE(presented.native_original_reason ==
            ayther::NativeOriginalReason::none);
    CHECK(original.native_original_reason ==
          ayther::NativeOriginalReason::policy_unconfirmed);
    CHECK(std::ranges::equal(presented.native_original.pixels,
                             original.native_original.pixels));
    CHECK(std::ranges::equal(presented.native_original.pcm,
                             original.native_original.pcm));
    CHECK(presented.fb_pixels != presented.native_original.pixels.data());
  }
}
