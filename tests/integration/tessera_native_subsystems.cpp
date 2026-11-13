// ayther.integration.tessera_native_subsystems (spec 008, TP-301.5): the
// Engine subsystems that sit on the frame pipeline — the bg stitcher, the
// Panoramas built on it, Lua and the live audio detector — run on the native
// megadrive machine and give what they give with gpgx.
//
// One synthetic ROM (tools/common/synth_rom.h), run by each provider:
//   - plane A holds a 64-column level whose cells all differ (column = tile,
//     row = flip and palette), and the program pans it one pixel per frame to
//     the right for kPanFrames frames, then rests;
//   - the frame counter goes to work RAM 0xFF0010 every frame, for Lua;
//   - FM channel 1 and PSG tone 0 play two notes at fixed frames, for the live
//     detector.
// The program steps once per VBlank, so a frame of one provider is the same
// frame of the other.
//
// Every provider must pass the checks on its own (non-vacuity: the stitcher
// sees the pan, the Panorama anchors, Lua reads the counter, the detector
// sees both notes), and Tessera must give the same series as gpgx. gpgx is
// the fork core of third_party/cores/core.lock or AYTHER_PARITY_GPGX_CORE;
// without it only Tessera runs and the comparison is reported as skipped.
#include "../support/synth_md_scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <tuple>
#include <vector>

namespace {

using namespace ayther::test::md_scene;

constexpr int kRunFrames = 240; // rested for the last ~75 frames
constexpr int kCaptureFrom = 5; // past the setup

// Lua: the counter the ROM leaves in work RAM, as the session lays it out
// (libretro SYSTEM_RAM, word-swapped, on both providers), and the tiles on
// screen, each into a shader parameter the frame carries back.
constexpr char kScript[] = R"lua(
ayther.on_frame(function()
  ayther.shader.set_param("crt_strength", ayther.ram.read_u8(0x10))
  ayther.shader.set_param("scan_strength", ayther.ram.read_u8(0x11))
  ayther.shader.set_param("vignette", #ayther.tiles.list())
end)
)lua";

struct Voice {
  std::uint64_t signature;
  std::uint8_t chip, channel, pitch;
  bool operator==(const Voice &) const = default;
  auto operator<=>(const Voice &) const = default;
};

struct Run {
  bool ok = false;
  std::vector<std::tuple<float, float, float>> lua; // per frame
  std::vector<std::vector<Voice>> voices;           // per frame
  std::int32_t bounds[4] = {};
  bool bounds_ok = false;
  std::size_t cells = 0;
  bool panorama_valid = false;
  std::int32_t panorama_x = 0, panorama_y = 0;
  std::uint32_t panorama_votes = 0;
};

Run run(const std::string &provider, const std::vector<std::uint8_t> &rom,
        const std::filesystem::path &core) {
  Run out;
  auto s = open(provider, rom, core);
  if (!s)
    return out;
  check(static_cast<bool>(s->load_script(kScript, "tp301_5")),
        provider + ": the Lua script loads");
  for (int frame = 0; frame < kRunFrames; ++frame) {
    if (frame == kCaptureFrom)
      s->bg_capture(true);
    const auto &view = s->step();
    out.lua.emplace_back(view.shader_params.crt_strength,
                         view.shader_params.scan_strength,
                         view.shader_params.vignette);
    AytherAudioActive active[64];
    const std::uint32_t count = s->audio_live_active(active, 64);
    std::vector<Voice> voices;
    for (std::uint32_t i = 0; i < std::min<std::uint32_t>(count, 64); ++i)
      voices.push_back({active[i].signature, active[i].chip, active[i].channel,
                        active[i].pitch});
    std::sort(voices.begin(), voices.end());
    out.voices.push_back(std::move(voices));
  }
  out.bounds_ok = s->bg_bounds(0, out.bounds);
  const auto cells = s->bg_cells(0);
  out.cells = cells.size();
  s->define_panorama(0x301, 0, 0, 0, 0, 0, cells.data(),
                     static_cast<std::uint32_t>(cells.size()),
                     "graphics/tp301_5.png");
  const auto &anchored = s->step();
  out.panorama_valid = anchored.panorama_valid;
  out.panorama_x = anchored.panorama_cam_x;
  out.panorama_y = anchored.panorama_cam_y;
  out.panorama_votes = anchored.panorama_votes;
  out.ok = true;
  std::printf("  %s: bounds x %d..%d y %d..%d, %zu cells; panorama %s at "
              "(%d,%d), %u votes\n",
              provider.c_str(), out.bounds[0], out.bounds[2], out.bounds[1],
              out.bounds[3], out.cells,
              out.panorama_valid ? "valid" : "invalid", out.panorama_x,
              out.panorama_y, out.panorama_votes);
  return out;
}

/// The checks every provider passes on its own.
void check_alone(const std::string &p, const Run &r) {
  // Bg stitcher: the pan from x = 0 to 160 shows columns 0..59 (H40).
  check(r.bounds_ok && r.bounds[0] == 0 && r.bounds[2] == 59,
        p + ": the stitcher reads the pan, columns 0..59");
  check(r.bounds_ok && r.bounds[1] == 0 && r.bounds[3] == 27,
        p + ": and rows 0..27");
  // Column 0 has already started to leave the screen when the capture opens
  // (kCaptureFrom), and a moving camera keeps whole cells only: 59 columns.
  check(r.cells >= 59u * 28u, p + ": every whole cell of the pan is kept (" +
                                  std::to_string(r.cells) + ")");
  // Panorama: anchored where the camera rests.
  check(r.panorama_valid, p + ": the Panorama anchors on the resting camera");
  check(r.panorama_x == kPanFrames && r.panorama_y == 0,
        p + ": at the camera position (160, 0)");
  // Lua: the counter grows one per frame while the pan runs, and a frame
  // has tiles on screen.
  int steps = 0;
  bool tiles = false;
  for (std::size_t f = 1; f < r.lua.size(); ++f) {
    const float a = std::get<0>(r.lua[f]) + 256.f * std::get<1>(r.lua[f]);
    const float b =
        std::get<0>(r.lua[f - 1]) + 256.f * std::get<1>(r.lua[f - 1]);
    const float c = std::get<1>(r.lua[f]) + 256.f * std::get<0>(r.lua[f]);
    const float d =
        std::get<1>(r.lua[f - 1]) + 256.f * std::get<0>(r.lua[f - 1]);
    if (a - b == 1.f || c - d == 1.f)
      ++steps;
    // The screen shows 40×28 cells; the default vignette is 0.2.
    tiles = tiles || std::get<2>(r.lua[f]) >= 40.f * 28.f;
  }
  check(steps >= kPanFrames - 2,
        p + ": Lua reads the work RAM counter frame by frame (" +
            std::to_string(steps) + " steps)");
  check(tiles, p + ": Lua sees the tiles on screen");
  // Live detector: FM 1 and PSG 0 sound in each note and not between them.
  const auto sounding = [&](std::size_t f, std::uint8_t chip) {
    return f < r.voices.size() && std::any_of(
                                      r.voices[f].begin(), r.voices[f].end(),
                                      [&](const Voice &v) {
                                        return v.chip == chip && v.channel == 0;
                                      });
  };
  for (const std::uint8_t chip : {std::uint8_t{0}, std::uint8_t{1}}) {
    const std::string name = chip == 0 ? "FM 1" : "PSG 0";
    // The frame index of `step` is one ahead of the ROM's loop counter when
    // the frame ends inside the loop; a margin of three frames covers both.
    check(sounding(kNoteOn1 + 10, chip) && sounding(kNoteOn2 + 10, chip),
          p + ": the live detector hears " + name + " in both notes");
    check(!sounding(kNoteOff1 + 10, chip) && !sounding(kNoteOff2 + 10, chip),
          p + ": and not after each key-off");
  }
  std::uint64_t first = 0, second = 0;
  for (const auto &v : r.voices[kNoteOn1 + 10])
    if (v.chip == 0)
      first = v.signature;
  for (const auto &v : r.voices[kNoteOn2 + 10])
    if (v.chip == 0)
      second = v.signature;
  check(first != 0 && second != 0 && first != second,
        p + ": the two FM notes have different signatures");
}

} // namespace

