#pragma once
// The synthetic Mega Drive scene of the spec 008 provider tests (TP-301.5,
// TP-302): one ROM written here (tools/common/synth_rom.h) that both gpgx and
// the native megadrive machine run, and the session each provider opens on
// it.
//
// The program steps once per VBlank, so a frame of one provider is the same
// frame of the other:
//   - plane A holds a 64-column level whose cells all differ (column = tile,
//     row = flip and palette), panned one pixel per frame to the right for
//     kPanFrames frames, then at rest;
//   - the frame counter goes to work RAM 0xFF0010 every frame;
//   - FM channel 1 and PSG tone 0 play two notes at fixed frames.
#include <ayther/ayther_session.h>

#include "../../tools/common/core_path.h"
#include "../../tools/common/synth_rom.h"
#include "../../tools/parity_bench/native_config.h"

#include <ayther_env.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ayther::test::md_scene {

inline int failures = 0;
inline void check(bool condition, const std::string &message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message.c_str());
}

using Session = ayther::AytherSession;
using ayther::synth::Rom;

inline constexpr int kPanFrames = 160; // the camera ends at x = 160
inline constexpr std::uint8_t kNoteOn1 = 20, kNoteOff1 = 60;
inline constexpr std::uint8_t kNoteOn2 = 90, kNoteOff2 = 120;

/// Pattern of level tile `t` (1..64): row 0 spells t in colours 1/2, the other
/// rows are a fixed ramp, so every tile hashes differently.
inline void write_tiles(Rom &r) {
  r.vdp_vram_at(32u); // tile 1
  for (std::uint32_t t = 1; t <= 64; ++t) {
    for (std::uint32_t row = 0; row < 8; ++row) {
      std::uint32_t pixels = 0;
      for (std::uint32_t p = 0; p < 8; ++p) {
        const std::uint32_t colour =
            row == 0 ? (((t >> p) & 1u) ? 1u : 2u) : 1u + ((row + p) % 3u);
        pixels = (pixels << 4) | colour;
      }
      r.vdp_data(static_cast<std::uint16_t>(pixels >> 16));
      r.vdp_data(static_cast<std::uint16_t>(pixels));
    }
  }
}

/// The level: plane A at 0xC000, 64×28 cells. Column x shows tile x + 1; row
/// y picks the palette line and the flips, so no two cells are alike.
inline void write_level(Rom &r) {
  for (std::uint32_t y = 0; y < 28; ++y) {
    r.vdp_vram_at(0xC000u + y * 64u * 2u);
    const std::uint16_t attributes = static_cast<std::uint16_t>(
        ((y % 4u) << 13) | (((y / 4u) & 1u) << 11) | (((y / 8u) & 1u) << 12));
    for (std::uint32_t x = 0; x < 64; ++x)
      r.vdp_data(static_cast<std::uint16_t>(attributes | (x + 1u)));
  }
}

inline void fm_patch(Rom &r) {
  r.fm(0x22, 0x00);
  r.fm(0x27, 0x00);
  r.fm(0x28, 0x00);
  r.fm(0x2B, 0x00);
  for (int op = 0; op < 4; ++op) {
    const auto o = static_cast<std::uint8_t>(op * 4);
    r.fm(static_cast<std::uint8_t>(0x30 + o), 0x01);
    r.fm(static_cast<std::uint8_t>(0x40 + o), 0x00);
    r.fm(static_cast<std::uint8_t>(0x50 + o), 0x1F);
    r.fm(static_cast<std::uint8_t>(0x60 + o), 0x00);
    r.fm(static_cast<std::uint8_t>(0x70 + o), 0x00);
    r.fm(static_cast<std::uint8_t>(0x80 + o), 0x0F);
    r.fm(static_cast<std::uint8_t>(0x90 + o), 0x00);
  }
  r.fm(0xB0, 0x07);
  r.fm(0xB4, 0xC0);
}

inline void note_on(Rom &r, std::uint8_t fnum_high, std::uint8_t fnum_low,
                    std::uint8_t psg_low) {
  r.fm(0xA4, fnum_high);
  r.fm(0xA0, fnum_low);
  r.fm(0x28, 0xF0);
  r.psg(static_cast<std::uint8_t>(0x80 | psg_low));
  r.psg(0x0F);
  r.psg(0x90); // tone 0 at full volume
}

inline void note_off(Rom &r) {
  r.fm(0x28, 0x00);
  r.psg(0x9F); // tone 0 muted
}

