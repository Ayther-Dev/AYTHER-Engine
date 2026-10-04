// Spec 002 phase 5 (R1): a test-core frame with a member sprite of area A
// tiles and a foreign sprite at SAT slot A, the scene that exposed the anchor
// by area. The test core places its sprites from the ROM checksum and redraws
// VRAM every frame, so the frame is reproduced from its saved core state.
// Needs AYTHER_TEST_CORE_PATH.
#pragma once

#include <ayther/ayther_session.h>

#include "../../tools/common/synth_rom.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ayther::test {

inline std::unique_ptr<AytherSession>
open_pose_session(const std::string &rom, const std::string &pack = {},
                  const std::string &registry = {}) {
  AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = rom;
  config.enable_audio = false;
  config.derive_core_pack = false;
  config.pack_path = pack;
  config.trust_registry = registry;
  auto created = AytherSession::create(config);
  if (!created) {
    std::fprintf(stderr, "[FAIL] %s\n", created.error.message.c_str());
    return nullptr;
  }
  return std::move(*created);
}

/// The scene element of a sprite occurrence, or null.
inline const SceneElement *element_of(const FrameView &v,
                                      const AytherSpriteOccurrence &o) {
  for (std::uint32_t i = 0; i < v.scene_count; ++i) {
    const SceneElement &e = v.scene[i];
    if (e.layer == 3 && e.slot == o.slot && e.x == o.screen_x &&
        e.y == o.screen_y && e.hash == o.hash)
      return &e;
  }
  return nullptr;
}

struct PoseAnchorScene {
  std::string rom;
  std::vector<std::uint8_t> before; ///< core state just before the frame
  AytherSpriteOccurrence member{};
  AytherSpriteOccurrence foreign{};
};

inline std::optional<PoseAnchorScene> find_pose_anchor_scene() {
  for (int r = 0; r < 64; ++r) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("ayther_pose_anchor_" + std::to_string(r) + ".md");
    const std::string title = "AYTHER POSE ANCHOR " + std::to_string(r);
    synth::Rom rom(title.c_str());
    synth::program_canonical(rom, true);
    if (!rom.save(path))
      return std::nullopt;
    auto s = open_pose_session(path.string());
    if (!s)
      return std::nullopt;
    for (int f = 0; f < 40; ++f) {
      std::vector<std::uint8_t> before;
      (void)s->serialize(before);
      const FrameView &v = s->step();
      for (std::uint32_t i = 0; i < v.sprite_occ_count; ++i) {
        const AytherSpriteOccurrence &m = v.sprite_occs[i];
        const unsigned area = static_cast<unsigned>(m.w_tiles) * m.h_tiles;
        for (std::uint32_t j = 0; j < v.sprite_occ_count; ++j) {
          const AytherSpriteOccurrence &o = v.sprite_occs[j];
          if (j != i && o.slot == area && o.slot != m.slot && o.hash != m.hash)
            return PoseAnchorScene{path.string(), std::move(before), m, o};
        }
      }
    }
  }
  return std::nullopt;
}

} // namespace ayther::test
