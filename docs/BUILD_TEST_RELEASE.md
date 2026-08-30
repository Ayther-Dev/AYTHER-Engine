# Build, test, and release

**Status:** CI, reproducible core packaging, signing, and attestations implemented

**Last verified:** 2026-08-30

This guide separates commands that work in the current checkout from release
gates that remain open. Tool installation is covered by
[Development environment](DEVELOPMENT_ENVIRONMENT.md).

> [!WARNING]
> No stable or supported artifact exists. A green Rust suite or headless CMake
> build is not sufficient evidence for an engine release.

## Rust quality gates

Run from the repository root:

```text
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --all-targets --locked
cargo doc --workspace --no-deps --locked
```

Rust formatting, linting, documentation, and tests are required CI gates. An
ignored benchmark is not a functional test failure, but must remain visible.

## Repository documentation and policy checks

Run these checks after changing documentation, public comments, dependencies,
or license metadata:

```text
pwsh tools/check_doc_references.ps1
pwsh tools/dep_graph.ps1 -Check
cmake -DAYTHER_REPO=. -P tools/check_license.cmake
pwsh tools/gen_api_reference.ps1 -Check
```

The documentation checker validates local Markdown links and `docs/...`
references embedded in source, CMake, and PowerShell. The public API index and
dependency graph are generated artifacts and must not be edited by hand.

## Sanitizers, fuzzing, coverage, and static analysis

These gates run on Linux with Clang. Each has a preset so a local run and CI
execute the same configuration:

```text
cmake --preset linux-native-asan   && cmake --build --preset linux-native-asan
ctest --preset linux-native-asan
cmake --preset linux-native-ubsan  && cmake --build --preset linux-native-ubsan
ctest --preset linux-native-ubsan
```

Both sanitizer presets build `Debug`, compile with `-fno-sanitize-recover=all`,
and run every CPU CTest; tests labelled `gpu` are excluded because the runners
promise no Vulkan hardware. A finding aborts the process, so any report is a
failure rather than a logged warning.

The Rust vulnerability gate and the fuzz smoke targets:

```text
cargo audit --deny warnings
cargo +nightly fuzz run packs    fuzz/corpus/packs    -- -max_total_time=30
cargo +nightly fuzz run decoders fuzz/corpus/decoders -- -max_total_time=30
cargo +nightly fuzz run ffi      fuzz/corpus/ffi      -- -max_total_time=30
```

`.cargo/audit.toml` holds the advisory exception list and is empty on purpose:
an unlisted advisory fails the build. Each fuzz target is seeded from
`fuzz/corpus/<target>` and CI keeps `fuzz/artifacts/<target>` when a target
crashes.

Coverage is reported per language and is informational for now — Rust through
`cargo llvm-cov`, C++ through the `linux-native-coverage` preset and
`tools/collect_cpp_coverage.sh`. Both land as pull-request artifacts
(`coverage-rust`, `coverage-cpp`). No threshold blocks a merge yet; adding one
is the next step, and it needs a recorded baseline first.

First-party C++ compiles with warnings as errors (`/W4 /WX`, or
`-Wall -Wextra -Wpedantic -Werror`). Vendored `ymfm` and the smoke tools and
probes under `tools/` are exempt via `ayther_instrument_target(...
NO_STRICT_WARNINGS)`; the engine, the unit and integration tests, and the FFI
tests are not.

`clang-tidy` runs only over the translation units a change actually touches:

```text
bash tools/run_clang_tidy_changed.sh <base-revision> build/linux-native
```

`.clang-tidy` is the single definition of scope for CI, an IDE, and a local
run. Analyzer, `bugprone-*`, ownership, virtual-destructor, and concurrency
findings block; the C-style-cast sweep is reported without blocking so the
existing pile stays visible without failing pull requests.

## Headless native build

Windows:

```powershell
cmake --preset windows-headless
cmake --build --preset windows-headless
ctest --preset windows-headless
cmake --install build/windows-headless
```

Linux uses the equivalent `linux-headless` names. These presets intentionally
build only the Rust core, CXX bridge, flat-ABI test, and the no-asset SF2 FFI
smoke. SDL3, Vulkan, ymfm, and libvpx are not required because
`Ayther::engine` remains excluded unless `AYTHER_BUILD_ENGINE=ON`.

## Complete native engine build

Set `VCPKG_ROOT` as described in the development-environment guide, then run:

```powershell
cmake --preset windows-native
cmake --build --preset windows-native
ctest --preset windows-native
cmake --install build/windows-native --prefix install/windows-native
```

