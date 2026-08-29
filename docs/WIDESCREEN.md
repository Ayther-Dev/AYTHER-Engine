# Widescreen composition and safe-zone roadmap

**Status:** widening and pack gating implemented; HUD safe-zone authoring remains planned

**Last verified:** 2026-08-29

AYTHER widescreen composition extends the visible background without stretching
the native frame. The Engine can fill lateral space from an observed panorama
strip and can let a pack select the effective width per frame.

> [!WARNING]
> This feature remains pre-release. The extension path, gate, and sprite
> behavior have focused tests, but the default aspect policy, author-facing safe
> zone, per-object HUD offsets, and broad real-game coverage are incomplete.

## Output geometry

The native emulated frame remains centered. `set_widescreen(logical_w)` requests
a logical width in emulator pixels; zero disables widening. The renderer fills
the added columns from panorama data rather than reading beyond the wrapped VDP
nametable.

`widescreen_target_width()` exposes two established candidates for 224-line
content:

- 398 pixels for square-pixel 16:9;
- 427 pixels when preserving the displayed 4:3 interpretation.

The pack-wide default remains an open product decision. Code and documentation
must not silently choose one and present it as a frozen format rule.

## Why panorama data is required

The nametable wraps, commonly every 512 pixels. Reading cells outside the
native viewport can therefore return another part of the level instead of the
adjacent background. `BackgroundStitcher` and `ScrollUnwrapper` build a
level-space panorama from content that was actually observed while scrolling.

`widescreen_plan()` emits every requested extension cell, including gaps. A
caller that returned only cells with available art would hide missing coverage
and make validation falsely optimistic.

If the camera cannot be anchored or the panorama has not observed the adjacent
area, the renderer must expose the absence of art through its defined fallback;
it must not invent level content.

## Pack-controlled gate

`set_widescreen_gate()` accepts the text of `widescreen.toml`. Rules select a
width under explicit runtime conditions, and the first matching rule wins.
The gate is evaluated early enough that the chosen width applies to the same
frame's panorama emission.

An empty document, or one without `[[widescreen]]`, disarms the gate and returns
control to the last manual `set_widescreen()` request. It does not force width
zero. This preserves behavior for packs authored before the gate existed.

Widening should normally be gated. A still menu or an unexplored edge may have
no usable panorama coverage, so enabling it globally can reveal empty space.

## Sprite behavior

Widening changes output geometry, not sprite identity. Live sprites and authored
replacements retain native-coordinate semantics and are composed relative to
the centered native frame. Focused tests cover sprites that touch or cross the
native boundary so extension logic does not clip them solely because they lie
outside the original output rectangle.

## Implemented verification

- `tests/widescreen_test.cpp` fixes target-width and extension-plan behavior.
- `tools/widescreen_spike` measures panorama coverage against a local project.
- `tools/widescreen_gate_smoke` verifies same-frame gate evaluation, repeated
  transitions, and restoration of manual control.
- `tools/widescreen_sprites_smoke` verifies sprite composition across the
  native boundary on a Vulkan device.
- `tools/widescreen_shot` renders paired off/on captures for manual seam and
  artistic review.

ROM- or project-dependent probes are local integration evidence. They cannot be
required public fixtures unless redistribution rights are documented.

## Planned HUD safe zone

Widening the background does not reposition the game's HUD. Elements anchored
to the old left or right edge can appear to float near the center of a wider
output.

The planned model treats HUD elements as existing Objects rather than creating
a parallel entity type. Each Object may carry an optional offset edited in its
normal property surface:

- left-anchored elements move toward the new left edge;
- right-anchored elements move toward the new right edge;
- centered elements may remain unchanged;
- absence of an offset is neutral and preserves current output.

The safe zone is an authoring visualization and rule boundary, not a global
translation applied to all HUD elements.

### Dynamic safe zone

A fixed rectangle is insufficient for games where the camera locks while the
player moves across the screen. The planned design composes two existing
mechanisms:

- Mode 3 supplies an anchored entity position from RAM;
- pack Lua defines how the safe zone follows it, including hysteresis or a
  fixed interval.

Game-specific camera policy belongs in the pack. The Engine must not hard-code
one heuristic for every title.

### Remaining implementation sequence

1. define the serialized safe-zone and per-Object offset contract;
2. apply offsets in rendering, with zero-offset pixel identity as the
   non-regression oracle;
3. transport the fields through pack build, parse, and compatibility checks;
4. expose authoring visualization and Object properties in the owning product;
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

Until these decisions are implemented and versioned, absence means no widening
opinion, no safe zone, and no HUD relocation.