int main() try {
  const auto rom = build_rom();
  if (rom.empty()) {
    std::printf("[FAIL] the synthetic ROM could not be written\n");
    return 1;
  }
  const auto core = gpgx_core();
  if (!core.available() && core.failure_exit_code != 77) {
    std::printf("[FAIL] %s\n", core.diagnostic.c_str());
    return 1;
  }

  const Run tessera = run("tessera", rom, {});
  if (tessera.ok)
    check_alone("tessera", tessera);

  if (!core.available()) {
    std::printf("[SKIP] gpgx comparison: %s\n", core.diagnostic.c_str());
  } else {
    const Run gpgx = run("gpgx", rom, core.path);
    if (gpgx.ok)
      check_alone("gpgx", gpgx);
    if (gpgx.ok && tessera.ok) {
      check(std::equal(std::begin(gpgx.bounds), std::end(gpgx.bounds),
                       std::begin(tessera.bounds)) &&
                gpgx.cells == tessera.cells,
            "tessera = gpgx: stitcher bounds and cells");
      check(gpgx.panorama_valid == tessera.panorama_valid &&
                gpgx.panorama_x == tessera.panorama_x &&
                gpgx.panorama_y == tessera.panorama_y &&
                gpgx.panorama_votes == tessera.panorama_votes,
            "tessera = gpgx: Panorama anchor and votes");
      std::size_t lua_frame = gpgx.lua.size();
      for (std::size_t f = 0; f < gpgx.lua.size(); ++f)
        if (gpgx.lua[f] != tessera.lua[f]) {
          lua_frame = f;
          break;
        }
      check(lua_frame == gpgx.lua.size(),
            "tessera = gpgx: Lua outputs every frame" +
                (lua_frame < gpgx.lua.size()
                     ? " (first difference at frame " +
                           std::to_string(lua_frame) + ")"
                     : std::string{}));
      std::size_t voice_frame = gpgx.voices.size();
      for (std::size_t f = 0; f < gpgx.voices.size(); ++f)
        if (gpgx.voices[f] != tessera.voices[f]) {
          voice_frame = f;
          break;
        }
      check(voice_frame == gpgx.voices.size(),
            "tessera = gpgx: live detector voices every frame" +
                (voice_frame < gpgx.voices.size()
                     ? " (first difference at frame " +
                           std::to_string(voice_frame) + ")"
                     : std::string{}));
    }
  }
  std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "OK", failures);
  return failures ? 1 : 0;
} catch (const std::exception &error) {
  std::printf("[FAIL] exception: %s\n", error.what());
  return 1;
}