This compiles all 24 explicit `ayther_engine` translation units plus the nine
ymfm units and Rust core. The Windows workflow above passed on 2026-08-27 with
Clang 22.1.6 and the pinned vcpkg baseline. Linux has not been verified in this
checkout.

For VP9, first run `pwsh tools/build_libvpx.ps1`, then replace every preset and
directory name above with `windows-native-vpx`. That configure/build/test/install
path also passed on 2026-08-27.

GPU oracles are opt-in and selected by their own preset. They synthesize their
inputs and require a working Vulkan device, but no ROM or recording:

```powershell
cmake --preset windows-native-gpu
cmake --build --preset windows-native-gpu
ctest --preset windows-native-gpu
```

The test preset filters on the `gpu` label. All eight GPU tests passed on
Windows on 2026-08-27. Linux uses the corresponding `linux-native-gpu` names
and remains unverified.

Regenerate and verify the dependency notice against the exact vcpkg status
selected by the native-VPX build:

```powershell
pwsh tools/gen_notice.ps1 -BuildDir build/windows-native-vpx
pwsh tools/gen_notice.ps1 -BuildDir build/windows-native-vpx -Check
```

The check fails when the committed `NOTICE.md` differs from the generated
inventory, so it belongs in the release pipeline after dependency resolution.

## What CTest proves

The headless preset registers two tests: `ayther.core.ffi` covers representative
layout, handle, pack, script, identity, and audio entry points; `sf2_synth`
covers the null-handle SoundFont contract without external assets.

The normal native preset registers 38 tests: 25 isolated engine unit/contract
executables, the two core/FFI tests, ten integration smokes for ABI, audio,
render output, subsystem routing, and voice routing, plus the installed-header
closure guard. Tests that require an unshipped emulator core use CTest skip code
77. The VPX preset adds three tests, and the GPU preset adds eight
hardware-labelled tests. These suites do not prove complete symbol coverage,
all emulator/game combinations, thread safety, or cross-platform behavior.

## Installed package

The headless install produces the core archive and C header. A native install
adds the engine and ymfm archives, the explicit engine-header allowlist,
shaders, CMake exports, the project notices, and the exact vcpkg license files
selected by that build. Renderer, audio implementation, libretro-host, and
Vulkan-backend headers remain source-tree internals and are not installed.
The VPX install also includes `vpxmd.lib`, headers, version, BSD license, patent
grant, and authors.

A core-only consumer can use:

```cmake
find_package(Ayther REQUIRED)
target_link_libraries(my_app PRIVATE Ayther::core)
```

An engine consumer declares the renderer dependencies in its own package-manager
environment and uses:

```cmake
find_package(Ayther 0.1 CONFIG REQUIRED COMPONENTS engine)
target_link_libraries(my_app PRIVATE Ayther::engine)
```

The native export also provides `Ayther::ymfm`; a VPX-enabled package provides
`Ayther::vpx`. `tests/package_consumer/` compiles only the installed C facade,
C++ session facade, and version header, then links and executes outside the
producer tree. No frontend, renderer implementation, or generated CXX bridge
headers are promised as public surfaces.

## Configuration matrix

| Preset family | Intended role | Current confidence |
|---|---|---|
| `*-headless` | core, bridge, and CPU/ABI checks | Windows verified; Linux pending |
| `*-release` | optimized core package | Pre-release core only; not an engine distribution |
| `*-native` | complete engine without VP9 | Windows build and 38 CTests verified; Linux pending |
| `*-native-vpx` | complete engine with VP9 | Windows build/install/consume verified; decoder fixtures and Linux pending |
| `*-native-gpu` | eight Vulkan GPU oracles | Windows 8/8 verified; Linux and broader GPU matrix pending |

## CI expectations

The checkout now includes `.github/workflows/ci.yml`. On pushes to `main`, pull
requests, and manual dispatches it runs repository-boundary checks, locked Rust
format/lint/tests/docs, documentation-reference and license consistency checks,
dependency-notice verification, and the Windows/Linux headless sequence. The
required native matrix additionally configures, builds, tests, and installs
`windows-native`, `linux-native`, `windows-native-vpx`, and
`linux-native-vpx`; every matrix entry then configures, links, and executes
`tests/package_consumer/` from outside the producer tree. Pull requests
additionally validate that individual commits do not mix the closed Lab
boundary with FOSS paths.

