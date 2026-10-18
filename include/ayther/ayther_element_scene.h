#pragma once
// ---------------------------------------------------------------------------
// ayther_element_scene.h — a synthetic VDP frame built from element records,
// so the Vulkan renderer composes elements with no emulator and no take
// (spec 007, RF-5.1).
//
// build_element_scene() lays the patterns of the placed records into a fresh
// 64 KB VRAM (host order, word-swapped like the core publishes it), assigns
// each distinct palette to one of the four CRAM lines, and emits one
// SceneElement per placement. frame_view() wraps it in a FrameView the
// renderer's scene path accepts (`fv.scene` + `scene_vram` + `scene_cram`,
// not dirty). AytherRenderer::export_elements() is the one-call form.
//
// The CPU counterpart is compose_elements_rgba() (ayther_element_record.h):
// both must give the same image; the GPU test checks it pixel by pixel.
//
// Limits of one scene: 2047 pattern tiles (tile 0 stays empty), four distinct
// palettes (the four CRAM lines), 80 sprites in depth order. Past any of them
// build_element_scene() fails and says which.
// ---------------------------------------------------------------------------
#include "ayther_element_record.h"
#include "ayther_session.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ayther {

/// A synthetic frame owning every buffer its FrameView points to. Keep it
/// alive (and unmoved) while the FrameView is in use.
struct ElementScene {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> vram;          ///< 64 KB, host (word-swapped) order
  std::vector<uint8_t> cram;          ///< 128 B, packed words, LE
  std::vector<SceneElement> elements; ///< one per placement, in order
  std::vector<uint16_t> framebuffer;  ///< w×h RGB565 base (the backdrop)
  uint16_t backdrop_packed = 0;

  /// The FrameView of this scene: the composed path, HD off unless the caller
  /// adds substitutions. Points into this object.
  [[nodiscard]] FrameView frame_view() const;
};

/// Builds the scene for `placements` on a `width × height` canvas. Records
/// are placed in order (later on top among sprites; tiles are drawn beneath
/// sprites, as plane A cells). A placement's palette is its `cram_line`
/// override, else its record's variant, else a grey ramp. false with `error`
/// set when a record is malformed or a limit of the scene is exceeded.
bool build_element_scene(std::span<const ElementPlacement> placements,
                         uint32_t width, uint32_t height,
                         uint16_t backdrop_packed, ElementScene &out,
                         std::string *error = nullptr);

} // namespace ayther
