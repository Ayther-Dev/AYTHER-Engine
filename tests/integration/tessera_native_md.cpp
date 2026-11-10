// ayther.integration.tessera_native_md (spec 008, TP-301.3): FX-02 on the
// native megadrive machine through AytherSession's frame pipeline —
// set_input, step and produce_frame over the TesseraBackend observation
// source, with no libretro core.
//
// FX-02 (Tessera's phase-0 input/PSG probe) holds B in frames 301-600 of its
// action trace. With B the program writes 0x2468 at VRAM 0xFFFC, changes CRAM
// colour 0 and opens PSG tone 0; without it, 0x1357 and a silent tone
// (fixture expected.json). The test drives the same input with set_input and
// reads every one of those effects back from the session.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ayther/ayther_session.h>
#include <ayther/engine/input.hpp>

#include "../support/tessera_native_config.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <vector>

namespace {

using namespace ayther::test::tessera;

constexpr std::uint32_t kVramMarker = 0xFFFC;
constexpr std::uint16_t kMarkerNeutral = 0x1357;
constexpr std::uint16_t kMarkerPressed = 0x2468;
// CRAM colour 0 as the observation contract's CRAM region carries it (9-bit
// BBBGGGRRR, host-endian u16): the fixture's MD words 0x0E00 (blue) and
// 0x000E (red).
constexpr std::uint16_t kColourNeutral = 0x01C0;
constexpr std::uint16_t kColourPressed = 0x0007;
constexpr std::uint64_t kPressedFirst = 301, kPressedLast = 600;
constexpr std::uint64_t kFrames = 900;

Bytes read_file(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "missing fixture " << path.string());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::unique_ptr<ayther::AytherSession> open_fx02() {
  ayther::AytherSession::Config config;
  config.provider = ayther::EmulationProvider::tessera_native;
  config.native_config =
      native_configuration("megadrive", read_file(AYTHER_FX02_PROGRAM),
                           Bytes(65536, 0), 1 /* TESSERA_SYSTEM_MEGA_DRIVE */);
  config.enable_audio = false;
  config.derive_core_pack = false;
  ayther::engine::NativeResult result;
  auto created = ayther::AytherSession::create(config, result);
  REQUIRE_MESSAGE(static_cast<bool>(created),
                  "native session create failed, code " << result.code);
  auto session = std::move(*created);
  REQUIRE(session);
  REQUIRE(session->native_emulation() != nullptr);
  return session;
}

/// The VDP word at a byte address of the session's VRAM (word-swapped bytes,
/// as the observation contract and the libretro fork lay them out).
std::optional<std::uint16_t> vram_word(const ayther::AytherSession &session,
                                       std::uint32_t address) {
  size_t size = 0;
  const std::uint8_t *vram = session.video_ram(&size);
  if (!vram || size < address + 2)
    return std::nullopt;
  return static_cast<std::uint16_t>((vram[address ^ 1u] << 8) |
                                    vram[(address + 1) ^ 1u]);
}

std::optional<std::uint16_t>
cram_colour0(const ayther::AytherSession &session) {
  size_t size = 0;
  const std::uint8_t *cram = session.color_ram(&size);
  if (!cram || size < 2)
    return std::nullopt;
  std::uint16_t word = 0;
  std::memcpy(&word, cram, sizeof(word));
  return word;
}

} // namespace

TEST_CASE("FX-02 runs on the native machine through set_input and step") {
  auto session = open_fx02();
  const auto pad_b =
      ayther::engine::input_mask(ayther::engine::RetroPadButton::b);

  std::uint32_t psg_writes_pressed = 0, chip_writes = 0;
  std::uint32_t neutral_frames = 0, pressed_frames = 0;
  for (std::uint64_t frame = 1; frame <= kFrames; ++frame) {
    const bool pressed = frame >= kPressedFirst && frame <= kPressedLast;
    session->set_input(0, pressed ? pad_b : std::uint16_t{0});
    const ayther::FrameView &view = session->step();
    REQUIRE_MESSAGE(view.native_result.code == 0, "frame " << frame);
    CHECK(view.frame_index == frame);

    // Video: the native original, as libretro XRGB8888.
    REQUIRE(view.fb_pixels != nullptr);
    CHECK(view.fb_format == 1);
    CHECK(view.pixel_encoding == ayther::FramePixelEncoding::libretro);
    CHECK(view.fb_width > 0);
    CHECK(view.fb_height > 0);
    CHECK(view.fb_pitch == view.fb_width * 4);
    // NTSC Mega Drive: 896 040 master cycles per frame.
    CHECK(view.emu_fps > 59.0);
    CHECK(view.emu_fps < 61.0);

    // Every frame carries the observed VDP state (no legacy memory here).
    CHECK_MESSAGE(vram_word(*session, kVramMarker).has_value(),
                  "no VRAM in frame " << frame);
    CHECK_MESSAGE(cram_colour0(*session).has_value(),
                  "no CRAM in frame " << frame);

    chip_writes += view.chip_write_count;
    if (pressed && frame <= kPressedFirst + 8)
      for (std::uint32_t i = 0; i < view.chip_write_count; ++i)
        psg_writes_pressed += view.chip_writes[i].chip == 1 ? 1u : 0u;

    // The central windows of the fixture, where the state is settled.
    const bool neutral_window =
        (frame >= 100 && frame <= 200) || (frame >= 650 && frame <= 850);
    const bool pressed_window = frame >= 350 && frame <= 550;
    if (!neutral_window && !pressed_window)
      continue;
    const auto marker = vram_word(*session, kVramMarker);
    const auto colour = cram_colour0(*session);
    REQUIRE(marker.has_value());
    REQUIRE(colour.has_value());
    if (neutral_window) {
      ++neutral_frames;
      CHECK(*marker == kMarkerNeutral);
      CHECK(*colour == kColourNeutral);
    } else {
      ++pressed_frames;
      CHECK(*marker == kMarkerPressed);
      CHECK(*colour == kColourPressed);
    }
  }
  CHECK(neutral_frames == 302);
  CHECK(pressed_frames == 201);
  // Chip writes come from the observation surface (no libretro core): PSG
  // tone 0 opens when B goes down.
  CHECK(chip_writes > 0);
  CHECK(psg_writes_pressed > 0);

  // Declared gap until Tessera publishes it (rc.4): no work RAM.
  size_t ram_size = 1;
  CHECK(session->work_ram(&ram_size) == nullptr);
  CHECK(ram_size == 0);
}
