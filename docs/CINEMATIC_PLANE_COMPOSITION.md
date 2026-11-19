# Cinematic plane composition

**Status:** implemented; GPU verification is hardware-dependent

**Last verified:** 2026-11-19

AYTHER composes cinematic video at the depth of the VDP planes represented by
its Pictures, the same way panoramas are drawn inline at the plane they
replace. Video does not unconditionally replace the final screen when a
composed scene is available, so the Window and live sprites above those planes
stay visible.

The contract is renderer-side and consumes only `FrameView` fields, so it is
the same for the `libretro` and `tessera_native` providers. The general
sprite and plane rules are in
[Replacement composition rules](REPLACEMENT_COMPOSITION.md).

> [!WARNING]
> Renderer integration is pre-release. The synthetic GPU oracle verifies exact
> composition behavior on a real Vulkan device, but broader driver and game
> coverage remains incomplete.

## Plane-mask contract

Each Picture declares a plane mask: bit 0 is A, bit 1 is B, and bit 2 is
Window. A cinematic computes the union of every step's Picture mask when it is
loaded and exposes that value as `FrameView::video_plane_mask`.

The union is deliberate. Using only the active step's mask would let a
cinematic that alternates A and A+B change depth during playback, which reads
as a z-order glitch. A stable union may cover one extra plane for the whole
clip, but it never changes depth implicitly. Depth changes within one
cinematic would require an explicit transition contract; they must not emerge
as a side effect of switching steps.

## Rendering order

With a valid composed scene, video is drawn inline after the highest plane in
its mask:

- `B` draws at the B position;
- `A` or `A+B` draws at the A position;
- a mask containing Window draws at the Window position.

Later live planes and sprites therefore remain visible. The full-screen video
lane is used only when the scene is not composed or no mask is known
(`video_plane_mask == 0`). Picture and video selection remain mutually
exclusive.

## VDP foreground priority

`FrameView::video_front` moves the video to the VDP foreground position when
the live cells covered by the cinematic are predominantly priority-1. This is
derived from VDP data, not an authoring checkbox: a full-screen flash encoded
as foreground data must cover the content it was designed to cover.

`video_front` has meaning only when a nonzero plane mask exists. With an
unknown mask the video already draws full-screen through the fallback lane.

## Alpha policy

The video path decodes VP9 to I420, which has no alpha channel. Cinematic video
is opaque over the planes in its mask.

Chroma key and a separate grayscale alpha stream are not supported. Chroma key
adds authoring state and produces halos when the key is poorly chosen; a
separate alpha stream duplicates decode and storage cost. Neither is planned
until a real single-plane cinematic needs transparency and can supply
acceptance fixtures.

## Verification

`tools/video_plane_smoke` constructs a scene without a ROM and compares exact
GPU pixels:

1. a control frame proves the synthetic live sprite is visible;
2. video with an A+B mask must leave that sprite above it;
3. foreground video must cover the same pixel;
4. the two cases must produce different expected colors, preventing a vacuous
   oracle.

The scene uses synthetic VRAM, CRAM, `SceneElement` values, and I420 planes. It
requires a Vulkan-capable environment and is registered as the `video_plane`
CTest (labels `engine;native;gpu`).

Additional acceptance evidence:

- without a composed scene, output follows the full-screen fallback;
- Picture and video lanes remain mutually exclusive;
- manual inspection of a background cinematic should show live HUD and sprites
  above the clip;
- a priority-1 cinematic should still cover them.

The pixel oracle determines correctness of z-order. Manual inspection evaluates
artistic suitability and does not replace the oracle.

## Non-goals

- implicit mask changes between cinematic steps;
- alpha or chroma-key authoring in the current video format;
- product UI for choosing or previewing cinematic masks;
- a claim that one passing GPU covers the supported hardware matrix.
