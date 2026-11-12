// parity_bench cross-pack (spec 008, TP-210.1, RF-10.3, plan §3.13): bake a
// pack from what one provider observes, play it with either provider, and
// compare per frame the substitutions applied and the presented frame.
//
//   bake     runs the ROM with provider A and writes a signed pack with one
//            rule per distinct identity it saw (screen tiles, sprites, plane
//            tiles), each rule with its own asset, plus the trust registry
//            that vouches for it.
//   play     runs the ROM with a pack and writes one line per frame: the
//            multiset of (element, identity, position) substituted and the
//            hash of the presented frame.
//   compare  compares two play files frame by frame.
//
// The presented frame is the input of the renderer, not its pixels: the
// canonical RGB565 framebuffer plus every substitution command in the order
// the renderer draws it, with all its fields (asset, destination, size,
// flips, tint, slot, priority). The same renderer draws the same image from
// the same input; parity_bench has no GPU compositor (decision of
// 2026-11-11).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ayther {
struct FrameView;
} // namespace ayther

namespace ayther::parity {

/// What a cross-pack session runs: libretro needs `core`; the native
/// provider reads the ROM image itself.
struct CrossSession {
  bool native = false;
  std::string core;     ///< libretro core (provider libretro)
  std::string rom_path; ///< the ROM
  std::string pack;     ///< pack to play ("" = none)
  std::string registry; ///< trust registry of `pack`
};

struct BakeResult {
  bool ok = false;
  std::string error;
  std::size_t tiles = 0;   ///< rules of tile_substitutions.toml
  std::size_t sprites = 0; ///< rules of sprite_substitutions.toml
  std::size_t planes = 0;  ///< rules of plane_tile_substitutions.toml
};

/// Runs `session` (its pack is ignored) with the port-0 inputs and bakes the
/// pack at `pack_path` and its registry at `registry_path`.
BakeResult bake_pack(const CrossSession &session,
                     const std::vector<std::uint16_t> &pads,
                     const std::string &pack_path,
                     const std::string &registry_path);

/// Runs `session` with its pack and returns the play file, or nullopt when
/// the session or the pack cannot be opened (`error` says which). With
/// `detail`, also the multiset of every frame, one entry per line.
std::optional<std::string> play_pack(const CrossSession &session,
                                     const std::vector<std::uint16_t> &pads,
                                     std::string *error,
                                     std::string *detail = nullptr);

/// One play line: `{"frame":N,"subs":{"hash":H,"count":C},"presented":
/// {"hash":H,"count":C}}`. `fb_hash`/`fb_count` carry the framebuffer of the
/// last frame the core drew, for a frame it repeated. Entries of the frame's
/// multiset are appended to `detail` when it is not null.
std::string play_line(std::uint32_t frame, const FrameView &view,
                      std::uint64_t &fb_hash, std::uint32_t &fb_count,
                      std::string *detail = nullptr);

struct CrossDivergence {
  std::string field;       ///< "subs" or "presented"
  std::uint32_t frame = 0; ///< first frame that differs
  std::size_t frames = 0;  ///< how many frames differ in this field
};

struct CrossCompare {
  bool ok = false; ///< false: a file is malformed or the frames differ
  std::string error;
  std::size_t frames = 0;
  std::size_t substituted_frames = 0; ///< golden frames with a substitution
  std::vector<CrossDivergence> divergences;
  [[nodiscard]] bool equal() const { return ok && divergences.empty(); }
};

CrossCompare compare_plays(const std::string &golden,
                           const std::string &candidate);

} // namespace ayther::parity
