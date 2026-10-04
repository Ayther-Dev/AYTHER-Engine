// Spec 002, BR-091 (RF-10.2, RNF-2), on a GPU.
//
// The preparation prewarms the textures of the pack catalog: the session
// lists what its catalogs can draw, and the renderer decodes and uploads it
// in order until the texture-memory budget (plan §8, P-13). What fits is
// resident before the first frame; what does not is left for decode on
// demand. A trusted synthetic pack with three pose assets and the test core.
#include <ayther/ayther_renderer.h>
#include <ayther/ayther_session.h>

#include "../../tools/common/synth_rom.h"
#include "pose_pack_fixture.h"
#include "vulkan_test_context.h"

#include <SDL3/SDL.h>

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

constexpr int kSide = 64;
// One 64x64 BGRA texture with its mip chain: 64*64*4 * 4/3, rounded up.
constexpr std::uint64_t kTextureBytes = 64ULL * 64ULL * 4ULL * 4ULL / 3ULL + 1;
} // namespace

int main() try {
  std::printf("=== pack_prewarm_test (spec 002 BR-091) ===\n");
  ayther::test::PosePackFixture pack("pack_prewarm");
  const std::vector<std::string> names = {"graphics/a.png", "graphics/b.png",
                                          "graphics/c.png"};
  for (const std::string &name : names)
    pack.add_asset(name, ayther::test::solid_png(kSide, kSide, 0x00FF00FFU));
  std::string error;
  check(pack.bake("[[pose]]\nhashes = [\"0x1\"]\nasset = \"graphics/a.png\"\n\n"
                  "[[pose]]\nhashes = [\"0x2\"]\nasset = \"graphics/b.png\"\n\n"
                  "[[pose]]\nhashes = [\"0x3\"]\nasset = \"graphics/c.png\"\n",
                  error),
        "the pack with three pose assets bakes");

  const auto rom_path =
      std::filesystem::temp_directory_path() / "ayther_pack_prewarm.md";
  {
    ayther::synth::Rom rom("AYTHER PACK PREWARM");
    ayther::synth::program_canonical(rom, true);
    if (!rom.save(rom_path)) {
      std::fprintf(stderr, "[FAIL] cannot write the synthetic ROM\n");
      return 1;
    }
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
  std::unique_ptr<ayther::AytherSession> session = std::move(*created);
  check(session->pack().is_valid(), "the session opens the pack");

  const std::vector<std::string> assets = session->catalog_texture_assets();
  check(assets == names,
        "RF-10.2: the session lists the catalog textures, each once, in order");

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "[FAIL] SDL_Init\n");
    return 1;
  }
  SDL_Window *window = SDL_CreateWindow("pack_prewarm_test", 64, 64,
                                        SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
  VulkanTestContext ctx;
  if (window == nullptr || !ctx.init(window)) {
    std::fprintf(stderr, "[FAIL] Vulkan context\n");
    return 1;
  }
  const std::string shaders = std::string(AYTHER_SOURCE_DIR) + "/shaders/";
  using TextureState = ayther::AytherRenderer::TextureState;

  // 1. A budget for two textures: the first two are resident at once, the
  //    third is left for decode on demand.
  {
    ayther::AytherRenderer renderer;
    if (!renderer.init(ctx, 320, 224, shaders.c_str())) {
      std::fprintf(stderr, "[FAIL] renderer init\n");
      return 1;
    }
    const ayther::AytherRenderer::PrewarmReport report =
        renderer.prewarm_textures(ctx, session->pack(), names,
                                  2 * kTextureBytes);
    std::printf("  resident=%u over_budget=%u failed=%u gpu_bytes=%llu "
                "decode_ms=%.2f upload_ms=%.2f\n",
                report.resident, report.over_budget, report.failed,
                static_cast<unsigned long long>(report.gpu_bytes),
                report.decode_ms, report.upload_ms);
    check(report.assets == 3 && report.resident == 2 &&
              report.over_budget == 1 && report.failed == 0,
          "RF-10.2: within the budget two textures are prewarmed and one is "
          "left over");
    check(report.gpu_bytes > 0 && report.gpu_bytes <= 2 * kTextureBytes,
          "RNF-2: the prewarmed memory stays within the budget");
    check(renderer.sprite_texture_state(names[0]) == TextureState::ready &&
              renderer.sprite_texture_state(names[1]) == TextureState::ready,
          "RF-10.2: the prewarmed textures are resident before any frame");
    check(renderer.sprite_texture_state(names[2]) ==
              TextureState::not_requested,
          "the texture over the budget is not requested yet");
    renderer.shutdown(ctx);
  }

  // 2. The default budget (P-13) holds all three.
  {
    ayther::AytherRenderer renderer;
    if (!renderer.init(ctx, 320, 224, shaders.c_str())) {
      std::fprintf(stderr, "[FAIL] renderer init\n");
      return 1;
    }
    const ayther::AytherRenderer::PrewarmReport report =
        renderer.prewarm_textures(ctx, session->pack(), names);
    check(report.resident == 3 && report.over_budget == 0,
          "RF-10.2: with the P-13 budget every catalog texture is resident");
    bool all_ready = true;
    for (const std::string &name : names)
      all_ready = all_ready &&
                  renderer.sprite_texture_state(name) == TextureState::ready;
    check(all_ready, "every prewarmed texture reads as ready");
    renderer.shutdown(ctx);
  }

  ctx.shutdown();
  SDL_DestroyWindow(window);
  SDL_Quit();
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", error.what());
  return 1;
}