/// Where build_rom leaves the ROM, for the libretro provider (a ROM path).
inline std::filesystem::path rom_path() {
  return std::filesystem::temp_directory_path() / "ayther_md_scene.md";
}

/// Writes the scene ROM to rom_path() and returns its bytes (empty on error).
inline std::vector<std::uint8_t> build_rom() {
  Rom r("AYTHER MD SCENE", 0x10000);
  r.vdp_reg(0x00, 0x04);
  r.vdp_reg(0x01, 0x04); // display off while the level is written
  r.vdp_reg(0x02, 0x30); // plane A @ 0xC000
  r.vdp_reg(0x03, 0x00);
  r.vdp_reg(0x04, 0x07); // plane B @ 0xE000 (empty)
  r.vdp_reg(0x05, 0x6C); // SAT @ 0xD800 (empty)
  r.vdp_reg(0x07, 0x00);
  r.vdp_reg(0x0A, 0xFF);
  r.vdp_reg(0x0B, 0x00); // whole-screen scroll
  r.vdp_reg(0x0C, 0x81); // H40
  r.vdp_reg(0x0D, 0x3C); // hscroll @ 0xF000
  r.vdp_reg(0x0F, 0x02);
  r.vdp_reg(0x10, 0x01); // 64×32 planes
  r.vdp_reg(0x11, 0x00);
  r.vdp_reg(0x12, 0x00);
  r.vdp_cram_at(0x0000);
  for (std::uint16_t line = 0; line < 4; ++line) {
    r.vdp_data(0x0400);
    r.vdp_data(static_cast<std::uint16_t>(0x000E >> line << line));
    r.vdp_data(static_cast<std::uint16_t>(0x00E0 - 0x0020 * line));
    r.vdp_data(static_cast<std::uint16_t>(0x0E00 - 0x0200 * line));
    for (int unused = 4; unused < 16; ++unused)
      r.vdp_data(0x0000);
  }
  write_tiles(r);
  write_level(r);
  r.take_z80_bus();
  fm_patch(r);
  r.psg(0x9F);
  r.vdp_reg(0x01, 0x44); // display on

  for (int frame = 0; frame <= kPanFrames; ++frame) {
    r.wait_vblank();
    r.vdp_vram_at(0xF000u);
    r.vdp_data(static_cast<std::uint16_t>(-frame & 0x3FF));
    r.write_word_to(0xFF0010u, static_cast<std::uint16_t>(frame));
    if (frame == kNoteOn1)
      note_on(r, 0x22, 0x69, 0x0E);
    if (frame == kNoteOff1 || frame == kNoteOff2)
      note_off(r);
    if (frame == kNoteOn2)
      note_on(r, 0x2A, 0x2C, 0x08);
  }
  r.spin();

  if (!r.save(rom_path()))
    return {};
  std::ifstream in(rom_path(), std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// gpgx for the comparison: AYTHER_PARITY_GPGX_CORE, else the fork core of
/// third_party/cores/core.lock. A missing core is failure_exit_code 77.
inline CorePathResult gpgx_core() {
  const char *override_path = ayther::env_get("AYTHER_PARITY_GPGX_CORE");
  return resolve_core_path(AYTHER_SOURCE_DIR,
                           override_path != nullptr && *override_path != '\0'
                               ? std::optional<std::string_view>(override_path)
                               : std::nullopt);
}

/// The scene session of `provider` ("tessera" or "gpgx", the latter on
/// `core`). enable_audio opens the player (non-fatal without a device).
inline std::unique_ptr<Session> open(const std::string &provider,
                                     const std::vector<std::uint8_t> &rom,
                                     const std::filesystem::path &core,
                                     bool enable_audio = false) {
  Session::Config config;
  config.enable_audio = enable_audio;
  config.derive_core_pack = false;
  if (provider == "tessera") {
    auto native = ayther::parity::megadrive_native_config(
        rom, std::vector<std::uint8_t>(65536));
    if (!native) {
      check(false, "tessera: the package has a megadrive backend");
      return nullptr;
    }
    config.provider = ayther::EmulationProvider::tessera_native;
    config.native_config = std::move(*native);
  } else {
    config.core_path = core.string();
    config.rom_path = rom_path().string();
  }
  auto created = Session::create(config);
  if (!created) {
    check(false, provider + ": session opens (" + created.error.message + ")");
    return nullptr;
  }
  return std::move(*created);
}

} // namespace ayther::test::md_scene
