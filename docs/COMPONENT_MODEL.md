# Component model

**Status:** core primitives implemented; native integration present; product authoring is outside this repository

**Last verified:** 2026-11-19

AYTHER turns atomic observations into logical components that persist across
space and time. A tile, sprite, or chip write answers what appeared in one
observation. A component answers what that observation belongs to and which
replacement behavior should apply.

> [!WARNING]
> This is pre-release behavior. Rust primitives and C++ wiring exist, but
> real-game, cross-platform, GPU, and authoring-workflow coverage is incomplete.
> The presence of a header or target is not by itself a stability claim.

The exact content identities are defined in
[Pack identity specification](IDENTITY_SPECIFICATION.md). This document owns
grouping, contextual resolution, and native orchestration.

## Model layers

| Layer | Purpose | Representative implementation |
|---|---|---|
| Observation | Atomic fact emitted by emulation | tile/sprite occurrence, chip write, RAM sample |
| Identity | Deterministic content key | sprite hash, plane-tile hash, pose key, audio signature |
| Context | Optional dimensions that disambiguate identity | position, plane, palette, orientation, RAM anchor, conditions |
| Component | Logical unit spanning observations | animation, audio event, background, anchored actor |
| Replacement | Pack-selected HD behavior | image, clip, audio asset, transform, composed layer |

Identity remains content-based and deterministic. Context narrows selection but
must not change the underlying identity algorithm.

## Emulation providers

Observations reach the component layer through the `ObservationSource`
abstraction (`include/ayther/observation/observation_source.h`). The libretro
provider (`RetroRunner`) and the optional `tessera_native` provider
(`TesseraBackend`, Mega Drive) implement it over the same observation ABI, so
every primitive in this document runs unchanged on either provider and yields
the same identities. Work RAM for RAM-based components comes from the core's
`SYSTEM_RAM` on libretro and from a session mirror of Tessera region `0x1000`
(same word-swapped layout) on `tessera_native`. Provider limitations are listed
in [Tessera native provider](TESSERA_NATIVE.md).

## Contextual resolution

Replacement rules proceed from more specific to more general. A rule may bind
identity plus position, orientation, palette, plane, or a stable palette
signature. Omitted dimensions are wildcards. The identity-only rule remains the
compatibility fallback.

Precedence is deterministic. Competing rules at the same specificity require a
stable tie-breaker; iteration order in a hash map or TOML parser is not a
contract.

Position has different meanings by component:

- screen-space coordinates identify a visible instance for one frame;
- level-space coordinates identify background cells across camera movement;
- RAM anchors identify a game entity independently of where it is drawn;
- relative pose coordinates preserve the geometry of a multi-sprite object.

## Sprite poses and animation

`SpriteHasher` reports content identity and observed instance fields. Pose
identity preserves captured member order and the flip of each member relative
to the others.

`AnimationGrouper` detects recurring member sets in a rolling observation
window. Its `anim_group_id` is useful for discovery and authoring, but it is not
a stable runtime playback key: transitions such as walk → jog → run change the
rolling set. `AnimationPlayer` (`ayther_animation.h`) therefore resolves
playback by pose hash.

The model supports:

- ordered animation frames and frame durations;
- pose-based replacement;
- geometric tweening between observed transforms;
- palette/orientation variants;
- native playback coordination through `ayther_animation.h`.

Animation is deterministic only when clocks are expressed in emulation frames
or another explicitly recorded timeline. Wall-clock sampling must not feed back
into identity or replay state.

## Audio events

Hashing mixed PCM cannot reliably identify overlapping effects. AYTHER instead
tracks chip events per channel and derives stable signatures from the relevant
register state.

`AudioEventDetector` owns event boundaries for FM, PSG, DAC, and PCM.
`AudioSubstitutor` resolves closed or active events to pack assets. Native
coordination schedules replacements without treating a mixed output batch as
identity.

Invariants:

- chip write cycle is excluded from identity;
- retriggers close and reopen events according to chip semantics;
- residual events require explicit initial-active state;
- channel masks reject unknown high bits;
- pitch, velocity, and volume are derived playback data, not identity;
- replacement failure preserves original audio when safe;
- sample frames, scalar samples, emulation frames, and stream positions are
  distinct units and must never be interchanged implicitly.

The live detector produces the same voices and FM/PSG signatures on both
providers. Pack-level continuity fields are specified in
[Audio continuity pack format](AUDIO_CONTINUITY_FORMAT.md).

## Backgrounds

