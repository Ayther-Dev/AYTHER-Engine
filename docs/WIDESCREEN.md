# Widescreen composition

**Status:** widening and pack gating implemented; HUD safe zone specified, not implemented

**Last verified:** 2026-11-19

AYTHER widescreen composition extends the visible background without stretching
the native frame. The Engine fills lateral space from an observed panorama
strip and lets a pack select the effective width per frame.

> [!WARNING]
> This feature is pre-release. The extension path, gate, and sprite behavior
> have focused tests, but the default aspect policy, the HUD safe zone,
> per-Drawing HUD offsets, and broad real-game coverage are not in place.

Widening is renderer- and session-side and depends only on observed plane
cells and scroll, so it behaves the same on the `libretro` and
`tessera_native` providers; both produce the same background strips (see
[Component model](COMPONENT_MODEL.md#backgrounds)).

## Output geometry

The native emulated frame stays centered. `AytherSession::set_widescreen(logical_w)`
requests a logical width in emulator pixels; zero (the default) disables
widening. The renderer fills the added columns from panorama data rather than
reading beyond the wrapped VDP nametable.

`widescreen_target_width()` (`include/ayther/widescreen.h`) computes the width
for an aspect ratio. For 224-line content at 16:9 it gives:

- 398 pixels with square pixels (5 cells per side at H40);
- 427 pixels when preserving the displayed 4:3 pixel aspect (426.67 rounded).

The pack-wide default is an open decision (see [Open decisions](#open-decisions));
code and documentation must not choose one silently.

## Why panorama data is required

The nametable wraps, commonly every 512 pixels. Reading cells outside the
native viewport can therefore return another part of the level instead of the
adjacent background. `BackgroundStitcher` and `ScrollUnwrapper` build a
level-space panorama from content that was actually observed while scrolling.

`widescreen_plan()` emits every requested extension cell, including gaps, and
`widescreen_coverage()` reports how many of them have art. A caller that
returned only cells with available art would hide missing coverage and make
validation falsely optimistic.

If the camera cannot be anchored or the panorama has not observed the adjacent
area, the renderer exposes the absence of art through its defined fallback; it
does not invent level content.

## Pack-controlled gate

`set_widescreen_gate()` accepts the text of `widescreen.toml`. Rules select a
width under explicit runtime conditions, and the first matching rule wins.
The gate is evaluated early enough that the chosen width applies to the same
frame's panorama emission. `widescreen_gated()` says whether the pack currently
expresses a width, and `widescreen_effective()` returns the width of the last
produced frame, which is also `FrameView::wide_w`.

An empty document, or one without `[[widescreen]]`, disarms the gate and returns
control to the last `set_widescreen()` request. It does not force width zero,
so packs that declare nothing keep the host's choice.

Widening should normally be gated. A still menu or an unexplored edge has no
panorama coverage, so enabling it globally reveals empty space.

## Sprite behavior

Widening changes output geometry, not sprite identity. Live sprites and authored
replacements keep native-coordinate semantics and are composed relative to the
centered native frame. Sprites that touch or cross the native boundary are not
clipped merely because they lie outside the original output rectangle.

## Verification

- `tests/unit/widescreen_test.cpp` fixes target-width and extension-plan
  behavior.
- `tools/widescreen_spike` measures panorama coverage against a local project.
- `tools/widescreen_gate_smoke` verifies same-frame gate evaluation, repeated
  transitions, and restoration of manual control.
- `tools/widescreen_sprites_smoke` verifies sprite composition across the
  native boundary on a Vulkan device.
- `tools/widescreen_shot` renders paired off/on captures for manual seam and
  artistic review.

ROM- or project-dependent probes are local integration evidence. They cannot be
required public fixtures unless redistribution rights are documented.

## HUD safe zone (specified, not implemented)

Widening the background does not reposition the game's HUD. Elements anchored
to the native left or right edge appear to float toward the center of a wider
output. No Engine API, pack field, or renderer path for this exists yet.

The intended model treats HUD elements as existing Drawings rather than a
parallel entity type. Each Drawing may carry an optional horizontal offset:

- left-anchored elements move toward the new left edge;
- right-anchored elements move toward the new right edge;
- centered elements may remain unchanged;
- absence of an offset is neutral and preserves current output.

The safe zone is an authoring visualization and rule boundary, not a global
translation applied to all HUD elements.

For games where the camera locks while the player crosses the screen, a fixed
rectangle is insufficient. The intended design composes two existing
mechanisms: Mode 3 supplies an anchored entity position from RAM, and pack Lua
defines how the safe zone follows it, including hysteresis. Game-specific
camera policy belongs in the pack; the Engine must not hard-code one heuristic
for every title.

Implementation order:

1. define the serialized safe-zone and per-Drawing offset contract;
2. apply offsets in rendering, with zero-offset pixel identity as the
   non-regression oracle;
3. carry the fields through pack build, parse, and compatibility checks;
4. expose authoring visualization and Drawing properties in the authoring
   product;
5. connect Mode 3 and Lua for per-frame dynamic behavior;
6. add synthetic and legally distributable fixtures before declaring support.

The Engine repository owns consumption and rendering contracts. AYTHER Lab or
other authoring products own their UI and project workflows.

## Open decisions

- default logical width or aspect policy when a pack does not choose one;
- default safe-zone rectangle, if any;
- transition semantics when a dynamic gate changes width;
- whether a future pack schema needs explicit anchor categories in addition to
  offsets.

Until these are decided and versioned, absence means no widening opinion, no
safe zone, and no HUD relocation.
