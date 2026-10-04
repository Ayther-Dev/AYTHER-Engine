// A trusted test pack with pose substitutions for spec 002 phase 5 tests:
// a manifest, pose_substitutions.toml and solid-colour PNG assets.
#pragma once

#include "trusted_pack_fixture.h"

#include <stb_image_write.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace ayther::test {

/// PNG bytes of a w×h image of one RGBA colour.
inline std::vector<std::uint8_t> solid_png(int w, int h, std::uint32_t rgba) {
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4);
  for (std::size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i + 0] = static_cast<std::uint8_t>(rgba >> 24U);
    pixels[i + 1] = static_cast<std::uint8_t>(rgba >> 16U);
    pixels[i + 2] = static_cast<std::uint8_t>(rgba >> 8U);
    pixels[i + 3] = static_cast<std::uint8_t>(rgba);
  }
  std::vector<std::uint8_t> png;
  stbi_write_png_to_func(
      [](void *context, void *data, int size) {
        auto *out = static_cast<std::vector<std::uint8_t> *>(context);
        const auto *bytes = static_cast<const std::uint8_t *>(data);
        out->insert(out->end(), bytes, bytes + size);
      },
      &png, w, h, 4, pixels.data(), w * 4);
  return png;
}

/// "0x%016llx" of a hash, as pose_substitutions.toml writes it.
inline std::string hash_hex(std::uint64_t hash) {
  char text[24];
  std::snprintf(text, sizeof(text), "0x%016llx",
                static_cast<unsigned long long>(hash));
  return text;
}

class PosePackFixture {
public:
  explicit PosePackFixture(const char *name) : pack_(name) {}

  /// Adds `asset` (path inside the pack) with PNG bytes.
  void add_asset(const std::string &asset, std::vector<std::uint8_t> png) {
    assets_.emplace_back(asset, std::move(png));
  }

  /// Bakes the pack with `poses_toml` as pose_substitutions.toml.
  bool bake(const std::string &poses_toml, std::string &error) {
    const std::string manifest =
        "[pack]\nname = \"pose_fixture\"\nversion = \"0.0.1\"\n"
        "game_id = \"crc32:00000000\"\nayther_min = \"0.8.0\"\n"
        "\n[regions]\ndefault = \"NTSC\"\nsupported = [\"NTSC\"]\n";
    bool ok =
        pack_.add_bytes("manifest.toml",
                        reinterpret_cast<const std::uint8_t *>(manifest.data()),
                        manifest.size());
    ok = ok && pack_.add_bytes(
                   "pose_substitutions.toml",
                   reinterpret_cast<const std::uint8_t *>(poses_toml.data()),
                   poses_toml.size());
    for (const auto &[path, png] : assets_)
      ok = ok && pack_.add_bytes(path.c_str(), png.data(), png.size());
    char buffer[256] = "";
    ok = ok && pack_.finish(buffer, sizeof(buffer));
    error = buffer;
    return ok;
  }

  [[nodiscard]] std::string pack_path() const { return pack_.pack_path(); }
  [[nodiscard]] std::string registry_path() const {
    return pack_.registry_path();
  }

private:
  TrustedPackFixture pack_;
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> assets_;
};

} // namespace ayther::test
