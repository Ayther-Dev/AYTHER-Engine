#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <ayther/ayther_session.h>
#include <doctest/doctest.h>

#include "pose_pack_fixture.h"

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
      value = (value >> 1) ^
              ((value & 1U) != 0 ? 0xEDB88320U : 0U);
  }
  return ~value;
}
NativeConfig configuration() {
  std::vector<NativeBackendInfo> backends;
  REQUIRE(ayther::engine::NativeEmulation::backends(backends).code == 0);
  const auto found = std::ranges::find(backends, std::string("synthetic.complete"),
                                       &NativeBackendInfo::id);
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
    hash[index] = static_cast<std::uint8_t>(
        (nibble(digest[index * 2]) << 4) | nibble(digest[index * 2 + 1]));
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
    if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f') return static_cast<std::uint8_t>(value - 'a' + 10);
    return static_cast<std::uint8_t>(value - 'A' + 10);
  };
  for (std::size_t index = 0; index < 32; ++index)
    hash[index] = static_cast<std::uint8_t>(
        (nibble(digest[index * 2]) << 4) | nibble(digest[index * 2 + 1]));
  field(result.config, 7, 5, hash);
  scalar(result.config, 8, 1, 895780);
  scalar(result.config, 9, 1, 53693175);
  scalar(result.config, 10, 1, 1);
  scalar(result.config, 11, 2, 32768);
  scalar(result.config, 12, 2, 4096);
  field(result.config, 13, 6, text("integer-causal-ZOH-v1;CD-disabled-zero-required;zero-initialized-upstream-options"));
  return result;
}
std::string manifest(const NativeConfig &config) {
  char id[32];
  std::snprintf(id, sizeof(id), "crc32:%08x", crc32(config.content));
  return std::string("[pack]\nname = \"native_graphics\"\n") +
         "version = \"1.0.0\"\n" +
         "game_id = \"" + id + "\"\n" +
         "ayther_min = \"0.1.0\"\n" +
         "author = \"AYTHER Tessera\"\n" +
         "license = \"MIT\"\n" +
         "contributors = [\"AYTHER Tessera\"]\n" +
         "[regions]\ndefault = \"NTSC\"\nsupported = [\"NTSC\"]\n" +
         "[systems]\nincluded = [\"tiles\"]\n";
}
std::unique_ptr<ayther::AytherSession>
open(const NativeConfig &native, const std::string &pack,
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

TEST_CASE("Native pack validation rejects bad candidates without losing the selected pack") {
  const auto native = configuration();
  ayther::test::TrustedPackFixture fixture("tessera_native_pack");
  const auto metadata = manifest(native);
  const std::string rules =
      "[[sub]]\nhash = \"0x0000000000000001\"\n"
      "asset = \"graphics/first.png\"\n"
      "[[sub]]\nhash = \"0x0000000000000001\"\n"
      "asset = \"graphics/second.png\"\n";
  const auto first = ayther::test::solid_png(8, 8, 0xFF0000FFU);
  const auto second = ayther::test::solid_png(8, 8, 0x00FF00FFU);
  REQUIRE(fixture.add_bytes("manifest.toml",
                            reinterpret_cast<const uint8_t *>(metadata.data()),
                            metadata.size()));
  REQUIRE(fixture.add_bytes("tile_substitutions.toml",
                            reinterpret_cast<const uint8_t *>(rules.data()),
                            rules.size()));
  REQUIRE(fixture.add_bytes("graphics/first.png", first.data(), first.size()));
  REQUIRE(fixture.add_bytes("graphics/second.png", second.data(), second.size()));
  std::array<char, 512> error{};
  REQUIRE(fixture.finish(error.data(), error.size()));
  auto session = open(native, fixture.pack_path(), fixture.registry_path());
  REQUIRE(session->has_pack());
  CHECK(std::string(session->game_id()).find("crc32:") == 0);
  const auto old = std::string(session->game_id());

  const auto missing = session->set_pack(fixture.pack_path() + ".missing");
  CHECK_FALSE(missing);
  CHECK(session->has_pack());
  CHECK(session->game_id() == old);
  const auto corrupt = fixture.pack_path() + ".corrupt.ay";
  std::filesystem::copy_file(fixture.pack_path(), corrupt);
  {
    std::fstream output(corrupt, std::ios::binary | std::ios::in | std::ios::out);
    REQUIRE(output.good());
    output.put('X'); // Invalidate the ZIP header without changing the selected file.
  }
  CHECK_FALSE(session->set_pack(corrupt));
  CHECK(session->has_pack());
  CHECK(session->game_id() == old);
  std::unique_ptr<AytherTileSubstitutor, decltype(&ayther_tile_sub_free)> catalog(
      ayther_tile_sub_new(), &ayther_tile_sub_free);
  REQUIRE(catalog != nullptr);
  auto *archive = fixture.open();
  REQUIRE(archive != nullptr);
  ayther_tile_sub_load_pack(catalog.get(), archive);
  ayther_tile_sub_begin_frame(catalog.get(), 1, nullptr, 0, false);
  std::array<char, 128> selected{};
  REQUIRE(ayther_tile_sub_lookup(catalog.get(), 1, selected.data(),
                                 static_cast<std::uint32_t>(selected.size())));
  CHECK(std::string(selected.data()) == "graphics/first.png");
  ayther_tile_sub_add_override(catalog.get(), 1, "graphics/second.png");
  REQUIRE(ayther_tile_sub_lookup(catalog.get(), 1, selected.data(),
                                 static_cast<std::uint32_t>(selected.size())));
  CHECK(std::string(selected.data()) == "graphics/second.png");
  const auto wrong_trust = open(native, "", "missing-registry.toml");
  const auto refused = wrong_trust->set_pack(fixture.pack_path());
  CHECK_FALSE(refused);
  CHECK_FALSE(wrong_trust->has_pack());

  const auto findings = session->validate_pack(fixture.pack_path());
  CHECK(std::ranges::none_of(findings, [](const auto &finding) {
    return finding.error;
  }));
  CHECK(session->reload_pack());
  CHECK(session->has_pack());
  CHECK(session->set_pack(""));
  CHECK_FALSE(session->has_pack());
}

TEST_CASE("Native activation rejects signed packs with missing attribution or dangling tile rules") {
  const auto native = configuration();
  const auto image = ayther::test::solid_png(8, 8, 0xFF0000FFU);
  const auto bake = [&](ayther::test::TrustedPackFixture &fixture,
                        const std::string &metadata,
                        const std::string &rules) {
    REQUIRE(fixture.add_bytes("manifest.toml",
                              reinterpret_cast<const uint8_t *>(metadata.data()),
                              metadata.size()));
    REQUIRE(fixture.add_bytes("tile_substitutions.toml",
                              reinterpret_cast<const uint8_t *>(rules.data()),
                              rules.size()));
    REQUIRE(fixture.add_bytes("assets/ok.png", image.data(), image.size()));
    std::array<char, 512> error{};
    REQUIRE(fixture.finish(error.data(), error.size()));
  };
  const std::string good_rules =
      "[[sub]]\nhash = \"0x0000000000000001\"\nasset = \"ok.png\"\n";
  ayther::test::TrustedPackFixture missing_author("tessera_missing_author");
  auto no_author = manifest(native);
  const std::string author = "author = \"AYTHER Tessera\"\n";
  REQUIRE(no_author.find(author) != std::string::npos);
  no_author.erase(no_author.find(author), author.size());
  bake(missing_author, no_author, good_rules);
  auto first = open(native, "", missing_author.registry_path());
  CHECK_FALSE(first->set_pack(missing_author.pack_path()));
  CHECK_FALSE(first->has_pack());

  ayther::test::TrustedPackFixture missing_asset("tessera_missing_asset");
  const std::string bad_rules =
      "[[sub]]\nhash = \"0x0000000000000001\"\nasset = \"missing.png\"\n";
  bake(missing_asset, manifest(native), bad_rules);
  auto second = open(native, "", missing_asset.registry_path());
  CHECK_FALSE(second->set_pack(missing_asset.pack_path()));
  CHECK_FALSE(second->has_pack());
}

TEST_CASE("Native validation rejects signed metadata and tile-rule mutations") {
  const auto native = configuration();
  const auto png = ayther::test::solid_png(8, 8, 0xFF0000FFU);
  const std::string valid_rules =
      "[[sub]]\nhash = \"0x0000000000000001\"\nasset = \"ok.png\"\n";
  struct Mutation {
    const char *name;
    std::string metadata;
    std::string rules;
    Bytes image;
  };
  auto future = manifest(native);
  future.replace(future.find("ayther_min = \"0.1.0\""),
                 std::string("ayther_min = \"0.1.0\"").size(),
                 "ayther_min = \"999.0.0\"");
  auto wrong_rom = manifest(native);
  wrong_rom.replace(wrong_rom.find("game_id = \"crc32:"),
                    std::string("game_id = \"crc32:").size() + 8,
                    "game_id = \"crc32:00000000");
  auto missing_license = manifest(native);
  missing_license.erase(missing_license.find("license = \"MIT\"\n"),
                        std::string("license = \"MIT\"\n").size());
  const std::vector<Mutation> mutations{
      {"future_version", future, valid_rules, png},
      {"wrong_rom", wrong_rom, valid_rules, png},
      {"missing_license", missing_license, valid_rules, png},
      {"invalid_hash", manifest(native),
       "[[sub]]\nhash = \"not-hex\"\nasset = \"ok.png\"\n", png},
      {"external_path", manifest(native),
       "[[sub]]\nhash = \"0x01\"\nasset = \"../ok.png\"\n", png},
      {"invalid_image", manifest(native), valid_rules, Bytes{0, 1, 2, 3}}};
  for (const auto &mutation : mutations) {
    INFO(std::string(mutation.name));
    ayther::test::TrustedPackFixture fixture(mutation.name);
    REQUIRE(fixture.add_bytes("manifest.toml",
                              reinterpret_cast<const std::uint8_t *>(
                                  mutation.metadata.data()),
                              mutation.metadata.size()));
    REQUIRE(fixture.add_bytes("tile_substitutions.toml",
                              reinterpret_cast<const std::uint8_t *>(
                                  mutation.rules.data()),
                              mutation.rules.size()));
    REQUIRE(fixture.add_bytes("assets/ok.png", mutation.image.data(),
                              mutation.image.size()));
    std::array<char, 512> error{};
    REQUIRE(fixture.finish(error.data(), error.size()));
    auto session = open(native, "", fixture.registry_path());
    const auto findings = session->validate_pack(fixture.pack_path());
    CHECK(std::ranges::any_of(findings, [&](const auto &finding) {
      return mutation.name == std::string_view("missing_license")
                 ? finding.code == "meta.license"
                 : finding.error;
    }));
    CHECK_FALSE(session->set_pack(fixture.pack_path()));
    CHECK_FALSE(session->has_pack());
  }
}

TEST_CASE("Persistent FX-03 seed binds the real Mega Drive ROM and first tile rule") {
  const auto root = std::filesystem::path(AYTHER_FX03_ROOT);
  const auto native = megadrive_configuration();
  auto session = open(native, (root / "fx03.ay").string(),
                      (root / "trust.toml").string());
  REQUIRE(session->has_pack());
  CHECK(std::string(session->game_id()) == "crc32:2af4a80d");
  std::array<ayther::engine::NativeInputAssignment, 8> neutral{};
  for (std::uint16_t control = 0; control < neutral.size(); ++control)
    neutral[control] = {0, control, 0};
  auto &emulation = *session->native_emulation();
  REQUIRE(session->native_submit_presentation_policy(4, 0).code == 0);
  REQUIRE(session->native_step_frame({1, 1, neutral}).code == 0);
  CHECK(session->native_presentation_policy().confirmed);
  ayther::engine::NativeSnapshot capture;
  REQUIRE(emulation.snapshot_acquire(capture).code == 0);
  ayther::engine::NativeRegion sat;
  REQUIRE(capture.region(0, sat).code == 0);
  CHECK(sat.data[0xD800 + 3] == 1); // slot 0 links to slot 1
  CHECK(sat.data[0xD800 + 12] == 0x80); // slot 1 is high priority
  const auto &frame = session->native_frame();
  REQUIRE(frame.native_result.code == 0);
  REQUIRE(frame.native_original.pixels.size() == 320 * 224 * 4);
  const auto pixel = (36 * 320 + 36) * 4;
  CHECK(frame.native_original.pixels[pixel] == 255);
  CHECK(frame.native_original.pixels[pixel + 1] == 0);
  CHECK(frame.native_original.pixels[pixel + 2] == 0);
  const auto colour_at = [&](std::size_t x, std::size_t y) {
    const auto offset = (y * 320 + x) * 4;
    return std::array<std::uint8_t, 3>{frame.native_original.pixels[offset],
                                       frame.native_original.pixels[offset + 1],
                                       frame.native_original.pixels[offset + 2]};
  };
  const std::array<std::uint8_t, 3> red{255, 0, 0},
                                   green{0, 255, 0}, blue{0, 0, 255};
  CHECK(colour_at(97, 68) == red);
  CHECK(colour_at(101, 68) == red);
  CHECK(colour_at(105, 68) == green); // SAT slot 0 masks slot 1
  CHECK(colour_at(109, 68) == blue);
  std::array<std::uint8_t, 8 * 8 * 4> bgra{};
  for (std::size_t y = 0; y < 8; ++y)
    for (std::size_t x = 0; x < 8; ++x) {
      const auto source = ((32 + y) * 320 + 32 + x) * 4;
      const auto dest = (y * 8 + x) * 4;
      bgra[dest] = frame.native_original.pixels[source + 2];
      bgra[dest + 1] = frame.native_original.pixels[source + 1];
      bgra[dest + 2] = frame.native_original.pixels[source];
      bgra[dest + 3] = 255;
    }
  std::unique_ptr<AytherTileHasher, decltype(&ayther_tile_hasher_free)> hasher(
      ayther_tile_hasher_new(), &ayther_tile_hasher_free);
  REQUIRE(hasher != nullptr);
  ayther_tile_hasher_process_frame(hasher.get(), bgra.data(), 8, 8, 32, 1);
  AytherTileOccurrence observed{};
  REQUIRE(ayther_tile_hasher_get_occurrences(hasher.get(), &observed, 1) == 1);
  std::ifstream metadata(root / "manifest.json", std::ios::binary);
  REQUIRE(metadata.good());
  const std::string description{std::istreambuf_iterator<char>(metadata),
                                std::istreambuf_iterator<char>()};
  const auto label = description.find("\"tile_hash\": \"0x");
  REQUIRE(label != std::string::npos);
  const auto expected = std::stoull(description.substr(label + 16, 16), nullptr, 16);
  CHECK(observed.hash == expected);
  for (std::size_t y = 0; y < 8; ++y)
    for (std::size_t x = 0; x < 8; ++x) {
      const auto source = ((64 + y) * 320 + 96 + x) * 4;
      const auto dest = (y * 8 + x) * 4;
      bgra[dest] = frame.native_original.pixels[source + 2];
      bgra[dest + 1] = frame.native_original.pixels[source + 1];
      bgra[dest + 2] = frame.native_original.pixels[source];
      bgra[dest + 3] = 255;
    }
  ayther_tile_hasher_process_frame(hasher.get(), bgra.data(), 8, 8, 32, 1);
  AytherTileOccurrence overlapped{};
  REQUIRE(ayther_tile_hasher_get_occurrences(hasher.get(), &overlapped, 1) == 1);
  CHECK(overlapped.hash == expected);
  std::unique_ptr<AytherTileSubstitutor, decltype(&ayther_tile_sub_free)> catalog(
      ayther_tile_sub_new(), &ayther_tile_sub_free);
  REQUIRE(catalog != nullptr);
  auto *archive = ayther_pack_open_trusted((root / "fx03.ay").string().c_str(),
                                          (root / "trust.toml").string().c_str());
  REQUIRE(archive != nullptr);
  ayther_tile_sub_load_pack(catalog.get(), archive);
  std::array<char, 128> asset{};
  REQUIRE(ayther_tile_sub_lookup(catalog.get(), observed.hash, asset.data(),
                                 static_cast<std::uint32_t>(asset.size())));
  CHECK(std::string(asset.data()) == "first.png");
  ayther_tile_sub_add_override(catalog.get(), observed.hash, "second.png");
  REQUIRE(ayther_tile_sub_lookup(catalog.get(), observed.hash, asset.data(),
                                 static_cast<std::uint32_t>(asset.size())));
  CHECK(std::string(asset.data()) == "second.png");
  ayther_pack_close(archive);
  CHECK(frame.native_original_reason ==
        ayther::NativeOriginalReason::policy_unconfirmed);
  session->reset();
  CHECK(session->has_pack());
  const auto rearmed = session->native_presentation_policy();
  CHECK(rearmed.pending_frame.epoch == 2);
  CHECK(rearmed.pending_transaction != 0);
  CHECK_FALSE(rearmed.confirmed);
  CHECK(rearmed.active_regions == 0);
  const auto restarted = session->native_step_frame({2, 1, neutral});
  REQUIRE(restarted.code == 0);
  CHECK(restarted.transaction == rearmed.pending_transaction);
  CHECK(session->native_presentation_policy().confirmed);
  std::vector<std::uint8_t> checkpoint;
  REQUIRE(emulation.checkpoint(checkpoint).code == 0);
  REQUIRE(session->native_submit_presentation_policy(0, 0).code == 0);
  const auto pending = session->native_presentation_policy().pending_transaction;
  CHECK(session->native_restore_checkpoint({}).code != 0);
  CHECK(session->native_presentation_policy().pending_transaction == pending);
  const auto restored = session->native_restore_checkpoint(checkpoint);
  REQUIRE(restored.code == 0);
  CHECK(restored.transaction == pending);
  CHECK(restored.cancel_reason == ayther::engine::native_cancel_restore);
  CHECK(session->has_pack());
  CHECK(session->native_presentation_policy().pending_transaction == 0);
  CHECK(session->native_presentation_policy().active_regions == 4);
  ayther::engine::NativeSessionInfo after_restore;
  REQUIRE(emulation.info(after_restore).code == 0);
  CHECK(after_restore.frame_id.epoch == 3);
  CHECK(after_restore.frame_id.frame == 1);
}
