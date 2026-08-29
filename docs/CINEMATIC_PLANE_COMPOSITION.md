# Cinematic plane composition

**Status:** implemented; GPU verification is hardware-dependent

**Last verified:** 2026-08-29

AYTHER composes cinematic video at the depth of the VDP planes represented by
its Pictures. It does not unconditionally replace the final screen when a
composed scene is available.

> [!WARNING]
> Renderer integration is pre-release. The synthetic GPU oracle verifies exact
> composition behavior on a real Vulkan device, but broader driver and game
> coverage remains incomplete.

## Problem

The legacy video lane was a full-screen opaque replacement. It covered live
Window and sprite content even when the cinematic represented only background
planes. HD sprites appeared to work because they were later in the HD stack,
which concealed the defect until the emulated game drew live content above the
background.

Panorama composition had already solved the same depth error by drawing its
quads inline at the plane they replace. Cinematic video follows that model.

## Plane-mask contract

Each Picture declares a plane mask: bit 0 is A, bit 1 is B, and bit 2 is
Window. A cinematic computes the union of every step's Picture mask when it is
loaded and exposes that value as `FrameView::video_plane_mask`.

The union is deliberate. Using only the active step's mask would allow a
cinematic that alternates A and A+B to change depth during playback. That
mid-clip z-order jump looks like a composition glitch. A stable union may cover
one extra plane for the whole clip, but it never changes depth implicitly.

If a future format needs depth changes during one cinematic, it requires an
explicit transition contract. It must not emerge as a side effect of switching
steps.

## Rendering order

With a valid composed scene, video is drawn inline after the highest plane in
its mask:

- `B` draws at the B position;
- `A` or `A+B` draws at the A position;
- a mask containing Window draws at the Window position.

Later live planes and sprites therefore remain visible. The global video lane
is retained only as the compatibility fallback when the scene is not composed
or no mask is known (`video_plane_mask == 0`). The Picture/video selection path
remains mutually exclusive.

## VDP foreground priority

`FrameView::video_front` moves the video to the VDP foreground position when
the live cells covered by the cinematic are predominantly priority-1. This is
derived from VDP data, not an authoring checkbox. A full-screen flash encoded as
foreground data must cover the content it was designed to cover.

`video_front` has meaning only when a nonzero plane mask exists. The unknown-mask
fallback already draws full-screen through the legacy lane.

## Alpha policy

The v1 video path uses I420, which has no alpha channel. Cinematic video is
opaque over the planes in its mask.

Chroma key is deliberately deferred. It adds authoring state and produces
halos or dirty edges when the key is poorly chosen. A separate grayscale alpha
stream is also out of scope because it duplicates decode and storage cost.
Neither option should be introduced until a real single-plane cinematic needs
transparency and can supply acceptance fixtures.

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
CTest when spike targets are enabled.

Additional acceptance evidence:

- without a composed scene, output follows the legacy fallback;
- Picture and video lanes remain mutually exclusive;
- manual inspection of a background cinematic should show live HUD and sprites
  above the clip;
- a priority-1 cinematic should still cover them.

The pixel oracle determines correctness of z-order. Manual inspection evaluates
artistic suitability and does not replace the oracle.

## Non-goals

- implicit mask changes between cinematic steps;
- alpha or chroma-key authoring in the v1 video format;
- product UI for choosing or previewing cinematic masks;
- a claim that one passing GPU covers the supported hardware matrix.
