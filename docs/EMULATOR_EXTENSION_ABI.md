# Emulator extension ABI

**Status:** ABI 1.10 implemented; integration remains pre-release

**Last verified:** 2026-08-29

AYTHER Engine can consume a versioned extension exposed by an AYTHER-aware
libretro core. The extension provides coherent snapshots, observable VDP and
audio state, controlled suppression, and optional frame recomposition. Stock
libretro cores remain valid but expose none of these capabilities.

The installed contract is
[`include/ayther/libretro_host/ayther_api.h`](../include/ayther/libretro_host/ayther_api.h).
That header is a complete copy of the fork-side contract, not an Engine-owned
subset. Do not edit it independently.

> [!WARNING]
> The extension is unstable and independently versioned from AYTHER releases,
> the flat core ABI, and the pack schema. A successful build does not establish
> behavioral compatibility with an arbitrary core binary.

> [!CAUTION]
> Emulator cores are native code outside the pack sandbox. Loading one is
> equivalent to loading an untrusted dynamic library. This documentation does
> not grant a right to distribute cores, ROMs, BIOS images, or game content.
> Apply BYOC and BYOR requirements from
> [Legal and distribution boundaries](LEGAL_AND_DISTRIBUTION.md).

## Contract provenance

The current copy identifies fork release `ayther-abi-1.10`, core version
`1.7.4`, and source commit `752a6ff7`. Keeping a local copy allows Engine and
core repositories to build independently. When updating it:

1. copy the complete fork header;
2. record the source release, core version, and commit in its preamble;
3. compile both sides and run ABI/layout oracles;
4. update this document and the protocol version table in the same change;
5. verify fallback behavior with a stock core.

Do not reconstruct the header from Engine call sites. Declaring a capability is
not the same as consuming it, and a reduced copy can conceal additive fields
needed by future integrations.

## Version negotiation

Versions encode `major << 16 | minor`; the current value is `1.10`.

- A different major version is incompatible unless an explicit adapter exists.
- Minor versions within major 1 are additive. Check that the core provides at
  least the minor version required by a feature; do not require equality.
- `abi_version` alone does not authorize a call. Check the relevant capability,
  function pointer, and containing structure size.
- A core with no extension entry point is treated as version zero with no
  capabilities, not as a load failure.

The Engine obtains `ayther_interface_v1` and validates its announced size before
reading members added by later minor versions. Functions moved into the
descriptor must be resolved there first; the legacy standalone export is only
the compatibility path for cores that predate that move.

## Capability discipline

Each optional feature requires all of the following:

1. the interface structure is large enough to contain the member;
2. the matching capability bit is present;
3. the function pointer is non-null;
4. region metadata or returned structure sizes satisfy the local contract;
5. the operation returns `AYTHER_STATUS_OK`.

Failure at any stage disables only the enhancement that needs the feature when
a safe legacy path exists. It must not be reported as successful activation.
Malformed sizes, stale snapshots, invalid writes, and incompatible layouts are
explicit integration errors.

## Coherent snapshots and generations

`capture_snapshot` produces a view of one emitted frame and identifies it with
`snapshot_generation`. Region reads associated with that snapshot pass the same
generation. A mismatch means the caller mixed data from different frames and
must discard or retry the observation.

The Engine must not infer fixed region sizes. It calls `query_region`, validates
the returned `byte_size` and element contract, allocates bounded storage, and
then calls `read_region`. This is particularly important for parsed sprites,
audio events, frame deltas, and structures whose size changed additively.

ABI 1.10 adds a system flag indicating that the geometry fields describe the
emitted frame. This prevents a mid-frame register change from being mistaken
for the geometry of the snapshot being consumed.

## Subscriptions

The Engine requests only the observation classes it consumes. Subscription
state is negotiated through `get_subscriptions` and `set_subscriptions` when the
core advertises that capability.

Do not request `AYTHER_SUB_ALL` as a convenience. Capturing unused journals,
memory regions, parsed sprites, or audio events creates measurable work in the
emulator core and makes the real dependency surface harder to audit. A missing
subscription must produce an unavailable feature or explicit fallback, never a
fabricated empty observation.

## Observation regions

The descriptor API is authoritative. Legacy IDs remain relevant to older cores
and compatibility tools:

| Legacy ID | Region |
|---:|---|
| `0x100` | CRAM |
| `0x101` | VDP registers |
| `0x102` | Layer mask control |
| `0x103` | Sprite-suppression control |
| `0x104` | Tile-suppression control |
| `0x105` | Plane-tile-suppression control |
| `0x106` | Plane-suppression activation |
| `0x107` | VSRAM |
| `0x108` | Layer dimming control |
| `0x109` | Audio writes |
| `0x10A` | Audio-write count |
| `0x10B` | Parsed sprites |
| `0x10C` | Parsed-sprite count |
| `0x10D` | Audio mute control |
| `0x10E` | Raster fallback reasons |
| `0x10F` | 8 KiB Z80 RAM, added in ABI 1.9 |

Writes target control regions and occur between frames. Writing emulated RAM or
core-owned state while the corresponding CPU is running is a data race unless
the contract explicitly provides synchronization.

## Semantic changes through ABI 1.10

Minor-version compatibility is additive at the binary surface, but several
observations became more accurate. Integrators must preserve these meanings:

- Raster fallback reasons no longer begin in a blanket unsupported state for
  Mode 4; the mask reports the actual reason.
- A journal `v_counter` identifies the first line that sees a change, normally
  `N + 1` relative to the write.
- Multilayer recomposition with CRAM events uses the corrected pixel-accurate
  output.
- Frame-delta dirty patterns and raster-event counts were corrected; consumers
  must not depend on the earlier always-empty or fixed-count behavior.
- Multilayer recomposition moved into the descriptor in ABI 1.2. Resolve the
  descriptor member first and use the standalone symbol only for 1.0/1.1.
- Audio-event schema 2 carries PCM sample start and loop-start data. The core
  reports facts; AYTHER Core remains responsible for computing pack identity.
- ABI 1.9 adds Z80 RAM and expands raster-journal capacity. Positive fallback
  masks remain fallback, not success.
- ABI 1.10 ties system geometry to the emitted frame through an explicit flag.

Structures that announce `event_size`, `struct_size`, or equivalent metadata
must be traversed using that size. Do not substitute a local `sizeof` when a
new ABI revision can extend or reorganize a record.

## Engine consumption rules

`RetroRunner` owns extension discovery and low-level region access.
`AytherSession` consumes validated observations and exposes enhancement state in
`FrameView`.

- Keep extension pointers borrowed from the loaded core; they become invalid
  when the core unloads.
- Perform version and capability checks at the boundary, not repeatedly in
  rendering or identity code.
- Preserve the stock-core path. Missing AYTHER observations may reduce
  fidelity, but must not corrupt native emulator output.
- Never treat an empty observation returned after an error as a valid empty
  frame.
- Log the negotiated version, capability mask, requested subscriptions, and
  fallback reason for diagnostics without exposing ROM or user-content data.

## Verification expectations

Release-grade verification requires:

- header layout and size assertions on both sides;
- negotiation tests for absent, older, current, truncated, and future-minor
  descriptors;
- stale-generation and undersized-buffer rejection;
- subscription and capability fallbacks;
- frame-delta and multilayer recomposition oracles;
- real-core integration on every supported platform;
- stock-core execution proving that optional observations remain optional.

The current repository contains substantial integration code and probes, but
the cross-platform and real-core matrix remains incomplete. ABI 1.10 support is
implemented; it is not yet a stable compatibility promise.
