// Spec 002, BR-095 (RF-9.4, RF-10.3): only the sprites the core drew are
// drawn as originals. The framebuffer judge of scene_inventory compares a
// few opaque pixels of each sprite with the core's frame; a sprite with none
// on screen was not drawn (the parsed list accumulates entries the game left
// behind). The test core draws a framebuffer unrelated to its sprites, so
// its frames are a synthetic scene with sprites the core did not draw.
#include <ayther/ayther_session.h>

#include "../../tools/common/synth_rom.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (!condition)
    ++failures;
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
}
} // namespace

int main() try {
  const auto rom_path =
      std::filesystem::temp_directory_path() / "ayther_framebuffer_judge.md";
  {
    ayther::synth::Rom rom("AYTHER FRAMEBUFFER JUDGE");
    ayther::synth::program_canonical(rom, true);
    if (!rom.save(rom_path))
      return 1;
  }
  ayther::AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = rom_path.string();
  config.enable_audio = false;
  config.derive_core_pack = false;
  auto created = ayther::AytherSession::create(config);
  if (!created) {
    std::fprintf(stderr, "[FAIL] %s\n", created.error.message.c_str());
    return 1;
  }
  std::unique_ptr<ayther::AytherSession> s = std::move(*created);

  // A frame where the judge finds sprites the core did not draw.
  std::uint32_t judged = 0;
  std::uint32_t dropped = 0;
  const ayther::FrameView *view = nullptr;
  for (int f = 0; f < 400 && dropped == 0; ++f) {
    view = &s->step();
    s->scene_judge_stats(&judged, &dropped, nullptr, nullptr);
  }
  std::printf("  judged=%u dropped=%u\n", judged, dropped);
  check(view != nullptr && dropped > 0,
        "a frame has sprites the core did not draw");
  if (view == nullptr || dropped == 0)
    return 1;

  std::uint32_t sprites_in_scene = 0;
  for (std::uint32_t i = 0; i < view->scene_count; ++i)
    if (view->scene[i].layer == 3)
      ++sprites_in_scene;
  std::printf("  occurrences=%u scene_sprites=%u\n", view->sprite_occ_count,
              sprites_in_scene);
  check(sprites_in_scene + dropped <= view->sprite_occ_count,
        "RF-9.4: a sprite the core did not draw is not drawn as an original");

  // Spec 002 (O2, invariant 6a): the verdict is published per occurrence,
  // so the render probe can tell a replacement that outlives its members.
  std::uint32_t not_drawn = 0;
  for (std::uint32_t i = 0;
       view->sprite_occ_core_drawn != nullptr && i < view->sprite_occ_count;
       ++i)
    not_drawn += view->sprite_occ_core_drawn[i] == 0 ? 1U : 0U;
  std::printf("  core_drawn published=%d not_drawn=%u\n",
              view->sprite_occ_core_drawn != nullptr ? 1 : 0, not_drawn);
  check(view->sprite_occ_core_drawn != nullptr && not_drawn >= dropped,
        "O2 6a: the frame publishes which occurrences the core did not draw");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
