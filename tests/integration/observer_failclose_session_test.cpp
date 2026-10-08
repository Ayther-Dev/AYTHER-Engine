// The versioned observer is an all-or-nothing frame boundary. A transient
// snapshot or PARSED_SPRITES read failure must never reuse the preceding
// frame's SYSTEM or expose a deprecated-memory prefix as authoritative.
#include <ayther/ayther_session.h>

#include "session/frame_composability.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}

std::filesystem::path scenario_rom(const char *name, const char *tag) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / name;
  std::vector<char> rom(0x10000, 0);
  std::copy(tag, tag + std::char_traits<char>::length(tag), rom.begin());
  std::ofstream out(path, std::ios::binary);
  out.write(rom.data(), static_cast<std::streamsize>(rom.size()));
  check(static_cast<bool>(out), "the observer regression ROM is written");
  return path;
}

std::unique_ptr<ayther::AytherSession>
open_session(const char *core, const std::filesystem::path &rom) {
  ayther::AytherSession::Config config;
  config.core_path = core;
  config.rom_path = rom.string();
  config.enable_audio = false;
  config.derive_core_pack = false;
  auto created = ayther::AytherSession::create(config);
  check(static_cast<bool>(created), "the observer regression session opens");
  if (!created) {
    std::fprintf(stderr, "[FAIL] %s\n", created.error.message.c_str());
    return nullptr;
  }
  return std::move(*created);
}

void snapshot_failure_fails_closed() {
  const auto rom =
      scenario_rom("ayther_snapshot_failure.md", "AYTHER-SNAPSHOT-FAILURE");
  auto session = open_session(AYTHER_TEST_CORE_PATH, rom);
  if (!session)
    return;

  const ayther::FrameView *frame = nullptr;
  for (int i = 0; i < 3; ++i)
    frame = &session->step();
  check(frame != nullptr &&
            (frame->scene_dirty & ayther::session::kDirtyGeometryPending) == 0,
        "the ABI geometry is current after the synthetic core settles");

  frame = &session->step();
  check((frame->scene_dirty & ayther::session::kDirtyGeometryPending) != 0 &&
            ayther::session::frame_requires_core_image(frame->scene_dirty),
        "O1: a transient snapshot failure invalidates SYSTEM and forces the "
        "complete core image for exactly that frame");

  frame = &session->step();
  check((frame->scene_dirty & ayther::session::kDirtyGeometryPending) == 0,
        "a successful following snapshot restores composability");
}

void parsed_sprite_read_failure_is_authoritative() {
  const auto rom = scenario_rom("ayther_parsed_read_failure.md",
                                "AYTHER-PARSED-READ-FAILURE");
  auto session = open_session(AYTHER_TEST_CORE_PATH, rom);
  if (!session)
    return;

  const ayther::FrameView *frame = nullptr;
  for (int i = 0; i < 3; ++i)
    frame = &session->step();
  std::uint8_t count = 0;
  check(session->parsed_sprites_raw(&count) != nullptr && count != 0,
        "a valid preceding ABI frame publishes parsed sprites");

  frame = &session->step();
  count = 0xFF;
  const std::uint8_t *sprites = session->parsed_sprites_raw(&count);
  check(frame->sprite_occ_count == 0,
        "a failed ABI sprite read clears the preceding occurrences");
  check(sprites == nullptr && count == 0,
        "a failed ABI sprite read never falls back to deprecated memory");
  check((frame->scene_dirty & ayther::session::kDirtyIncompleteSpriteCapture) !=
                0 &&
            ayther::session::frame_requires_core_image(frame->scene_dirty),
        "O1: a failed ABI sprite read marks the frame incomplete and forces "
        "the complete core image");

  frame = &session->step();
  count = 0;
  check(session->parsed_sprites_raw(&count) != nullptr && count != 0 &&
            (frame->scene_dirty &
             ayther::session::kDirtyIncompleteSpriteCapture) == 0,
        "a successful following sprite read replaces the incomplete view");
}

void legacy_core_keeps_legacy_behavior() {
  const auto rom = scenario_rom("ayther_legacy_observer.md", "LEGACY-CORE");
  auto session = open_session(AYTHER_STOCK_CORE_PATH, rom);
  if (!session)
    return;

  const ayther::FrameView &frame = session->step();
  check((frame.scene_dirty & ayther::session::kDirtyGeometryPending) == 0,
        "a genuine legacy core is not classified as a failed ABI snapshot");
}

} // namespace

int main() {
  snapshot_failure_fails_closed();
  parsed_sprite_read_failure_is_authoritative();
  legacy_core_keeps_legacy_behavior();
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
