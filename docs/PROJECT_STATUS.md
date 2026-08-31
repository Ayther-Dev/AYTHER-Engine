# Project status

**Assessment date:** 2026-08-30

**Repository phase:** early construction and stabilization

**Release status:** no stable or supported release

This document is the authoritative statement of what AYTHER Engine can do
today. It separates observed checkout state from the target architecture.

## Executive status

The Rust core is substantial and documented at the crate level. Its formatting,
tests, lint, and documentation gates are green. The root build provides an
installable, out-of-tree-consumable `Ayther::core` and a complete `Ayther::engine`,
with manifest-mode native dependencies, ymfm, and optional libvpx. The repository
is nevertheless incomplete as a release distribution. CI covers repository
policy, Rust quality and dependency audit, headless and native Windows and Linux
builds, VPX on both platforms, out-of-tree package consumers, ASan and UBSan,
fuzz smoke, clang-tidy on changed sources, and coverage gates for both languages.
GPU jobs are opt-in, and when they do run an omission is reported as a failure
rather than as approval. Reproducible pre-release automation and production trust
primitives are present. Of the eight release blockers, three are closed, three
remain open, and two are deferred on components that live outside this
repository.

The current checkout is appropriate for core development and documentation
work. It is not appropriate for a public binary release, a production pack
trust decision, or a compatibility promise to third-party integrators.

## Observed checkout state

| Capability or artifact | State | Notes |
|---|---|---|
| Rust workspace | Present | Workspace version `0.1.0`, edition 2024, resolver 3 |
| `ayther_core` | Present | Builds as `rlib` and `staticlib` |
| Rust unit tests | Passing | 400 passed, 0 failed; one optional archive benchmark ignored on 2026-08-30 |
| Rust formatting | Passing | `cargo fmt --all -- --check` passed on 2026-08-30 |
| Rust linting | Passing | `cargo clippy --workspace --all-targets --locked -- -D warnings` passed on 2026-08-30 |
| Rust documentation gates | Enforced in source | Missing docs, broken intra-doc links, and unsafe operations in unsafe functions are denied |
| CXX bridge declarations | Present | Rust side in `core/src/ffi.rs`; generated C++ consumer tree is absent |
| Legacy C ABI | Present | Raw `ayther_*` exports in `core/src/lib.rs` |
| Public C header | Present | `include/ayther/ayther_core_ffi.h` declares the current flat C ABI; the C++ test checks representative layout and lifecycle contracts |
| C++ engine | Buildable, pre-release | `Ayther::engine` owns all 30 sources; Windows native and native-VPX builds pass |
| Root CMake project | Implemented, pre-release | Corrosion owns Cargo integration; CMake exposes core, engine, ymfm, and optional VPX targets |
| Engine tests and tools | Passing, with four developer-gated skips | On 2026-08-30 `windows-native` passed 46 of 46 and `windows-native-vpx` 49 of 49 with the fork cores present; a clean clone runs 42 and skips 4, which need a developer-supplied core. `abi_negociacion` no longer skips: it builds its core from `tools/test_core/` |
| Flat C FFI verification | Passing | `ayther.core.ffi` reports 132 of 132 checks under the optimized `windows-native-vpx` build |
| Installable package | Pre-release | Core and native packages install namespaced targets, headers, shaders, and notices; external Windows consumers pass with VPX disabled and enabled |
| Line coverage | Measured, gated | Rust 75.24% (12,727/16,915), C++ 62.00% (11,936/19,251); both are regression barriers, see [Coverage](COVERAGE.md) |
| Warnings policy | Enforced | Every first-party target compiles under warnings-as-errors; the two exemptions, vendored ymfm and the generated cxx bridge, are named in `CMakeLists.txt` |
| CI and release workflows | Reproducible pre-release path | CI covers the build/test matrix; tag releases compare rebuilt core packages, emit SPDX SBOMs, checksums, Sigstore bundles, SLSA provenance, and signed SBOM attestations before protected publication |
| Third-party inventory | Implemented and enforced | CI re-derives the dependency notice and fails when it drifts; the release payload contract requires `share/licenses/Ayther/NOTICE.md`, plus the VPX notices on Windows |
| Pack container security | Implemented baseline | Builder, reader, and validator share canonical paths, duplicate rejection, archive/entry/metadata limits, and compression-ratio defenses |
| Production pack trust | Implemented primitive, provisioning pending | Explicit public-key registries enforce identity, validity, revocation and game scope; optimized builds reject the RFC test key; Hub operational keys remain external |

## Version contract

The release identity is now unified while protocol revisions remain independent.
The policy and bump rules are recorded in
[ADR 0002](adr/0002-release-and-protocol-version-contract.md).

| Axis | Current value | Meaning | Status |
|---|---:|---|---|
| AYTHER release | `0.1.0` | Cargo, CMake, vcpkg, SDK headers/library, pack validation, and Lua | Unified and cross-language tested |
| Flat C ABI revision | `7` | Value returned by `ayther_core_version()` | Adds explicit trusted pack open without changing existing layouts |
| Pack manifest schema | `2` | Highest schema accepted and written | Independent format revision |
| Emulator extension ABI | `1.10` | AYTHER-aware libretro core negotiation | Independent major/minor protocol |
| SDK C API revision | `1` | `AYTHER_SDK_C_API_VERSION` | Independent facade revision |

