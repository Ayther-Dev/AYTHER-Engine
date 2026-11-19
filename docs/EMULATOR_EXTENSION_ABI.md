# Emulator extension ABI

**Status:** ABI 1.10 implemented; pre-release, not a stable compatibility
promise.

AYTHER Engine can consume a versioned extension exposed by an AYTHER-aware
libretro core. The extension provides coherent snapshots, observable VDP and
audio state, controlled suppression, and optional frame recomposition. Stock
libretro cores remain valid but expose none of these capabilities.

This contract applies to the **libretro provider** only. The native Tessera
provider is reached through the Tessera C API (1.1 or later), whose
observation calls have the same shape as `ayther_interface_v1` with the session
as first parameter. Inside Engine both are read through one private
`ObservationSource` interface (`include/ayther/observation/observation_source.h`),
so the rules below about generations, region sizes, subscriptions, and
capability checks hold for both. Native-provider specifics are in
[Tessera native provider](TESSERA_NATIVE.md).

The contract header is
[`include/ayther/libretro_host/ayther_api.h`](../include/ayther/libretro_host/ayther_api.h).
It is a complete copy of the fork-side contract, not an Engine-owned subset. Do
not edit it independently.

> [!WARNING]
> The extension is versioned independently from AYTHER releases, the flat core
> ABI, and the pack schema. A successful build does not establish behavioral
> compatibility with an arbitrary core binary.

> [!CAUTION]
> Emulator cores are native code outside the pack sandbox. Loading one is
> equivalent to loading an untrusted dynamic library. This documentation does
> not grant a right to distribute cores, ROMs, BIOS images, or game content.
> Apply BYOC and BYOR requirements from
> [Legal and distribution boundaries](LEGAL_AND_DISTRIBUTION.md).

## Contract provenance

The current copy identifies fork release `ayther-abi-1.10`, core version
`1.7.4`, and source commit `752a6ff7`. Keeping a local copy lets Engine and the
core build independently. When updating it:

1. copy the complete fork header;
2. record the source release, core version, and commit in its preamble;
3. compile both sides and run the ABI and layout tests;
4. update this document and the version table in
   [API and compatibility](API_COMPATIBILITY.md) in the same change;
5. verify fallback behavior with a stock core.

Do not reconstruct the header from Engine call sites. Declaring a capability is
not the same as consuming it, and a reduced copy can hide additive fields that
later integrations need.

## Version negotiation

Versions encode `major << 16 | minor`; the current value is `1.10`
(`AYTHER_ABI_VERSION_LATEST`).

- A different major version is incompatible unless an explicit adapter exists.
- Minor versions within major 1 are additive. Check that the core provides at
  least the minor version a feature requires; do not require equality.
- `abi_version` alone does not authorize a call. Check the capability bit, the
  function pointer, and the containing structure size.
- A core with no extension entry point is treated as version zero with no
  capabilities, not as a load failure.

Engine obtains `ayther_interface_v1` and validates its announced size before
reading members added by later minor versions. Functions moved into the
descriptor are resolved there first; the standalone export is only the
compatibility path for cores that predate the move.

The [Core probe contract](CORE_PROBE_CONTRACT.md) is stricter than runtime
negotiation: a core is accepted for AYTHER only when it announces ABI 1.10 or
later and every capability in `kRequiredCapabilities`.

## Capability discipline

Each optional feature requires all of the following:

1. the interface structure is large enough to contain the member;
2. the matching `AYTHER_CAP_*` bit is present;
3. the function pointer is non-null;
4. region metadata or returned structure sizes satisfy the local contract;
5. the operation returns `AYTHER_STATUS_OK`.

Failure at any stage disables only the enhancement that needs the feature when
a safe legacy path exists. It is never reported as successful activation.
Malformed sizes, stale snapshots, invalid writes, and incompatible layouts are
explicit integration errors.

## Coherent snapshots and generations

`capture_snapshot` produces a view of one emitted frame and identifies it with
`snapshot_generation`. Region reads associated with that snapshot pass the same
generation. A mismatch means the caller mixed data from different frames and
must discard or retry the observation.

