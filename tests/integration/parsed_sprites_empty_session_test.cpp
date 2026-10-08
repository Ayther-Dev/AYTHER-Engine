// A valid empty PARSED_SPRITES ABI snapshot is authoritative. This regression
// drives the real test core, observer, runner, and public session accessor;
// the core deliberately retains a non-empty deprecated-memory prefix so any
// accidental legacy fallback is directly observable.
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

constexpr char kScenarioTag[] = "AYTHER-EMPTY-ABI-SPRITES";

} // namespace

int main() {
  const std::filesystem::path rom_path =
      std::filesystem::temp_directory_path() / "ayther_empty_abi_sprites.md";
  {
    std::vector<char> rom(0x10000, 0);
    std::copy(std::begin(kScenarioTag), std::end(kScenarioTag) - 1,
              rom.begin());
    std::ofstream out(rom_path, std::ios::binary);
    out.write(rom.data(), static_cast<std::streamsize>(rom.size()));
    check(static_cast<bool>(out), "the regression ROM is written");
    if (!out)
      return 1;
  }

  ayther::AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = rom_path.string();
  config.enable_audio = false;
  config.derive_core_pack = false;
  auto created = ayther::AytherSession::create(config);
  check(static_cast<bool>(created),
        "the session opens with the in-repository ABI core");
  if (!created) {
    std::fprintf(stderr, "[FAIL] %s\n", created.error.message.c_str());
    return 1;
  }

  std::unique_ptr<ayther::AytherSession> session = std::move(*created);
  const ayther::FrameView &frame = session->step();

  check(frame.sprite_occ_count == 0 && frame.raster_reasons == 0,
        "the pending-geometry regression frame has no sprites or raster "
        "writes that could mask the geometry contract");
  check((frame.scene_dirty & ayther::session::kDirtyGeometryPending) != 0 &&
            ayther::session::frame_requires_core_image(frame.scene_dirty),
        "RF-10.1/O1: GEOMETRY_PENDING forces the emitted frame to the core "
        "image even with an empty parsed-sprite list");

  std::uint8_t count = 0xFF;
  const std::uint8_t *sprites = session->parsed_sprites_raw(&count);
  check(sprites == nullptr,
        "a valid empty ABI sprite list returns a null data pointer");
  check(count == 0,
        "a valid empty ABI sprite list never exposes the legacy prefix");

  return failures == 0 ? 0 : 1;
}
