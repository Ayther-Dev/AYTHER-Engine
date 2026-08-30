# Project status

**Assessment date:** 2026-08-28

**Repository phase:** early construction and stabilization

**Release status:** no stable or supported release

This document is the authoritative statement of what AYTHER Engine can do
today. It separates observed checkout state from the target architecture.

## Executive status

The Rust core is substantial and documented at the crate level. Its formatting,
tests, lint, and documentation gates are green. The root build now provides an
installable, out-of-tree-consumable `Ayther::core` and a complete 24-source
`Ayther::engine`, with manifest-mode native dependencies, ymfm, and optional
libvpx. The repository is nevertheless incomplete as a release distribution. A
baseline CI workflow now covers repository policy, Rust quality, headless and
native Windows/Linux builds, VPX on both platforms, and out-of-tree package
consumers. GPU jobs are explicitly opt-in and skipped by default; protected
release automation and production trust configuration remain open.

The current checkout is appropriate for core development and documentation
work. It is not appropriate for a public binary release, a production pack
trust decision, or a compatibility promise to third-party integrators.

## Observed checkout state

| Capability or artifact | State | Notes |
|---|---|---|
| Rust workspace | Present | Workspace version `0.1.0`, edition 2024, resolver 3 |
| `ayther_core` | Present | Builds as `rlib` and `staticlib` |
| Rust unit tests | Passing | 375 passed; one optional archive benchmark ignored on 2026-08-29 |
| Rust formatting | Passing | `cargo fmt --all -- --check` is a required CI gate |
| Rust linting | Passing | `cargo clippy --workspace --all-targets --locked -- -D warnings` |
| Rust documentation gates | Enforced in source | Missing docs, broken intra-doc links, and unsafe operations in unsafe functions are denied |
| CXX bridge declarations | Present | Rust side in `core/src/ffi.rs`; generated C++ consumer tree is absent |
| Legacy C ABI | Present | Raw `ayther_*` exports in `core/src/lib.rs` |
| Public C header | Present | `include/ayther/ayther_core_ffi.h` declares the current flat C ABI; the C++ test checks representative layout and lifecycle contracts |
| C++ engine | Buildable, pre-release | `Ayther::engine` owns all 24 sources; Windows native and native-VPX builds pass |
| Root CMake project | Implemented, pre-release | Corrosion owns Cargo integration; CMake exposes core, engine, ymfm, and optional VPX targets |
| Engine tests and tools | Partial | Headless ABI tests and native CPU/audio/renderer integration probes are present; real-emulator, cross-platform, and broader hardware coverage remain incomplete |
| Installable package | Pre-release | Core and native packages install namespaced targets, headers, shaders, and notices; external Windows consumers pass with VPX disabled and enabled |
| CI and release workflows | Expanded baseline | `.github/workflows/ci.yml` checks repository boundaries, locked Rust gates, headless and native Windows/Linux builds, VPX on both platforms, installed-package consumers, and explicitly opt-in GPU jobs; protected release automation remains pending |
| Third-party inventory | Implemented for validated builds | Cargo graph, vcpkg ports, vendored revisions, and shipped license material are recorded; release CI enforcement remains pending |
| Pack container security | Implemented baseline | Builder, reader, and validator share canonical paths, duplicate rejection, archive/entry/metadata limits, and compression-ratio defenses |
| Production pack trust | Missing | The code embeds a public RFC test key; no production key registry, expiry, scope, revocation, or rotation path is present |

## Version contract

The release identity is now unified while protocol revisions remain independent.
The policy and bump rules are recorded in
[ADR 0002](adr/0002-release-and-protocol-version-contract.md).

| Axis | Current value | Meaning | Status |
|---|---:|---|---|
| AYTHER release | `0.1.0` | Cargo, CMake, vcpkg, SDK headers/library, pack validation, and Lua | Unified and cross-language tested |
| Flat C ABI revision | `5` | Value returned by `ayther_core_version()` | Independent, unstable protocol revision |
| Pack manifest schema | `2` | Highest schema accepted and written | Independent format revision |
| Emulator extension ABI | `1.10` | AYTHER-aware libretro core negotiation | Independent major/minor protocol |
| SDK C API revision | `1` | `AYTHER_SDK_C_API_VERSION` | Independent facade revision |

During `0.x`, the release minor version may break source or binary contracts;
patch versions remain compatible within one minor. Protocol revisions move only
when their own boundary changes and must carry migration and conformance tests.

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

The shared CMake presets cover Windows and Linux core and native builds, with
separate VPX variants. Windows configure/build/test/install and external
consumption passed for native builds with VPX disabled and enabled on
2026-08-27. The eight opt-in Vulkan CTests also passed on Windows; Linux has not
been exercised and the broader GPU/driver matrix remains unverified. macOS, mobile,
WebAssembly, and console targets are not supported commitments.

The engine's current observation model is specialized around Mega Drive / Genesis
hardware data and an AYTHER-aware libretro core extension. General support for
other platforms is research or roadmap work, not a present capability.

## Release blockers

1. Exercise the enforced public C/C++ header allowlist from first-party
   frontends. `Ayther::core` and `Ayther::engine` are pre-release consumable.
2. Pass Rust, C++ unit, ABI/layout, headless integration, renderer, audio, VPX,
   and real-emulator tests in CI on Windows and Linux.
3. Pass deterministic CPU renderer tests and explicit GPU-required tests, with
   skipped hardware reported rather than represented as passing.
4. Enforce the accepted version contract and protocol baselines in protected CI.
5. Replace development-only signing with an explicit production trust store,
   key rotation/revocation process, and release-mode acceptance tests.
6. Enforce generation and shipment of complete transitive third-party notices
   for every release artifact.
7. Complete security review of media-decoder limits, scripting lifetimes, FFI
   ownership, dynamic library loading, and adversarial fuzz coverage.
8. Produce one clean clone → configure → build → test → install → consume cycle
   on each supported platform in protected CI.

## Definition of “stable”

“Stable” requires all release blockers above, a published support matrix, a
documented compatibility window, reproducible artifacts, private vulnerability
reporting, and at least one release candidate consumed from outside the source
tree. Test success in the Rust core alone is not sufficient.