Engine does not infer fixed region sizes. It calls `query_region`, validates
the returned `byte_size` and element contract, allocates bounded storage, and
then calls `read_region`. This matters for parsed sprites, audio events, frame
deltas, and structures whose size grew additively.

## Subscriptions

Engine requests only the observation classes it consumes, through
`get_subscriptions` and `set_subscriptions` when the core advertises that
capability.

Do not request `AYTHER_SUB_ALL` as a convenience. Capturing unused journals,
memory regions, parsed sprites, or audio events costs work in the emulator and
hides the real dependency surface. A missing subscription produces an
unavailable feature or an explicit fallback, never a fabricated empty
observation.

## Observation regions

The descriptor API is authoritative. Legacy IDs (`AYTHER_LEGACY_MEMORY_*`)
remain relevant to older cores and compatibility tools:

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
| `0x10E` | Raster dirty / fallback reasons |
| `0x10F` | Z80 RAM (8 KiB), since ABI 1.9 |

Writes target control regions and happen between frames. Writing emulated RAM
or core-owned state while the corresponding CPU runs is a data race unless the
contract provides synchronization.

## Semantic changes through ABI 1.10

Minor versions are additive at the binary surface, but several observations
became more accurate. Integrators must preserve these meanings:

- Raster fallback reasons (`0x10E`) no longer start in a blanket unsupported
  state for Mode 4; the mask reports the actual reason. Positive fallback masks
  remain fallback, not success.
- A journal `v_counter` identifies the first line that sees a change, normally
  `N + 1` relative to the write.
- Multilayer recomposition with CRAM events produces the corrected
  pixel-accurate output.
- Frame-delta dirty patterns and raster-event counts are corrected; consumers
  must not depend on the earlier always-empty or fixed-count behavior.
- Multilayer recomposition lives in the descriptor since ABI 1.2. Resolve the
  descriptor member first; use the standalone symbol only for 1.0 and 1.1.
- Audio-event schema 2 carries PCM sample start and loop-start data. The core
  reports facts; AYTHER Core computes pack identity.
- ABI 1.9 adds Z80 RAM and expands raster-journal capacity.
- ABI 1.10 makes the system descriptor describe the emitted frame:
  `viewport_w`, `viewport_h`, `interlace`, and `h40` refer to the frame being
  delivered, and the flag `AYTHER_SYSTEM_GEOMETRY_PENDING` signals that the
  registers already changed the geometry for the next frame. The former
  reserved byte became `flags` at the same offset and size.

Structures that announce `event_size`, `struct_size`, or equivalent metadata
are traversed using that size. Do not substitute a local `sizeof` when a new
revision can extend a record.

## Engine consumption rules

`RetroRunner` owns extension discovery and low-level region access through the
`ObservationSource` interface. `AytherSession` consumes validated observations
and exposes enhancement state in `FrameView`.

- Extension pointers are borrowed from the loaded core and become invalid when
  it unloads.
- Version and capability checks happen at the boundary, not repeatedly in
  rendering or identity code.
- The stock-core path is preserved. Missing observations may reduce fidelity
  but never corrupt native emulator output.
- An empty observation returned after an error is not a valid empty frame.
- Diagnostics log the negotiated version, capability mask, requested
  subscriptions, and fallback reason without ROM or user-content data.

## Verification

In the repository:

- the `abi_frame_delta`, `abi_multilayer`, `abi_read_parity`,
  `abi_write_control`, and `ayther_abi_smoke` tool tests exercise negotiation
  and region access against the in-repository test cores, one with the
  extension (`ayther_test_core`) and one without (`ayther_test_core_stock`);
  `AYTHER_ABI_CORE` and `AYTHER_STOCK_CORE` point them at a real fork core;
- the core probe vectors cover stock, partial, and complete cores.

Still required for a stable promise: negotiation tests for truncated and
future-minor descriptors, stale-generation and undersized-buffer rejection on
every path, and real-core integration on every supported platform.
