# Tessera native provider

**Status:** implemented, optional, off by default; Windows x64 only

This is the authoritative guide to the Tessera integration: how to build Engine
with it, which Tessera it accepts, how a host selects it, what works through it,
how it is packaged, and how it is tested.

[AYTHER Tessera](https://github.com/Ayther-Dev/AYTHER-Tessera) is AYTHER's own
clean-room emulator core, developed and released from its own repository. Engine
can use it as a **native emulation provider**, an alternative to loading a
user-supplied libretro core. The supported native machine is the Sega Mega
Drive / Genesis (Tessera backend `megadrive`). Tessera also ships synthetic
backends (`synthetic.*`) that Engine uses for contract tests.

## How it fits

- Engine depends on Tessera; Tessera does not depend on Engine. Engine links
  only an **installed** `Tessera::core` package through the Tessera C API. It
  never builds or downloads Tessera itself.
- `TesseraBackend` is private to Engine. No installed Engine header includes a
  Tessera, libretro, or backend header.
- Engine reads every emulator through one internal `ObservationSource`
  interface. `RetroRunner` implements it from a libretro core's
  `ayther_interface_v1` descriptor ([emulator extension ABI](EMULATOR_EXTENSION_ABI.md)
  1.10); `TesseraBackend` implements it from a native session over Tessera API
  1.1, which exposes the same primitives. A `FrameView` is therefore built by the
  same code from either provider.
- With the native provider, a Mega Drive ROM runs through the same frame
  pipeline as with a libretro core and yields the same identities as the
  Genesis Plus GX reference core (see [Testing and parity](#testing-and-parity)).

See [Architecture](ARCHITECTURE.md#emulation-providers) for the provider layer
in context.

## Build configuration

| CMake variable | Default | Meaning |
|---|---|---|
| `AYTHER_ENABLE_TESSERA` | `OFF` | Build the native provider. Requires `AYTHER_BUILD_ENGINE` (`TESSERA_ENGINE_REQUIRED`) |
| `Tessera_DIR` | — | `<prefix>/lib/cmake/Tessera` of an installed Tessera package |
| `AYTHER_TESSERA_ALLOW_UNLOCKED` | `OFF` | Accept a package other than the locked one. Local development only; CI never sets it |
| `AYTHER_TESSERA_TEST_SOURCE_DIR` | — | Tessera source checkout at the locked commit; required for the native tests (test-only headers and CLI fixtures) |
| `AYTHER_TESSERA_TEST_FIXTURE_DIR` | `<source>/build/windows-dev/native-fixtures` | Native fixtures generated in the Tessera checkout; CTest never downloads them |
| `AYTHER_TESSERA_PHASE3_EVIDENCE_DIR` | — | Optional Tessera Mega Drive replay evidence; enables `ayther.integration.tessera_checkpoint` |

```powershell
cmake --preset windows-native-vpx `
  -DAYTHER_ENABLE_TESSERA=ON `
  "-DTessera_DIR=<tessera-prefix>/lib/cmake/Tessera"
```

Platform constraints:

- Tessera packages are built for Windows x64 with the MSVC ABI and the `/MD`
  runtime; there is no Linux Tessera provider.
- Engine and Tessera must agree on AddressSanitizer: a diagnostics (ASan)
  Tessera package requires `AYTHER_ENABLE_ASAN=ON` and vice versa, or configure
  fails.
- No preset enables Tessera; pass the options explicitly.

With `AYTHER_ENABLE_TESSERA=OFF` the libretro provider is unchanged, and the
native entry points are stubs that return `NativeStatus::unsupported_capability`.

## Version requirement and lock

Configure accepts a package that declares **Tessera API 1.1 or a later 1.x**
(`Tessera_ABI_MAJOR` / `Tessera_ABI_MINOR` in `TesseraConfig.cmake`). An older
API fails with `TESSERA_API_TOO_OLD`. The package version (`0.x`) is not
constrained, because Tessera release tags move independently of its API.

On top of the API floor, [`dependencies/tessera.lock.json`](../dependencies/tessera.lock.json)
pins the exact Tessera:

| Field | Meaning |
|---|---|
| `sourceCommit` | Full Tessera commit the package is built from |
| `api.major`, `api.minor` | API the package declares |
| `package.version` | Package version |
| `release.tag`, `release.status` | Release tag, or `unreleased` while the pinned commit has no tag |
| `artifact.fileName`, `artifact.sha256` | Released package and its SHA-256 |

The lock currently pins Tessera `v0.2.0-rc.9` (API 1.1). Configure compares the
installed package (`source.revision`, version, API) with the lock and fails with
`TESSERA_LOCK_MISMATCH` when they differ; other lock errors are
`TESSERA_LOCK_MISSING`, `TESSERA_LOCK_INVALID`, and `TESSERA_LOCK_API`.

CI uses the lock in two places:

- `ci.yml` repeats the commit as the literal `TESSERA_COMMIT`, so the checkout
  ref does not come from a repository file, and its *Verify the Tessera pin*
  step fails when the literal and the lock differ.
- `native-release.yml` reads the commit from the lock.

### Bumping Tessera

Change the lock in the same pull request as the code that needs the new
Tessera:

1. Update `sourceCommit`, `api`, `package.version`, `release.*`, and
   `artifact.*` in `dependencies/tessera.lock.json`.
2. Update `TESSERA_COMMIT` in `.github/workflows/ci.yml` to the same commit.
3. Check an installed package against the lock:

   ```powershell
   cmake -DAYTHER_TESSERA_LOCK=dependencies/tessera.lock.json `
         -DAYTHER_TESSERA_PREFIX=<tessera-prefix> -P tools/check_tessera_lock.cmake
   ```

4. Run the native test suite (below). The `ayther.package.tessera_lock.*` tests
   check the lock itself and that the verifier rejects a different commit,
   version, tag, or API.

## Selecting the provider

A host selects the provider per session:

```cpp
ayther::AytherSession::Config configuration;
configuration.provider = ayther::EmulationProvider::tessera_native;
configuration.native_config = native_configuration; // engine::NativeConfig
ayther::engine::NativeResult preparation;
auto session = ayther::AytherSession::create(configuration, preparation);
if (!session) {
  // Keep preparation.code, phase, frame_id, and transaction for diagnostics.
}
```

`native_configuration` names the backend and carries the content bytes, the
canonical configuration, and the initial persistent memory. The configuration
includes the backend build ID enumerated by `NativeEmulation::backends`;
content detection and its evidence come from `NativeEmulation::detect`. The
session verifies the selection and reports system, capabilities, and identities
through `info()`.

An invalid selection, incompatible content or configuration, or a failed
preparation returns an error. Engine **never falls back** to the libretro
provider.

These `Config` options have no native counterpart and are rejected at creation
with `unsupported_capability`: `patch_path`, `core_options`, `rom_path`, and the
`audio_observer` and `render_observer` callbacks.

AYTHER Runtime exposes the same choice on its command line
(`--provider tessera`); see the Runtime documentation.

### Mega Drive configuration

A Mega Drive session uses system 1 (`TESSERA_SYSTEM_MEGA_DRIVE`), 48 kHz audio,
and a map of two twelve-control pads (configuration field 6). The ROM is passed
as bytes (`content`), never as a path. `tools/parity_bench/native_config.h` and
`tests/support/tessera_native_config.hpp` build this configuration from a ROM
and are the reference to copy.

### Driving a native session

There are two ways to advance the machine:

- **Session facade.** `set_input()` stores the RetroPad state of each port, and
  `step()` advances the native machine through the normal frame pipeline
  (`produce_frame`), exactly as with libretro. On each step the session assigns
  every control the configuration's map declares, with the Genesis Plus GX
  mapping: Mega Drive A, B, C ← RetroPad Y, B, A, plus the directions and Start.
  Mega Drive X, Y, Z, and Mode stay released. If the advance fails, `step()`
  reports the code in `FrameView::native_result`.
- **Typed driver.** `native_emulation()` returns the `engine::NativeEmulation`
  driver, which takes complete control states with epoch and frame context. See
  [Typed driver: originals and ownership](#typed-driver-originals-and-ownership).

In the frame pipeline the `megadrive` frame is delivered as RGB565, the format
Genesis Plus GX produces, so the tile hasher, sprite judge, and raster
recomposition see the same pixels on both providers.

Work RAM comes from Tessera region `0x1000` in the libretro `SYSTEM_RAM` layout
(word-swapped). The session keeps a mirror that it refreshes after each advance,
restore, reset, or close. `poke()` and Engine cheats (`add_cheat`) write through
`write_control`, so a write is machine state and checkpoints carry it. Core
cheat codes (`cheat_set`) are a libretro feature and do not exist on this path.

Hosts must keep every `NativeResult`: each later query has its own result. The
`create` overload that takes a `NativeResult` preserves the context of a
failure that produced no session. The facade's `reset()` delegates to the
driver, and observer subscriptions are reapplied after a native reset.

## Capabilities

| Capability | Native behavior |
|---|---|
| Packs | `Config::pack_path`, `set_pack()`, and `reload_pack()` work as with libretro; `validate_pack()` reports platform `megadrive` and the ROM CRC32. Packless sessions initialize their pack consumers too |
| Substitutions | A pack baked on Genesis Plus GX plays on Tessera, and vice versa, with the same per-frame multiset of substitutions |
| Identities | Sprites, tiles, plane cells, attribution, and sound events match Genesis Plus GX ([identity specification](IDENTITY_SPECIFICATION.md)) |
| Backgrounds and panoramas | The background stitcher (`bg_capture`, `bg_bounds`, `bg_cells`, `export_background_plane`) and panorama anchoring (`define_panorama`) match Genesis Plus GX |
| Lua | `load_script()` works; `ayther.ram.*` reads the work RAM mirror, `ayther.tiles.list()` and `ayther.sprites.list()` the frame's occurrences |
| Live audio detector | `audio_live_active()` reports the same voices and FM/PSG signatures, frame by frame |
| Rewind | `enable_rewind()` keeps a window of 10 s × fps of zstd-compressed driver checkpoints; `rewind_step()` restores them. Memory is tested to stay within 1.25× that of Genesis Plus GX |
| Takes | `record_start()` writes `.arp` takes with emulation generation `tessera` and a keyframe every 300 frames; `replay_seek()`, per-channel capture, and take analysis work on them |
| Checkpoints | `serialize_size()` and `unserialize()` use driver checkpoints |
| Device audio | `enable_audio` opens the player; native 48 kHz PCM is resampled to the player's 44.1 kHz with phase carried across frames, so there are no seams at frame boundaries |
| Channel mute | `AUDIO_MUTE` isolates each channel: `capture_channel_pcm()` with the FM 1 mask returns only that voice |
| Not available | `patch_path`, `core_options`, `rom_path`, `audio_observer`, `render_observer` (rejected at creation) |

## Typed driver: originals and ownership

After an advance through the typed driver, `native_frame()` refreshes the
borrowed originals without copying pixels or PCM:

- `pixel_encoding = rgba8` and `fb_format = -1` distinguish RGBA8 from the
  libretro formats.
- `native_original` carries S16LE PCM with its real rate, channel count, sample
  count, ticks, and identity. It is not sent to the 44.1 kHz device player.
- With a pack and a confirmed presentation policy, the frame is composed with
  the pack's substitutions. Otherwise `native_original_reason` says why the
  original is shown (`no_pack`, `policy_unconfirmed`, `capture_lost`,
  `temporal_incomplete`, and others in `NativeOriginalReason`).

Ownership rules:

- Borrowed originals expire at the next successful advance, reset, restore, or
  close. Failed operations keep the current originals; with no current
  originals the facade returns an empty view with the matching result.
- `NativeSnapshot` holds an independent reference and may outlive the session.
  Retain a further reference before handing one to another reader; each wrapper
  object has a single owner.
- Memory is released through the API of the module that allocated it. Queues
  and budgets are never enlarged.
- `CONTROL_PENDING`, `END_OF_TRACE`, confirmation, and cancellation keep their
  code, frame and epoch, transaction, and cause, independently of event-queue
  space. End of trace pauses without advancing or applying a pending control.
  A failed reset keeps the session and its historical captures.

## Release packaging

The regular release packages (`ayther-engine` and `ayther-engine-vpx`, tags
`v0.1.0-rc.N`) do **not** contain the Tessera provider.

`.github/workflows/native-release.yml` builds a separate candidate for each tag
`native-v0.1.0-rc.N`:

- Windows x64, VPX enabled, Tessera built from the locked commit;
- asset `ayther-engine-vpx-native-v0.1.0-rc.N-windows-x86_64.zip` plus
  `CHECKSUMS.sha256`, with a build-provenance attestation;
- created as a **draft** pre-release and published once AYTHER Runtime pins it.
  A draft that Runtime never pinned stays a draft.

Release mechanics shared with the regular packages are in
[Build, test, and release](BUILD_TEST_RELEASE.md).

## Testing and parity

The native tests register in the normal CTest graph when
`AYTHER_ENABLE_TESSERA=ON`. The product links the installed package; the Tessera
source checkout supplies only test headers and CLI fixtures.

| Test | Covers |
|---|---|
| `ayther.package.tessera_lock.*` | Lock format and verifier rejections |
| `ayther.contract.tessera_observation_source` | `ObservationSource` over Tessera |
| `ayther.integration.tessera_session_test` | `AytherSession` driven through its public typed driver |
| `ayther.integration.tessera_native_replay` | Fresh-process replays compared with the standalone Tessera CLI |
| `ayther.integration.tessera_native_md` | Mega Drive through the frame pipeline |
| `ayther.integration.tessera_controls_test` | Control mapping |
| `ayther.integration.tessera_pack_test`, `tessera_presentation_test` | Pack composition and presentation policy |
| `ayther.integration.tessera_native_subsystems` | Backgrounds, panoramas, Lua, live audio detector |
| `ayther.integration.tessera_native_state` | Rewind, takes, device audio, `AUDIO_MUTE` |
| `ayther.integration.tessera_checkpoint` | Checkpoints against Tessera replay evidence (when supplied) |
| `ayther.integration.parity_bench_cross_pack` | Packs baked on one provider and played on the other |

`tessera_native_subsystems` and `tessera_native_state` run on a synthetic ROM
written by the test (`tools/common/synth_rom.h`). When the Genesis Plus GX fork
core named by `third_party/cores/core.lock` is available (or
`AYTHER_PARITY_GPGX_CORE` points to it), they repeat the series on that core and
compare. Some tests return CTest skip code 77 when an optional input, such as a
Tessera backend or that core, is missing.

In CI, the Windows native job builds and installs Tessera from the locked
commit, generates its fixtures, configures Engine with the provider, and runs
these tests; it also downloads the locked core, verified by SHA-256, for the
cross-pack test.

### Parity bench

`tools/parity_bench` (`AYTHER_BUILD_PARITY_BENCH=ON`) compares the two providers.
`parity_bench --help` lists the full syntax.

| Subcommand | Purpose |
|---|---|
| `dump` | Write the `FrameView` dump of a session (`--provider libretro\|tessera_native`); `--identities` also writes canonical identities per segment |
| `compare` | Compare two dumps; report the first divergence per subplane |
| `check` | Strict exception accounting over a catalog of dumps: fails on a divergence no declared exception covers, and on a declared exception that no longer diverges |
| `compare-identities` | Compare two identity files (exit 0 equal, 5 different) |
| `cross-pack bake\|play\|compare` | Bake a signed pack from one provider's identities (`--shard K/N` splits large games), play it on either provider, and compare per-frame substitutions |
| `trace-from-arp` | Convert a take's inputs to a Tessera trace |

The Tessera repository publishes the exception registry that `check --registry`
reads.