Five further jobs run on every pull request. `Linux native ASan` and
`Linux native UBSan` build the complete engine and run the CPU CTests under
their sanitizer, uploading `Testing/Temporary/LastTest.log` on failure.
`Fuzz smoke` runs the `packs`, `decoders`, and `ffi` targets for 30 seconds
each against their seeded corpus and keeps any crash as an artifact. The Rust
audit runs inside the `rust` job with a pinned `cargo-audit`. Coverage is
published by `rust-coverage` and `cpp-coverage` as separate artifacts, and
`clang-tidy` runs inside the `linux-native` matrix entry over the touched
translation units only.

Every one of those jobs is defined to fail the workflow on a finding, but a
failing job blocks a merge only once a repository administrator marks it as a
required status check. Until `Linux native ASan`, `Linux native UBSan`, the
three `Fuzz smoke (...)` jobs, `rust`, and `Linux native + package consumer`
are listed in the branch-protection rule for `main`, the mechanics alone do not
make them mandatory. The two coverage jobs are deliberately not required while
they remain informational.

The `GPU (Windows, opt-in)` and `GPU (Linux, opt-in)` jobs are deliberately
skipped in ordinary push and pull-request runs because GitHub-hosted runners do
not promise the required Vulkan hardware. A manual dispatch exposes the boolean
`run_gpu_tests` input; selecting it builds the `*-native-gpu` presets and runs
only tests labelled `gpu` (Linux uses Mesa plus a virtual display). This is an
explicit omission, not a successful GPU oracle.

The tag-only `.github/workflows/release.yml` validates the version contract,
runs the locked Rust gates plus optimized production-trust acceptance tests,
and builds the core SDK on Windows and Linux. It derives `SOURCE_DATE_EPOCH`
from the tagged commit, remaps build paths, packages each install tree twice,
and requires byte-identical SHA-256 digests before publication.

Each candidate carries an SPDX 2.3 SBOM generated from the locked Cargo graph
and exact installed files. The protected publish job creates SHA-256 checksums,
keyless Sigstore bundles for every asset, SLSA build provenance, and signed SBOM
attestations, then creates a GitHub pre-release. Private signing keys and
long-lived CI credentials are not used; OIDC credentials are short-lived.

Repository administrators must configure the `release` environment with
required reviewers, protect `v*` tags, and restrict workflow changes with
CODEOWNERS/branch protection. Without those repository controls, the workflow
mechanics alone are not a protected release boundary.

A consumer verifies a downloaded archive with all three independent records:

```text
sha256sum --check CHECKSUMS.sha256
gh attestation verify ayther-engine-v0.1.0-linux-x86_64.zip \
  --repo Ayther-Dev/AYTHER-Engine
cosign verify-blob \
  --bundle ayther-engine-v0.1.0-linux-x86_64.zip.sigstore.json \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  --certificate-identity-regexp '^https://github.com/Ayther-Dev/AYTHER-Engine/.github/workflows/release.yml@refs/tags/v[0-9].*$' \
  ayther-engine-v0.1.0-linux-x86_64.zip
```

The native engine, physical GPU/driver matrix, real-emulator fixtures, and the
remaining security review are still release blockers; automated publications
remain marked as pre-releases.

A release-capable pipeline must run on every supported platform and retain:

- locked Rust formatting, lint, tests, and documentation;
- C and C++ header compilation plus exhaustive ABI/layout checks;
- headless emulator/session integration tests with legally distributable
  fixtures;
- renderer tests separated into deterministic CPU and GPU-required groups;
- install, package discovery, link, and execution from outside the source tree;
- dependency, license, vulnerability, secret, and artifact-content scans;
- checksums and signatures generated only by protected release infrastructure.

Skipped tests must be reported as skipped. Missing hardware, ROMs, cores, or
fixtures must never be represented as a passing oracle.

## Release gate

Do not publish a stable build until all items in
[Project status](PROJECT_STATUS.md#release-blockers) are closed and the following
evidence is archived:

1. clean configure/build/test/install/consume logs for every supported platform;
2. logs proving the release/protocol version contract and baselines agree;
3. provisioned production registry and evidence that the documented rotation
   and revocation procedure is exercised by the shipping host;
4. complete transitive notices and source-offer obligations for shipped code;
5. security review of pack parsing, paths, limits, scripting, FFI, and dynamic
   library loading;
6. release notes, checksums, provenance, rollback instructions, and a supported
   version matrix.

Release artifacts must not contain ROMs, BIOS images, commercial game assets,
private keys, or emulator cores that the project is not authorized to ship.
