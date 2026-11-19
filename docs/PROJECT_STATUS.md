# Project status

**Assessment date:** 2026-11-19

**Baseline:** `main` at `6d1c2e8`, tagged `v0.1.0-rc.28`

**Release status:** pre-release candidates only; no stable or supported release

This document is the authoritative statement of what AYTHER Engine can do at the
baseline above. It separates what is implemented here from what is specified but
not present and from planned work. Per-configuration test evidence is in the
[support matrix](SUPPORT_MATRIX.md); the release decision is in the
[current go/no-go record](RELEASE_GO_NO_GO_CURRENT.md); change history is in
the [changelog](../CHANGELOG.md).

## Summary

The Rust core, the C++ engine, the installable CMake package, and the release
automation are implemented and exercised in CI on Windows and Linux x86_64.
Engine can run a game through a user-supplied libretro core or, in an optional
Windows build, through the Tessera native provider. Signed pre-release packages
are published and consumed by AYTHER Runtime.

The checkout is suitable for development and for integration by first-party
products that lock a release candidate. It is not a stable release and carries
no compatibility promise to third-party integrators beyond the `0.1.x` rules in
[API and compatibility](API_COMPATIBILITY.md).

## Releases

| Item | State |
|---|---|
| Product version | `0.1.0` (Cargo, CMake, vcpkg, SDK headers) |
| Latest pre-release | [`v0.1.0-rc.28`](https://github.com/Ayther-Dev/AYTHER-Engine/releases/tag/v0.1.0-rc.28): `ayther-engine` and `ayther-engine-vpx` for Windows and Linux x86_64, with SPDX SBOMs, SHA-256 checksums, Sigstore bundles, and SLSA provenance |
| Native candidate | [`native-v0.1.0-rc.28`](https://github.com/Ayther-Dev/AYTHER-Engine/releases/tag/native-v0.1.0-rc.28): Windows x64 VPX package with the Tessera provider, published because AYTHER Runtime pins it |
| Tags without a regular release | `v0.1.0-rc.26` and `v0.1.0-rc.27`: the release workflow failed; superseded by `rc.28`. Their native candidates remain drafts |
| Downstream | AYTHER Runtime `v0.1.0-beta.18` locks `rc.28` and selects Tessera with `--provider tessera` |

Release candidates are produced by `release.yml` and `native-release.yml`; the
procedure is in [Build, test, and release](BUILD_TEST_RELEASE.md).

## Capabilities

### Implemented

- Tile, sprite, pose, animation, background, plane, and audio identities and
  substitution, specified bit-exactly in the
  [identity specification](IDENTITY_SPECIFICATION.md).
- RAM conditions, AOB scanning, entity anchoring, and game profiles.
- `.ay` packs: lazy archive reads, integrity indexes, canonical paths and ZIP
  resource limits, manifest schema handling, profiles, tiers, regional
  resolution, credits, compatibility reports, and explicit trust registries with
  scope, validity, and revocation ([pack security model](PACK_SECURITY_MODEL.md)).
- Lua 5.4 scripting with instruction and memory limits; in-memory IPS/BPS
  patching; SoundFont conversion, synthesis, and instrument mapping.
- The C++ session (`AytherSession`), Vulkan renderer, SDL audio, rewind, `.arp`
  takes, audio continuity, cinematic planes, widescreen, render observation
  (C3), visual state (C4), and replacement composition rules R1–R9.
- Two emulation providers behind one `ObservationSource`
  ([Architecture](ARCHITECTURE.md#emulation-providers)):
  - libretro, with the AYTHER [emulator extension ABI](EMULATOR_EXTENSION_ABI.md)
    1.10 for full observation;
  - Tessera native, Mega Drive only, Windows x64 only
    ([Tessera native provider](TESSERA_NATIVE.md)).
- `tools/parity_bench`: native-versus-libretro parity checks, identity
  comparison, and cross-provider pack bake/play/compare.
- Installed package `find_package(Ayther)` with `Ayther::core`,
  `Ayther::engine`, and `Ayther::ymfm`; VPX builds add `Ayther::vpx`.
- Runtime-facing version and compiled-capability probes owned by Engine.

### Specified but not present

- Native-provider equivalents of `patch_path`, `core_options`, `rom_path`, and
  the `audio_observer` and `render_observer` callbacks; a native session rejects
  them with `unsupported_capability`.
- A Tessera provider outside Windows x64, and Tessera inside the regular
  release packages.
- An automated ABI symbol and layout baseline against the previous release.

### Outside this repository

- AYTHER Tessera (emulator core), Runtime (game-session host), Lab (authoring),
  Play CE (launcher), and Web, each with its own repository and releases.
- Libretro emulator cores, ROMs, BIOS files, and third-party game content.

## Tessera integration

The native provider is specified by spec 008, whose acceptance gates are:

| Gate | Scope | Closed |
|---|---|---|
| G1 | First acceptance gate of spec 008 | 2026-11-09 |
| G2 | Observation equivalence with Genesis Plus GX, approved on Tessera `v0.2.0-rc.8` | 2026-11-12 |

Engine locks Tessera `v0.2.0-rc.9` in `dependencies/tessera.lock.json`. Later
work, such as host integration in AYTHER Runtime, is tracked in the consuming
repositories.

## Version contract

The release identity is unified while protocol revisions move independently;
the policy is [ADR 0002](adr/0002-release-and-protocol-version-contract.md).

| Axis | Current value | Source of truth |
|---|---:|---|
| AYTHER release | `0.1.0` | Cargo and CMake project versions |
| Flat C ABI revision | `9` | `CORE_C_ABI_REVISION` in `core/src/lib.rs`, returned by `ayther_core_version()` |
| Pack manifest schema | `2` | `MANIFEST_SCHEMA` in `core/src/archive_vfs.rs` |
| Emulator extension ABI | `1.10` | `AYTHER_ABI_VERSION_LATEST` in `include/ayther/libretro_host/ayther_api.h` |
| SDK C API revision | `1` | `AYTHER_SDK_C_API_VERSION` in `include/ayther/ayther_sdk.h` |
| Tessera C API accepted | `1.1` or later `1.x` | `cmake/AytherTesseraLock.cmake` |

During `0.x`, a minor release may break source or binary contracts; patch
releases stay source-compatible within one minor. The Runtime-facing C++ API is
source-compatible throughout `0.1.x`; no C++ binary compatibility is promised
([ADR 0003](adr/0003-runtime-engine-public-api-ownership.md)).

## Validation

- CI runs Rust quality gates, headless builds, native and native-VPX builds with
  out-of-tree package consumers, ASan and UBSan, fuzz smoke, Rust and C++
  coverage gates ([Coverage policy](COVERAGE.md)), and the Tessera provider
  tests on Windows. The jobs per platform and the reference runs are listed in
  the [support matrix](SUPPORT_MATRIX.md#evidence).
- GPU oracles are opt-in. `tools/check_gpu_matrix.ps1` fails a skipped suite
  and records the device and driver of a run; no hardware matrix exists.
- Oracles that need the Genesis Plus GX fork core named by
  `third_party/cores/core.lock`, Tessera fixtures, or a GPU report skip code 77
  without them.
- CodeQL runs on `main`. No code-scanning alert was open on 2026-11-19.

## Release blockers

Eight conditions gate the first supported release. *Deferred* means the work
depends on a component outside this repository.

| # | Blocker | Status | Evidence |
|---|---|---|---|
| 1 | Exercise the enforced public-header allowlist from first-party frontends | Deferred | The allowlist is enforced at install time and exercised by package consumers in CI. The `runtime-integration.yml` workflow builds the pinned AYTHER Runtime against the published packages and runs its tests; the other frontends live elsewhere |
| 2 | Pass unit, ABI, integration, renderer, audio, VPX, and real-emulator tests in CI on Windows and Linux | Open | The required matrix passes on both platforms. Real-emulator oracles still depend on an external core, and GPU tests are opt-in |
| 3 | Report skipped GPU tests rather than represent them as passing | Closed | `tools/check_gpu_matrix.ps1` fails when the suite is skipped and records the answering hardware |
| 4 | Enforce the version contract and protocol baselines in protected CI | Open | The release workflow validates that the tag and every product version agree. No automated symbol or layout baseline compares a build with its predecessor |
| 5 | Provision operational signing keys and exercise rotation and revocation through a shipping host | Deferred | The trust primitive and its rotation and revocation fixtures exist; operational keys are external |
| 6 | Ship complete third-party notices with every release artifact | Closed | CI re-derives `NOTICE.md` with `tools/gen_notice.ps1 -Check`; the release payload contract requires `share/licenses/Ayther/NOTICE.md` |
| 7 | Complete the security review of decoder limits, scripting lifetimes, FFI ownership, dynamic loading, and fuzz coverage | Open | Decoded-resource ceilings, the Lua memory cap, sanitizers, and fuzz targets are in place, and the earlier CodeQL findings are fixed. The review itself is not complete |
| 8 | Run a clean configure, build, test, install, and consume cycle per platform in protected CI | Closed | The native CI jobs and the release workflow perform that sequence on Windows and Linux |

## Definition of “stable”

Stable requires every blocker above closed, a published support matrix, a
documented compatibility window, reproducible artifacts, private vulnerability
reporting, and at least one release candidate consumed from outside the source
tree.

| Requirement | Met | Where |
|---|---|---|
| All release blockers closed | No | Three open, two deferred |
| Published support matrix | Yes | [Support matrix](SUPPORT_MATRIX.md) |
| Documented compatibility window | Yes | [API and compatibility](API_COMPATIBILITY.md) |
| Reproducible artifacts | Yes | Each release job packages twice and requires byte-identical archives; signed pre-releases are published |
| Private vulnerability reporting | Yes | [SECURITY.md](../SECURITY.md) |
| Release candidate consumed from outside the source tree | Yes | AYTHER Runtime locks `v0.1.0-rc.28` |

Main-branch review currently runs under the time-bounded
[single-maintainer exception](GOVERNANCE_EXCEPTIONS.md#gov-2026-001-single-maintainer-code-owner-review),
which is not independent review and cannot count as two-person release
approval.
