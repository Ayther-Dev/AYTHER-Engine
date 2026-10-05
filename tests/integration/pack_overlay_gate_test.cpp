// Spec 002, DI-18 (F-1): an overlay (Acetato) gated on a recognition-only
// Cuadro — one without an asset — opens its gate from a baked pack, as it
// does in the Lab's live session; and the pack keeps the overlay's position
// in the layer stack.
//
// The Cuadro is taken from the frame itself: the plane cells of plane B at a
// frame of the synthetic ROM, as the Lab captures them. A pack with that
// Cuadro (no asset) and an Acetato gated by presence on it is baked; the
// session that opens it must report the Cuadro present at that frame, so the
// gate opens, and must expose the Acetato's declared stack index.
#include <ayther/ayther_layers.h>
#include <ayther/ayther_session.h>

#include "../../tools/common/synth_rom.h"
#include "ayther_components_toml.h"
#include "pose_pack_fixture.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
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

constexpr int kFrame = 240;
constexpr std::uint64_t kCuadro = 0x4451C3DCE705076DULL;

std::unique_ptr<ayther::AytherSession> open(const std::string &rom,
                                            const std::string &pack,
                                            const std::string &registry) {
  ayther::AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = rom;
  config.enable_audio = false;
  config.derive_core_pack = false;
  config.pack_path = pack;
  config.trust_registry = registry;
  auto created = ayther::AytherSession::create(config);
  if (!created) {
    std::fprintf(stderr, "[FAIL] %s\n", created.error.message.c_str());
    return nullptr;
  }
  return std::move(*created);
}

const ayther::FrameView *run_to(ayther::AytherSession &s, int frame) {
  const ayther::FrameView *fv = nullptr;
  for (int f = 0; f <= frame; ++f)
    fv = &s.step();
  return fv;
}
} // namespace

int main() try {
  const auto rom_path =
      std::filesystem::temp_directory_path() / "ayther_overlay_gate.md";
  {
    ayther::synth::Rom rom("AYTHER OVERLAY GATE");
    ayther::synth::program_canonical(rom, true);
    if (!rom.save(rom_path))
      return 1;
  }

  // 1. The Cuadro: plane B's cells at kFrame, without a pack.
  ayther::PackScreen screen;
  screen.id = kCuadro;
  screen.name = "Pantalla de prueba";
  screen.plane_mask = 0x02; // plane B
  {
    auto s = open(rom_path.string(), "", "");
    if (!s)
      return 1;
    const ayther::FrameView *fv = run_to(*s, kFrame);
    for (std::uint32_t i = 0; fv && fv->plane_cells && i < fv->plane_cell_count;
         ++i) {
      const ayther::PlaneCellHit &c = fv->plane_cells[i];
      if (c.plane != 1 || c.screen_x < 0 || c.screen_y < 0 || c.hash == 0)
        continue;
      screen.cells.push_back({c.hash, 1,
                              static_cast<std::uint8_t>(c.screen_x / 8),
                              static_cast<std::uint8_t>(c.screen_y / 8)});
    }
  }
  std::printf("  Cuadro: %zu cells of plane B at frame %d\n",
              screen.cells.size(), kFrame);
  check(!screen.cells.empty(), "the frame has plane B cells to recognize");

  // 2. The pack: the Cuadro without an asset, and an Acetato gated on it.
  ayther::test::PosePackFixture pack("overlay_gate");
  pack.add_asset("graphics/nubes.png",
                 ayther::test::solid_png(16, 16, 0xFFFFFFFFU));
  const std::string elements =
      ayther::bake_elements_toml({screen}, {}, {}, {}, {}, {});
  pack.add_asset("elements.toml",
                 std::vector<std::uint8_t>(elements.begin(), elements.end()));
  const std::string acetatos = "[[acetato]]\n"
                               "name = \"Nubes\"\n"
                               "index = 1\n"
                               "visible = true\n"
                               "asset = \"graphics/nubes.png\"\n"
                               "img_w = 16\n"
                               "img_h = 16\n"
                               "tile_mode = 1\n"
                               "screen = \"0x4451c3dce705076d\"\n"
                               "gate = \"presencia\"\n";
  pack.add_asset("acetatos.toml",
                 std::vector<std::uint8_t>(acetatos.begin(), acetatos.end()));
  std::string error;
  check(pack.bake("", error), "the pack with the Cuadro and the Acetato bakes");

  // 3. The session that opens it.
  auto s = open(rom_path.string(), pack.pack_path(), pack.registry_path());
  if (!s)
    return 1;
  const auto &overlays = s->pack_overlays();
  check(overlays.size() == 1 && overlays[0].index == 1,
        "F-1b: the Acetato keeps its stack index");
  check(overlays.size() == 1 && overlays[0].content.gated() &&
            overlays[0].content.gate_presence != 0 &&
            overlays[0].content.has_screen(kCuadro),
        "the Acetato is gated by presence on the Cuadro");
  const ayther::FrameView *fv = run_to(*s, kFrame);
  bool present = false;
  for (std::uint32_t i = 0; fv && i < fv->screen_presence_count; ++i)
    present = present || fv->screen_presence_ids[i] == kCuadro;
  std::printf("  presence count %u\n", fv ? fv->screen_presence_count : 0U);
  check(present, "F-1: the Cuadro without an asset is recognized from the "
                 "baked pack");
  check(fv && !overlays.empty() &&
            overlay_gate_open(overlays[0].content, fv->screen_match_id,
                              fv->screen_presence_ids,
                              fv->screen_presence_count),
        "F-1: so the Acetato's gate opens");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
