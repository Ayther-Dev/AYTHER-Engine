// Spec 002, BR-115 (RF-1.3, CV-8): retiring the pack clears every piece of
// state derived from it. A pack with one plane set, one screen (Cuadro), one
// panorama and one two-step kinematic; after set_pack(pack) the session holds
// them, and after set_pack("") nothing of the pack remains — no sets, screens,
// panoramas or kinematics, no catalog textures to prewarm, and no inherited
// palette luma peak (the E1 tint reference).
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

std::string elements_toml() {
  ayther::PackScreen screen;
  screen.id = 0xC0FFEE01;
  screen.name = "Screen";
  screen.plane_mask = 0x03;
  screen.asset = "graphics/screen.png";
  screen.cells = {{0x1111, 0, 3, 4}, {0x2222, 1, 5, 6}};
  ayther::PackScreen screen2 = screen;
  screen2.id = 0xC0FFEE02;
  screen2.cells = {{0x3333, 0, 1, 1}};
  ayther::PackPanorama pano;
  pano.id = 0xB00B1E02;
  pano.name = "Strip";
  pano.plane = 1;
  pano.w_cells = 4;
  pano.h_cells = 2;
  pano.asset = "graphics/pano.png";
  pano.cells = {{0xAA01, 0, 0}, {0xAA02, 1, 0}};
  ayther::PackKinematic kin;
  kin.id = 0xC1EA0003;
  kin.name = "Intro";
  kin.gap_frames = 12;
  kin.steps = {{0xC0FFEE01, "", 0}, {0xC0FFEE02, "", 0}};
  ayther::PackPlaneSet set;
  set.id = 0x5E700006;
  set.name = "Sign";
  set.type = "utileria";
  set.plane = 0;
  set.w_cells = 2;
  set.h_cells = 1;
  set.asset = "graphics/set.png";
  set.members = {{0xD001, 0, 0}, {0xD002, 1, 0}};
  return ayther::bake_elements_toml({screen, screen2}, {pano}, {kin}, {}, {set},
                                    {});
}
} // namespace

int main() try {
  ayther::test::PosePackFixture pack("pack_removal");
  for (const char *asset :
       {"graphics/screen.png", "graphics/pano.png", "graphics/set.png"})
    pack.add_asset(asset, ayther::test::solid_png(16, 16, 0x00FF00FFU));
  const std::string elements = elements_toml();
  pack.add_asset("elements.toml",
                 std::vector<std::uint8_t>(elements.begin(), elements.end()));
  std::string error;
  check(pack.bake("", error), "the pack with elements bakes");

  const auto rom_path =
      std::filesystem::temp_directory_path() / "ayther_pack_removal.md";
  {
    ayther::synth::Rom rom("AYTHER PACK REMOVAL");
    ayther::synth::program_canonical(rom, true);
    if (!rom.save(rom_path))
      return 1;
  }
  ayther::AytherSession::Config config;
  config.core_path = AYTHER_TEST_CORE_PATH;
  config.rom_path = rom_path.string();
  config.enable_audio = false;
  config.derive_core_pack = false;
  config.pack_path = pack.pack_path();
  config.trust_registry = pack.registry_path();
  auto created = ayther::AytherSession::create(config);
  if (!created) {
    std::fprintf(stderr, "[FAIL] %s\n", created.error.message.c_str());
    return 1;
  }
  std::unique_ptr<ayther::AytherSession> s = std::move(*created);
  // The test core writes its CRAM from frame ~171 on: by 240 the palette
  // luma peak is above zero.
  for (int f = 0; f < 240; ++f)
    (void)s->step();

  const ayther::AytherSession::PackDerivedState loaded =
      s->pack_derived_state();
  std::printf("  loaded: sets=%u screens=%u panoramas=%u kinematics=%u "
              "peak0=%.3f\n",
              loaded.plane_sets, loaded.screens, loaded.panoramas,
              loaded.kinematics, loaded.luma_peak[0]);
  check(loaded.plane_sets == 1 && loaded.screens == 2 &&
            loaded.panoramas == 1 && loaded.kinematics == 1,
        "with the pack, its set, screens, panorama and kinematic are loaded");
  check(!s->catalog_texture_assets().empty(),
        "with the pack, its catalog lists textures to prewarm");
  bool peak = false;
  for (const double p : loaded.luma_peak)
    peak = peak || p > 0.0;
  check(peak, "the palette luma peak has been measured");

  check(static_cast<bool>(s->set_pack("")), "the pack is retired");
  const ayther::AytherSession::PackDerivedState gone = s->pack_derived_state();
  std::printf("  retired: sets=%u screens=%u panoramas=%u kinematics=%u "
              "peak0=%.3f\n",
              gone.plane_sets, gone.screens, gone.panoramas, gone.kinematics,
              gone.luma_peak[0]);
  check(gone.plane_sets == 0 && gone.screens == 0 && gone.panoramas == 0 &&
            gone.kinematics == 0,
        "RF-1.3: no set, screen, panorama or kinematic of the pack remains");
  check(s->catalog_texture_assets().empty(),
        "RF-1.3: no catalog texture of the pack remains");
  bool zero = true;
  for (const double p : gone.luma_peak)
    zero = zero && p == 0.0;
  check(zero, "RF-1.3: no palette luma peak is inherited");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