During `0.x`, the release minor version may break source or binary contracts;
patch versions remain source-compatible within one minor. The Runtime-facing
C++ API therefore preserves source compatibility throughout `0.1.x`, while
`0.2.0` may revise the contract; no C++ binary compatibility is promised. The
ownership decision is [ADR 0003](adr/0003-runtime-engine-public-api-ownership.md).
Protocol revisions move only when their own boundary changes and must carry
migration and conformance tests.

## Capability maturity

### Implemented and locally testable

- tile, sprite, pose, animation, background, and audio identity/substitution
  primitives;
- RAM conditions, AOB scanning, entity anchoring, and game profiles;
- signed/lazy `.ay` archives, integrity indexes, canonical path and ZIP resource
  limits, manifest schema handling, profiles, tiers, regional resolution,
  credits, and compatibility reports;
- constrained Lua execution and runtime substitution overrides;
- in-memory IPS/BPS patching;
- SoundFont conversion, trimming, synthesis, and mapping;
- installed Runtime-facing version and compiled-capability probes owned by
  Engine, with no device or environment probing;
- Rust-facing APIs, a typed CXX bridge definition, and a broad legacy C ABI.

### Native integration present, behavior verification incomplete

The root contains the C++ session, libretro host, Vulkan renderer, SDL audio,
rewind/recording, headers, and shaders. They are exposed through
`Ayther::engine`; ymfm is vendored at a recorded upstream commit, and optional
libvpx is built from a pinned tag. The complete target, installed package, and
external consumer link on Windows. The native CTest graph now covers CPU
contracts, audio, ABI integration, VPX, and eight synthetic Vulkan oracles.
Broader renderer correctness, real-emulator and game coverage, decoder fixtures,
cross-platform execution, and thread-safety testing remain incomplete, so
buildability is not a stability claim.

Detailed native contracts are maintained in
[C++ API and implementation contracts](CPP_API_REFERENCE.md), with specialized
documents for the [component model](COMPONENT_MODEL.md),
[emulator extension ABI](EMULATOR_EXTENSION_ABI.md),
[cinematic composition](CINEMATIC_PLANE_COMPOSITION.md), and
[widescreen behavior](WIDESCREEN.md).

### Explicitly outside this repository

- AYTHER Runtime: game-session executable and reference frontend;
- AYTHER SDK: authoring tools, examples, fixtures, conformance suite, and pack
  format guides;
- AYTHER Play: player-facing launcher;
- AYTHER Hub: distribution service and operational keys;
- AYTHER Lab: proprietary authoring application;
- emulator cores, ROMs, BIOS files, and third-party game content.

## Platform status

The full result set, and the distinction between what was measured here and
what only CI covers, is published as the [support matrix](SUPPORT_MATRIX.md).
In summary, measured on 2026-08-30 at commit `846081e`:

- Windows 11 x86_64 with clang-cl 22.1.6 is the verified platform. `windows-native`
  passed 46 of 46, `windows-native-vpx` 49 of 49, `windows-native-coverage` 46 of
  46, and `windows-headless` 3 of 3, with zero failures in each.
- The eight Vulkan oracles passed on an NVIDIA GeForce RTX 3060 Laptop GPU,
  driver 616.224.0, Vulkan API 1.4.351. The GPU wrapper records the device and
  driver that answered and fails when the suite is skipped instead of run, so a
  green GPU result always names its hardware. One device is not a driver matrix;
  AMD, Intel, and older drivers remain unmeasured.
- Linux x86_64 is covered by CI on every pull request but was not reproduced by
  hand for this assessment.
- The Rust baseline passed formatting, linting, and 400 tests, with one optional
  archive benchmark ignored.

A clean clone runs the same `windows-native` suite as 42 passed and 4 skipped.
`audio_mute`, `audio_output`, `render_output`, and `subsystem_routing` hard-code
a path to a fork emulator core that `.gitignore` excludes, so they return skip
code 77 when a developer has not supplied it. This is a gap in those four tests
rather than a missing capability, and it is tracked as release blocker 2; the
fix pattern already exists, since two sibling oracles consult `AYTHER_ABI_CORE`
and therefore run against the in-repo test core everywhere.

macOS, mobile, WebAssembly, console, and non-x86_64 targets are not supported
commitments and nothing has measured them.

The engine's current observation model is specialized around Mega Drive / Genesis
hardware data and an AYTHER-aware libretro core extension. General support for
other platforms is research or roadmap work, not a present capability.

## Release blockers

Eight blockers were recorded for the first supported release. Each is marked
**closed**, **open**, or **deferred**, with the evidence behind the mark.
*Deferred* means the work depends on a component outside this repository and
cannot be closed here; it is not a quiet downgrade to "done".

Three are closed, three are open, and two are deferred.

