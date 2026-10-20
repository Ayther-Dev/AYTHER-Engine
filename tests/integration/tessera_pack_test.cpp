#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <ayther/ayther_session.h>
#include <doctest/doctest.h>

#include "pose_pack_fixture.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
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