`BackgroundStitcher` accumulates observed plane cells in level space. It keeps
enough history to produce a strip wider than the nametable, records conflicts,
and classifies cells that change at the same level coordinate.

`ScrollUnwrapper` converts wrapped VDP scroll values into a continuous camera
coordinate. A caller supplies the wrap period; jumps near the boundary choose
the nearest continuous interpretation rather than resetting the level origin.

These primitives are pure core logic. Native export and rendering are separate
steps:

1. observe plane cells and scroll;
2. unwrap the camera trajectory;
3. stitch cells in level space;
4. export or package the authored strip;
5. render the panorama at the plane's composition depth;
6. use the strip to fill widescreen areas that the wrapped nametable cannot
   represent (see [Widescreen composition](WIDESCREEN.md)).

The session exposes this pipeline through `bg_capture`, `bg_bounds`,
`bg_cells`, `export_background_plane`, and `define_panorama`; both providers
produce the same strips and panorama anchors.

Repeated sky or other low-information regions may not contain enough unusual
cells to recover a unique camera position. That is an observation limit, not a
reason to fabricate an anchor.

## Plane-set substitution

Plane-set substitution matches a Drawing's relative cell layout within any one
of the three VDP planes. Capture-plane metadata is provenance, not part of that
match.

- Visible members must all be present in the same plane; members outside the
  viewport are excused.
- Cells claimed by higher-ranked content cannot participate.
- Sets are tried by complexity: more members first, then larger bounding box,
  then id. The most specific Drawing claims first and the order is total
  (`src/session/plane_set_order.h`).
- Each frame, the visible cells are bucketed by hash once; an anchor only
  visits cells with its own hash, in the same order as a full scan, so the
  resulting substitutions are identical.

The resulting overlay uses the occurrence's plane and its anchor's priority, so
matching and composition refer to the same visible Drawing.

## RAM anchoring (Mode 3)

Game profiles map RAM observations to logical entities. `ram_anchor` assigns
visible sprite instances to those entities using spatial and type constraints.
This allows a component to retain identity when its screen position, pose, or
sprite composition changes.

The profile and matcher treat RAM and ROM-derived data as untrusted input:

- validate addresses, widths, endianness, and bounds;
- distinguish missing data from numeric zero;
- make assignment and tie-breaking deterministic;
- reject ambiguous or impossible geometry rather than silently choosing an
  arbitrary entity;
- keep game-specific heuristics in profiles or scripts, not hard-coded in the
  engine.

The repository contains `game_profile`, `ram_anchor`, the native Mode 3
contract (`ayther_mode3.h`), and a real-ROM spike (`tools/mode3_spike`).
ROM-dependent probes are local integration evidence and cannot be mandatory
redistributable fixtures.

## ABI and ownership boundary

Core handles are opaque, single-threaded by default, and released through their
matching destructor functions. C++ owners wrap them in `unique_handle` rather
than storing loose raw pointers. Shared structures use fixed-width fields and
`repr(C)` layouts.

Any layout change must update, in one change:

1. the Rust definition;
2. the public C declaration;
3. the CXX bridge where applicable;
4. compile-time size and offset assertions;
5. serialization and compatibility tests;
6. the documented ABI revision when the public boundary changes.

No Rust panic or C++ exception may unwind across the binary boundary. Expected
failures use explicit result/status values; cleanup paths are non-throwing.

## Current maturity

| Capability | Current evidence | Remaining limitation |
|---|---|---|
| Sprite and pose identity | Rust tests, KAT, production hasher equivalence, libretro/Tessera identity parity | published pack migration must accompany any identity change |
| Animation | core model and native playback coordination | broader real-game transition coverage |
| Audio events | chip-level tests, native wiring, live-detector parity across providers | full device/core/platform matrix |
| Background stitching | pure-core tests, background spike, provider parity in `tessera_native_subsystems` | broader camera and level-layout coverage |
| Plane sets | ordering and overlap tests, cross-provider pack parity | broader real-game coverage |
| RAM anchoring | profile/matcher tests and Mode 3 spike | game-specific profiles and ambiguity coverage; no dedicated `tessera_native` test |
| Component serialization | native TOML helpers and pack integration | schema remains pre-release |

Checkout-level maturity lives in [Project status](PROJECT_STATUS.md).

## Out of scope

This repository does not own AYTHER Lab UI, SDK authoring workflows, Runtime,
Play, or Hub. It documents the Engine contracts those products may consume.
Nothing here authorizes bundling ROMs, emulator cores, or commercial source
assets in a pack or test fixture.