| # | Blocker | Status | Evidence |
|---|---|---|---|
| 1 | Exercise the enforced public header allowlist from first-party frontends | Deferred | The allowlist is enforced at install time and exercised by the out-of-tree package consumers in CI and by the release-candidate frontend check, which refuses a report containing repository paths. The first-party frontends themselves — Runtime and Play — live outside this repository, so no work here can close this |
| 2 | Pass Rust, C++ unit, ABI/layout, headless integration, renderer, audio, VPX, and real-emulator tests in CI on Windows and Linux | Open | Two gaps. No Linux native job has ever passed: the only CI run on record failed them all on a missing `<cmath>` include, since repaired in an unpushed commit but never re-validated. Separately, the four real-emulator oracles skip without a developer-supplied fork core, as described under Platform status. `abi_negociacion`, previously the largest gap, is closed |
| 3 | Pass deterministic CPU renderer and explicit GPU-required tests, with skipped hardware reported rather than represented as passing | Closed | `tools/check_gpu_matrix.ps1` fails when the suite is skipped and records the answering device and driver, so an omission can no longer be read as approval. Eight of eight GPU oracles passed on recorded hardware. The `render_output` skip on a clean clone belongs to blocker 2, not here |
| 4 | Enforce the version contract and protocol baselines in protected CI | Open | The release workflow validates that the tag and every product version agree before anything is built, which covers the release axis. No automated symbol or layout baseline compares a build against its predecessor, so an accidental ABI change is still invisible |
| 5 | Provision Hub's operational keys and exercise rotation and revocation through a shipping host | Deferred | The trust primitive is implemented and tested: registries enforce identity, validity window, revocation, and per-game scope, optimized builds reject the RFC test key, and rotation and revocation have dedicated fixtures in both Rust and the flat C ABI. Hub's operational keys are external to this repository |
| 6 | Enforce generation and shipment of complete transitive third-party notices for every release artifact | Closed | CI re-derives the dependency notice with `tools/gen_notice.ps1 -Check` and fails on drift. The release payload contract requires `share/licenses/Ayther/NOTICE.md` in the installed tree, and the release workflow verifies the artifact carries exactly its advertised payload |
| 7 | Complete security review of media-decoder limits, scripting lifetimes, FFI ownership, dynamic library loading, and adversarial fuzz coverage | Open, confirmed defect repaired | Substantial parts are built: decoded-resource ceilings for image, audio, and video, a 64 MiB Lua memory cap, ASan and UBSan jobs, and three fuzz targets. The 43-byte SoundFont reproducer that previously requested 4 GiB is now rejected by `validated_sf2_extent()` before the third-party parser; the regression covers both `Sf2Synth::new()` and `new_shared()`. The wider security review remains open pending final sanitizer, fuzz, and CI evidence. See the historical [go/no-go decision](RELEASE_GO_NO_GO.md) |
| 8 | Produce one clean clone to configure, build, test, install, and consume cycle on each supported platform in protected CI | Closed | The `native` CI job performs exactly that sequence — checkout, configure, build, test, install, then configure, build, and run an out-of-tree consumer — across four configurations: Windows and Linux, each with VPX disabled and enabled. The release workflow repeats it against the packaged artifact |

Blockers 2, 4, and 7 are the remaining engineering work. Blockers 1 and 5 are
gated on AYTHER Runtime, Play, and Hub, and will stay deferred as long as those
components live elsewhere.

## Definition of “stable”

“Stable” requires all release blockers above, a published support matrix, a
documented compatibility window, reproducible artifacts, private vulnerability
reporting, and at least one release candidate consumed from outside the source
tree. Test success in the Rust core alone is not sufficient.

Of those six requirements, two are met and four are not:

| Requirement | Met | Where |
|---|---|---|
| All release blockers closed | No | Three open, two deferred; see the table above |
| Published support matrix | Yes | [Support matrix](SUPPORT_MATRIX.md) |
| Documented compatibility window | Yes | [API and compatibility](API_COMPATIBILITY.md) |
| Reproducible artifacts | Implemented, never executed | The release workflow packages twice and requires the two archives to be byte-identical, then emits SPDX SBOMs, checksums, Sigstore bundles, and SLSA provenance. No tag has been pushed, so the workflow has never run |
| Private vulnerability reporting | **No** | [SECURITY.md](../SECURITY.md) documents the channel, but GitHub private vulnerability reporting is disabled on the repository, so the option it tells reporters to use does not exist |
| A release candidate consumed from outside the source tree | **No** | `tools/check_rc_consumer.ps1` works and runs the installed package as a frontend would, but no release candidate has ever been published: there are no tags and no releases |

The stability gate was executed against commit `c3866fe` on 2026-08-30 and
returned **no-go**; the evidence for each criterion is recorded in the
[go/no-go decision](RELEASE_GO_NO_GO.md). The remaining distance to “stable” is
therefore both the blocker list and parts of the release machinery that are
implemented but have never been operated.

This document stays the authoritative statement: the checkout is suitable for
core development and documentation work, and not for a public binary release, a
production pack trust decision, or a compatibility promise to third-party
integrators.
