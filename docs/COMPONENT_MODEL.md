# Component model

**Status:** core primitives implemented; native integration present; product authoring is outside this repository

**Last verified:** 2026-08-29

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
must not silently change the underlying identity algorithm.

## Contextual resolution

Replacement rules proceed from more specific to more general. A rule may bind
identity plus position, orientation, palette, plane, or a stable palette
signature. Omitted dimensions are wildcards. The original identity-only rule
remains the compatibility fallback.

The resolver must make precedence deterministic. Competing rules at the same
specificity require a stable tie-breaker; iteration order in a hash map or TOML
parser is not a contract.

Position has different meanings by component:

- screen-space coordinates identify a visible instance for one frame;
- level-space coordinates identify background cells across camera movement;
- RAM anchors identify a game entity independently of where it is drawn;
- relative pose coordinates preserve the geometry of a multi-sprite object.

## Sprite poses and animation

`SpriteHasher` reports content identity and observed instance fields. Pose
identity preserves captured member order and optional relative geometry.

`AnimationGrouper` detects recurring member sets in a rolling observation
window. Its `anim_group_id` is useful for discovery and authoring, but it is not
a stable runtime playback key: transitions such as walk → jog → run change the
rolling set. `AnimationPlayer` therefore resolves playback by pose hash.

The current model supports:

- ordered animation frames and frame durations;
- pose-based replacement;
- geometric tweening between observed transforms;
- palette/orientation variants;
- native playback coordination through `ayther_animation.h`.

Animation remains deterministic only when clocks are expressed in emulation
frames or another explicitly recorded timeline. Wall-clock sampling must not
feed back into identity or replay state.

## Audio events

Hashing mixed PCM cannot reliably identify overlapping effects. AYTHER instead
tracks chip events per channel and derives stable signatures from the relevant
register state.

`AudioEventDetector` owns event boundaries for FM, PSG, DAC, and PCM.
`AudioSubstitutor` resolves closed or active events to pack assets. Native
coordination schedules replacements without treating a mixed output batch as
identity.

Important invariants:

- chip write cycle is excluded from identity;
- retriggers close and reopen events according to chip semantics;
- residual events require explicit initial-active state;
- channel masks reject unknown high bits;
- pitch, velocity, and volume are derived playback data, not identity;
- replacement failure preserves original audio when safe;
- sample frames, scalar samples, emulation frames, and stream positions are
  distinct units and must never be interchanged implicitly.

The Rust detector has extensive synthetic coverage. Native audio and real-core
behavior remain part of the incomplete release matrix.

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
   represent.

Repeated sky or other low-information regions may not contain enough unusual
cells to recover a unique camera position. That is an observation limit, not a
reason to fabricate an anchor.

## RAM anchoring (Mode 3)

Game profiles map RAM observations to logical entities. `ram_anchor` assigns
visible sprite instances to those entities using spatial and type constraints.
This allows a component to retain identity when its screen position, pose, or
sprite composition changes.

The profile and matcher must treat RAM and ROM-derived data as untrusted input:

- validate addresses, widths, endianness, and bounds;
- distinguish missing data from numeric zero;
- make assignment and tie-breaking deterministic;
- reject ambiguous or impossible geometry rather than silently choosing an
  arbitrary entity;
- keep game-specific heuristics in profiles or scripts, not hard-coded in the
  engine.

The repository contains `game_profile`, `ram_anchor`, the native Mode 3
contract, and a real-ROM spike. ROM-dependent probes are local integration
evidence and cannot be mandatory redistributable fixtures.

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
| Sprite and pose identity | Rust tests, KAT, production hasher equivalence | published pack migration must accompany any identity change |
| Animation | core model and native playback coordination | broader real-game transition coverage |
| Audio events | extensive chip-level tests and native wiring | full device/core/platform matrix |
| Background stitching | pure-core tests and background spike | broader camera and level-layout coverage |
| RAM anchoring | profile/matcher tests and Mode 3 spike | game-specific profiles and ambiguity coverage |
| Component serialization | native TOML helpers and pack integration | schema remains pre-release |

Historical milestone labels and one-time test counts are intentionally omitted.
Authoritative checkout maturity lives in [Project status](PROJECT_STATUS.md).

## Compatibility note: corrected SAT parsing

The current SAT parser scans all 80 slots and decodes word-swapped words
explicitly. An earlier implementation followed a cyclic link chain and decoded
individual bytes as though they were in logical big-endian order. It could
report incorrect dimensions, positions, palette, flips, priority, and tile
indices.

Correcting that defect changed sprite hashes because the engine began hashing
the graphic actually referenced by the SAT. Packs or authoring data produced
with the defective parser, including saved sprite substitutions keyed only by
those hashes, require re-observation or re-authoring. This is a data migration,
not a cosmetic documentation update.

## Out of scope

This repository does not own AYTHER Lab UI, SDK authoring workflows, Runtime,
Play, or Hub. It documents the Engine contracts those products may consume.
Nothing here authorizes bundling ROMs, emulator cores, or commercial source
assets in a pack or test fixture.
