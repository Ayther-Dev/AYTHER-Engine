# Build, test, and release

**Status:** CI, reproducible Engine packaging, signing, and attestations
implemented; no stable release exists

**Last verified:** 2026-11-19, against `v0.1.0-rc.28` (`6d1c2e8`)

This guide covers the commands that build and test the Engine, what CI runs on
every pull request, and how release candidates are produced and verified. Tool
installation is in [Development environment](DEVELOPMENT_ENVIRONMENT.md); the
optional Tessera provider is in [Tessera native provider](TESSERA_NATIVE.md).

> [!WARNING]
> Every published artifact is a pre-release. A green Rust suite or headless
> CMake build is not sufficient evidence for an engine release; see
> [Release gate](#release-gate).

## Rust quality gates

Run from the repository root:

```text
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --all-targets --locked
cargo doc --workspace --no-deps --locked
```

All four are required CI gates on Windows and Linux. An ignored benchmark is not
a functional test failure, but it must remain visible.

## Repository documentation and policy checks

Run these after changing documentation, public comments, dependencies, or
license metadata:

```text
pwsh tools/check_doc_references.ps1
pwsh tools/dep_graph.ps1 -Check
pwsh tools/gen_api_reference.ps1 -Check
cmake -DAYTHER_REPO=. -P tools/check_license.cmake
pwsh tools/gen_notice.ps1 -Check
```

The documentation checker validates local Markdown links and `docs/...`
references embedded in source, CMake, and PowerShell. The public API index,
dependency graph, and `NOTICE.md` are generated and must not be edited by hand.

Without a build directory, `gen_notice.ps1 -Check` verifies only the Rust crate
section; this is what CI runs. To verify the vcpkg section as well, point it at
a native build:

```powershell
pwsh tools/gen_notice.ps1 -BuildDir build/windows-native-vpx -Check
```

## Configure presets

`CMakePresets.json` is the single definition of every configuration. Each
preset builds in `build/<preset>`. Replace `<platform>` with `windows` or
`linux`.

| Preset | Build type | Purpose |
|---|---|---|
| `<platform>-headless` | RelWithDebInfo | Rust core, CXX bridge, and CPU/ABI tests; no native dependencies |
| `<platform>-release` | Release | Optimized core-only build (not a published artifact) |
| `<platform>-release-engine` | Release | Optimized complete engine; builds the `ayther-engine` release archive |
| `<platform>-release-engine-vpx` | Release | The same with VP9; builds the `ayther-engine-vpx` archive |
| `<platform>-native` | RelWithDebInfo | Complete engine for development, with `tools/parity_bench` |
| `<platform>-native-vpx` | RelWithDebInfo | Complete engine with VP9 |
| `<platform>-native-gpu` | RelWithDebInfo | Complete engine plus the Vulkan GPU oracles (`gpu` label only) |
| `windows-native-coverage` | RelWithDebInfo + `/Od` | LLVM source coverage on Windows |
| `linux-native-coverage` | Debug | LLVM source coverage on Linux (CPU and GPU suites) |
| `linux-native-asan`, `linux-native-ubsan`, `linux-native-sanitizers` | Debug | AddressSanitizer, UndefinedBehaviorSanitizer, or both |

Every configure preset has matching build and test presets, plus
`linux-native-coverage-gpu` for the GPU half of Linux coverage. Test presets
for release, sanitizer, and coverage builds exclude the `gpu` label.

The Tessera provider is not enabled by any preset. It is added to a native or
release-engine preset with `-DAYTHER_ENABLE_TESSERA=ON -DTessera_DIR=...`; see
[Optional Tessera provider](#optional-tessera-provider).

## Headless build

```powershell
cmake --preset windows-headless
cmake --build --preset windows-headless
ctest --preset windows-headless
cmake --install build/windows-headless
```

Linux uses the `linux-headless` names. The headless presets build only the Rust
core, the CXX bridge, and the tests that need no native dependencies.
`Ayther::engine` is excluded unless `AYTHER_BUILD_ENGINE=ON`, so SDL3, Vulkan,
ymfm, and libvpx are not required.

## Complete native build

Set `VCPKG_ROOT` as described in the development-environment guide, then run:

```powershell
cmake --preset windows-native
cmake --build --preset windows-native
ctest --preset windows-native
cmake --install build/windows-native --prefix install/windows-native
```

For VP9 on Windows, first run `pwsh tools/build_libvpx.ps1`, then use the
`windows-native-vpx` preset names. Linux uses `linux-native` and
`linux-native-vpx`; the Linux VPX variant links the system libvpx through
`pkg-config`.

GPU oracles are opt-in. They synthesize their inputs and need a working Vulkan
device, but no ROM or recording:

```powershell
cmake --preset windows-native-gpu
cmake --build --preset windows-native-gpu
ctest --preset windows-native-gpu
```

Run `ctest --preset <preset> -N` to list the registered tests of a
configuration.

## Optional Tessera provider

`AYTHER_ENABLE_TESSERA=ON` links an installed Tessera package (AYTHER's own
emulator core) as a native provider behind `AytherSession`. The option is OFF
by default and no preset enables it.

```powershell
cmake --preset windows-native-vpx `
  -DAYTHER_ENABLE_TESSERA=ON `
  "-DTessera_DIR=<tessera-prefix>/lib/cmake/Tessera"
```

Configure fails unless the package declares Tessera API 1.1 or a later 1.x and
matches `dependencies/tessera.lock.json` (currently Tessera `v0.2.0-rc.9`).
`AYTHER_TESSERA_ALLOW_UNLOCKED=ON` accepts another package for local
development only; nothing built with it may be published.
`tools/check_tessera_lock.cmake` implements the comparison and the
`ayther.package.tessera_lock.*` tests cover it.

The integration tests additionally need `AYTHER_TESSERA_TEST_SOURCE_DIR` (a
Tessera checkout at the locked commit) and `AYTHER_TESSERA_TEST_FIXTURE_DIR`
(fixtures generated locally from that checkout). Building Tessera, the
fixtures, and the parity bench is documented in
[Tessera native provider](TESSERA_NATIVE.md).

## What CTest proves

The external-core oracles `audio_mute`, `audio_output`, `render_output`, and
`subsystem_routing` resolve `AYTHER_ABI_CORE` first, then the `vram_file`
filename in `third_party/cores/core.lock`. An invalid explicit path or a
malformed lock is a failure. Only an absent optional locked binary produces
CTest skip code 77, and an override is never silently replaced by another
binary.

Native CI captures the complete test log and runs
`tools/check_core_test_results.ps1 -LogPath native-ctest.log`. The checker
requires one result for each of those four tests and for the six bundled-core
tests `abi_negociacion`, `abi_frame_delta`, `abi_multilayer`, `abi_lecturas`,
`abi_escrituras`, and `e2e_determinismo`. The bundled-core tests must run even
when the optional external core is absent. The checker also rejects failures,
duplicate results, and unexplained skips anywhere in the log. Its own contract
test is `ayther.quality.core_test_results`.

These suites do not prove complete symbol coverage, every emulator/game
combination, thread safety, or behavior on platforms CI does not run.

### The test emulator core

`tools/test_core/` is a deterministic libretro core owned by this repository
and built from source on both platforms. It emulates nothing: its output is a
function of the ROM bytes, the frame number, and the input, which is what the
ABI and determinism oracles need. It is built twice from one source:
`ayther_test_core` exports the AYTHER extension entry point and
`ayther_test_core_stock` does not, so both halves of the extension negotiation
run on every checkout.

The parsed-sprite table it reports depends on the ROM and the slot, not on the
frame. A libretro core is process-global, so the E-5 oracle's session and its
control runner share one instance and step it alternately; a frame-varying
table would compare two different frames. The cost is that this fixture cannot
catch a stale published copy. Frame-varying signals live in RAM, video, and
audio.

### End-to-end determinism

`tools/e2e_determinism/` runs a synthetic ROM, the test core, a signed pack,
and a scripted input track through a real session and hashes frames, audio,
events, and work RAM.

```text
ctest --preset windows-native -R e2e_determinismo
```

Two sessions in one process must agree (run-to-run), which catches
nondeterminism from uninitialized memory, pointer values, iteration order, or
time. Frames and audio must also equal constants pinned in
`tools/e2e_determinism/CMakeLists.txt` (run-to-golden); CI running the same
test on Windows and Linux turns those constants into a cross-platform claim.
Events and work RAM are compared run-to-run only, because they change with the
optimization level. Only integer data is hashed.

A mismatch against the pinned hashes is a finding, not a number to refresh:
decide whether the engine, the test core, or a platform changed before
re-pinning.

### GPU matrix

GPU jobs run through `tools/check_gpu_matrix.ps1` rather than calling CTest
directly:

```text
pwsh tools/check_gpu_matrix.ps1 -Preset windows-native-gpu
pwsh tools/check_gpu_matrix.ps1 -Preset linux-native-gpu -Launcher 'xvfb-run --auto-servernum'
```

The GPU oracles skip themselves when no Vulkan device answers, so the wrapper
**fails** when the suite was skipped instead of executed, when fewer tests ran
than expected, or when no device was reported. `vk_context` logs the device
name, type, vendor, driver version, and Vulkan API version on every context
creation, and the wrapper writes a report that is uploaded whether the job
passed or failed.

## Compiler warnings and static analysis

First-party C++ compiles with warnings as errors: `/W4 /WX` on the MSVC-style
driver, `-Wall -Wextra -Wpedantic -Werror` on the GNU-style one. Every
first-party target is under that policy, with no target-level opt-out. Two
built targets are exempt because the code is not ours to fix:

| Target | Why |
|---|---|
| `ayther_ymfm` | Vendored Yamaha FM cores, maintained upstream |
| `ayther_cxx` | Generated by cxxbridge from `core/src/ffi.rs` |

vcpkg packages and the vendored `ymfm` headers are SYSTEM includes
(`-imsvc` / `-isystem`), so a warning in the log is first-party by
construction.

Deprecated platform APIs are wrapped, not suppressed: `ayther::file_open`
(`ayther_file.h`) and `ayther::env_get` (`ayther_env.h`) replace `fopen` and
`getenv`, and `_CRT_SECURE_NO_WARNINGS` is defined nowhere. The only
suppression is `AYTHER_LEGACY_ABI_BEGIN` / `..._END` from
`ayther_diagnostic.h`, in three-line windows around the ABI parity oracles that
must call the deprecated accessors they compare against.

No preset configures a Windows `Debug` build. `CMAKE_MSVC_RUNTIME_LIBRARY` is
pinned to the release CRT so first-party objects match the Rust staticlib,
which Cargo always builds in release, and vcpkg's debug-CRT libraries would
mismatch it. The coverage preset gets unoptimized code from `/Od` on top of
`RelWithDebInfo` instead.

Formatting and `clang-tidy` run only over the sources a change touches:

```text
bash tools/run_clang_tidy_changed.sh <base-revision> build/linux-native
```

CI runs this with clang-format 18 and clang-tidy 18 on Linux, and it checks the
whole of every changed file. A newer local clang-format can disagree with
version 18; format with 18 before pushing. `.clang-tidy` is the single
definition of analysis scope. Analyzer, `bugprone-*`, ownership,
virtual-destructor, and concurrency findings block; the C-style-cast sweep is
reported without blocking.

## Sanitizers, fuzzing, and coverage

```text
cmake --preset linux-native-asan   && cmake --build --preset linux-native-asan
ctest --preset linux-native-asan
cmake --preset linux-native-ubsan  && cmake --build --preset linux-native-ubsan
ctest --preset linux-native-ubsan
```

CI uses `linux-native-sanitizers`, which enables both sanitizers in one build.
Sanitizer builds compile with `-fno-sanitize-recover=all` and run every CPU
test, so any report aborts the test.

The Rust vulnerability gate and the fuzz smoke targets:

```text
cargo audit --deny warnings
cargo audit --file fuzz/Cargo.lock --deny warnings
cargo +nightly fuzz run packs    fuzz/corpus/packs    -- -max_total_time=30
cargo +nightly fuzz run decoders fuzz/corpus/decoders -- -max_total_time=30
cargo +nightly fuzz run ffi      fuzz/corpus/ffi      -- -max_total_time=30
```

`.cargo/audit.toml` holds the advisory exception list and is empty on purpose.
CI pins `cargo-audit`, `cargo-fuzz`, and the fuzz nightly in the `env` block of
`.github/workflows/ci.yml`. Each fuzz target is seeded from
`fuzz/corpus/<target>`, and CI keeps `fuzz/artifacts/<target>` when a target
crashes.

Coverage is gated per language. Thresholds, scope, and local reproduction are
in [Coverage policy](COVERAGE.md).

## Installed package

A native install contains the core, engine, and ymfm archives, the
CMake-owned public header allowlist (`cmake/AytherPublicHeaders.txt`),
compiled SPIR-V shaders, CMake exports, the project notices, and the vcpkg
license files selected by that build. Renderer, audio implementation,
libretro-host, and Vulkan-backend headers stay internal. A Windows VPX install
also bundles `vpxmd.lib` with its headers, version, license, patent grant, and
authors.

An engine consumer declares the engine's native dependencies in its own package
manager and uses:

```cmake
find_package(Ayther 0.1.0 CONFIG REQUIRED COMPONENTS engine)
target_link_libraries(my_app PRIVATE Ayther::engine)
```

The package also exports `Ayther::core`, `Ayther::ymfm`, and, when VPX is
enabled, `Ayther::vpx`. `tests/package_consumer/` compiles only the installed C
facade, C++ session facade, and version header, then links and runs outside the
producer tree. Consumer requirements are in
[Development environment](DEVELOPMENT_ENVIRONMENT.md#consuming-an-installed-engine).

Local packages built for a QA campaign are development artifacts. They do not
use the name, hash, signature, or provenance of a published release; besides
the CMake configuration and version headers they carry a content manifest and
metadata declaring `officialRelease: false`, the base reference, and the
contract version exercised. A consumer must pin the exact hash of such a file
and keep that selection separate from any release lock.

## CI

`.github/workflows/ci.yml` runs on pull requests and manual dispatch. All
mandatory jobs feed the `Required CI gate` job, the single status check the
`main` ruleset requires.

| Job | What it runs |
|---|---|
| Repository policy | component boundaries, documentation references, dependency graph, API index, license consistency, coverage-gate unit tests, and (on pull requests) commit boundaries |
| Rust quality gates (Windows, Linux) | locked format, clippy, tests, docs, and `gen_notice.ps1 -Check`; `cargo audit` over both lockfiles on Linux |
| Headless (Windows, Linux) | `*-headless` configure, build, test, install |
| Linux native ASan + UBSan | `linux-native-sanitizers`, CPU tests |
| Fuzz smoke | `packs`, `decoders`, and `ffi`, 30 seconds each |
| Rust coverage gate | `cargo llvm-cov` and the Rust thresholds |
| C++ coverage collection (Linux) | `linux-native-coverage` with clang 18, CPU suite and GPU suite on Mesa Lavapipe |
| Native matrix | `linux-native`, `linux-native-vpx`, and `windows-native-vpx`: build, test, `check_core_test_results.ps1`, install, out-of-tree package consumer, incompatible-version rejection; clang-format and clang-tidy on changed sources (`linux-native`) |
| C++ coverage gate | merges the Linux report with the Windows Tessera-native report and enforces the C++ thresholds |

The Windows native job also builds Tessera from the commit in
`dependencies/tessera.lock.json` (a step fails if `TESSERA_COMMIT` in
`ci.yml` differs from the lock), generates the native fixtures, runs the
Tessera integration and cross-pack parity tests with the locked gpgx core, and
collects Windows coverage for the four `ayther.integration.tessera_*` tests.
Linux jobs do not build Tessera.

Every Windows job installs LLVM 22.1.8 with `tools/install_pinned_llvm.ps1`
before configuring, because the runner image moves MSVC and LLVM independently
and MSVC 14.52's STL refuses Clang older than 22. Raise the pin together with
the local toolchain.

A push to `main` and the weekly schedule do not rebuild or retest: `main`
accepts a pull request only when its branch is up to date, so the merged tree
is the one the pull request validated. There, the vcpkg jobs only configure
(`WARM_ONLY`) to keep a vcpkg binary cache that every pull request restores.
vcpkg binaries are cached per job and preset.

The `GPU (Windows, opt-in)` and `GPU (Linux, opt-in)` jobs run only on a manual
dispatch with `run_gpu_tests`, because hosted runners promise no Vulkan
hardware. A skipped GPU job is an explicit omission, not a passing oracle.

## Release artifacts

A `v*` tag publishes **two Engine variants on Windows and Linux**, four ZIP
archives in total:

| Archive | Targets | Native dependencies |
|---|---|---|
| `ayther-engine-<tag>-<platform>.zip` | `Ayther::core`, `Ayther::engine`, `Ayther::ymfm` | SDL3, Vulkan, VMA, toml++, zstd |
| `ayther-engine-vpx-<tag>-<platform>.zip` | the above plus `Ayther::vpx` | the above plus libvpx |

Each archive is a complete Engine package with the public headers, compiled
SPIR-V, CMake exports, and notices. Windows bundles libvpx built by
`tools/build_libvpx.ps1`; Linux links the system libvpx and ships none of it.
None of these archives contains the Tessera provider.

The scope is enforced in four places:

- `tools/check_release_payload.ps1` checks the unpacked archive for the Engine,
  Core, and ymfm archives, target export, public headers, compiled shaders, and
  dependency notices.
- `tools/gen_release_sbom.ps1` takes the artifact family, so an SBOM cannot
  describe the two variants interchangeably.
- `tools/gen_release_notes.ps1` writes the scope table into the release body.
- `tools/finalize_release_assets.ps1` rejects a missing or extra release input
  and writes `CHECKSUMS.sha256` over the four final ZIPs only.

### Release workflow

`.github/workflows/release.yml` runs on a `v*` tag in four stages:

1. `validate` checks the version contract, refuses a workflow rerun or a tag
   that already owns a GitHub release, runs the locked Rust gates, and proves
   in optimized code that production trust opens packs and the development key
   is rejected.
2. `build` (four entries) configures a `*-release-engine` or
   `*-release-engine-vpx` preset, builds, runs CTest, installs, emits the SPDX
   2.3 SBOM, packages the install tree twice, requires byte-identical SHA-256
   digests, and checks the payload of the unpacked ZIP.
3. `attest` rejects any missing or extra input, writes `CHECKSUMS.sha256`,
   creates a keyless Sigstore bundle for every asset (up to three attempts per
   asset, each with fresh ephemeral keys), records SLSA build provenance and
   signed SBOM attestations, and uploads one candidate artifact.
4. `package-consumer` (four entries) downloads the candidate on a fresh runner,
   verifies SHA-256, Sigstore, and GitHub provenance before extraction, and
   builds and runs `tests/external_package_consumer` from `RUNNER_TEMP`.
   `tools/check_external_consumer_paths.ps1` then fails the release if any
   spelling of the producer workspace path appears in the consumer's cache or
   imported-target report.

`publish` depends on all four consumers, is the only job with
`contents: write`, waits on the protected `release` environment, and creates a
GitHub **pre-release** from the already verified bytes without rebuilding or
re-signing them. No long-lived signing key is used; OIDC credentials are
short-lived.

The build derives `SOURCE_DATE_EPOCH` from the tagged commit and remaps build
paths. ZIP entry times use midnight of the preceding UTC date, so timezone
conversion cannot make installed CMake files newer than a consumer build on
the day of publication.

A workflow that fails before `publish` creates no release. The tag stays, and
the fix ships under a new tag (as `v0.1.0-rc.26` and `v0.1.0-rc.27` did before
`v0.1.0-rc.28`).

### Native Tessera candidates

`.github/workflows/native-release.yml` runs on a `native-v0.1.0-rc.N` tag. On
`windows-2025-vs2026` it builds Tessera from the commit in
`dependencies/tessera.lock.json`, requires the installed Tessera package to
report that commit with a clean tree, generates the native fixtures and replay
evidence, builds and tests `windows-release-engine-vpx` with
`AYTHER_ENABLE_TESSERA=ON`, checks the payload, packages twice, and records
SLSA provenance. It publishes one archive,
`ayther-engine-vpx-native-v0.1.0-rc.N-windows-x86_64.zip`, plus
`CHECKSUMS.sha256`.

The release is created as a **draft** pre-release. A maintainer publishes it
once AYTHER Runtime pins its native lock to that candidate; a draft that is
never pinned stays unpublished. The native tag is pushed for the same commit as
the matching `v*` tag. Licensing of this combination is in
[Legal and distribution boundaries](LEGAL_AND_DISTRIBUTION.md#optional-tessera-provider).

### Runtime integration

`.github/workflows/runtime-integration.yml` runs after a successful `Release`
workflow (`workflow_run`) or by manual dispatch with an Engine tag and a
Runtime commit. It is deliberately outside the release DAG: a Runtime failure
cannot block an Engine release. It pins AYTHER Runtime by full commit SHA in
the workflow file, downloads the published archives, verifies them, and runs
Runtime's CTest suite on both platforms with the toolchains Runtime enforces:
Ubuntu 24.04 with `clang++-18`, and `windows-2025-vs2026` with the Visual
Studio 18 2026 generator and the `v145` toolset, built and tested as
`Release`. On Linux it excludes only `probe_core`, whose negative fixture is
the Windows-only `SDL3.dll`.

If the Runtime repository is private, configure the least-privilege
`AYTHER_RUNTIME_READ_TOKEN` Actions secret; otherwise the workflow uses its
ordinary token. It has read-only contents and attestation permissions and
cannot create or modify releases.

## Publishing a release candidate

The version contract accepts a pre-release suffix. `CMakeLists.txt`,
`include/ayther/ayther_version.h`, `Cargo.toml`, and `vcpkg.json` carry the core
`MAJOR.MINOR.PATCH`; `Cargo.toml` and `vcpkg.json` may additionally carry the
full tag.

```text
pwsh tools/check_release_version.ps1 -Tag v0.1.0-rc.N
git tag -a v0.1.0-rc.N -m 'AYTHER v0.1.0-rc.N'
git push origin v0.1.0-rc.N
git tag -a native-v0.1.0-rc.N -m 'Engine native-v0.1.0-rc.N'
git push origin native-v0.1.0-rc.N
```

Pushing the tags is the only manual step besides approving the `release`
environment and publishing the native draft. The repository controls that make
this a protected boundary are:

- the `release` environment with a branch policy and required reviewers;
- the `Immutable release tags` ruleset, which blocks update and deletion of
  `refs/tags/v*`;
- the `Protect main` ruleset (pull request, strict `Required CI gate`, no
  deletion or force push) and CODEOWNERS over `.github/workflows/` and the
  release tools.

The immutable-tag ruleset does not cover `native-v*` tags.

Required approving and Code Owner review are currently disabled under the
temporary
[single-maintainer exception](GOVERNANCE_EXCEPTIONS.md#gov-2026-001-single-maintainer-code-owner-review).
It does not provide independent review and must not be cited as such in
release evidence. The current decision is in
[Release go / no-go](RELEASE_GO_NO_GO_CURRENT.md).

To withdraw a published candidate, follow the
[release rollback runbook](RELEASE_ROLLBACK.md): keep the tag, preserve the
evidence, withdraw the distribution, notify consumers, and fix forward under a
new version.

## Verifying a release artifact

`tools/verify_release_artifact.ps1` runs the acceptance sequence: download,
checksum, Sigstore, provenance, unpack, payload check, and an out-of-tree
consumer build.

```text
pwsh tools/verify_release_artifact.ps1 -Tag v0.1.0-rc.28 -Product ayther-engine
```

A consumer can verify a downloaded archive with the three independent records:

```text
sha256sum --check CHECKSUMS.sha256
gh attestation verify ayther-engine-v0.1.0-rc.28-linux-x86_64.zip \
  --repo Ayther-Dev/AYTHER-Engine
cosign verify-blob \
  --bundle ayther-engine-v0.1.0-rc.28-linux-x86_64.zip.sigstore.json \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  --certificate-identity-regexp '^https://github.com/Ayther-Dev/AYTHER-Engine/.github/workflows/release.yml@refs/tags/v[0-9].*$' \
  ayther-engine-v0.1.0-rc.28-linux-x86_64.zip
```

Native candidates carry SHA-256 and SLSA provenance only; verify them with
`sha256sum` and `gh attestation verify`.

AYTHER Runtime's `bootstrap_ayther_engine.ps1` is the consumer path: it
downloads one archive and `CHECKSUMS.sha256`, checks the locked and published
SHA-256 values, verifies the SLSA attestation, and extracts the CMake prefix,
without Git, submodules, or an Engine checkout.

### Consuming a candidate as a frontend

`tools/check_rc_consumer.ps1` is a deeper, manually run session smoke:

```text
build/<preset>/bin/make_test_pack pack.ay trust.toml crc32:rc000001 rom.md
pwsh tools/check_rc_consumer.ps1 -Prefix <unpacked-artifact> -Core build/<preset>/bin/ayther_test_core.dll -Rom rom.md -Pack pack.ay -TrustRegistry trust.toml
```

It builds `tests/package_consumer` against an installed prefix, creates a
session, opens a **trusted** pack, steps frames, and reports what the renderer
and the audio device did. It refuses a prefix inside the checkout and a report
containing an absolute path into the repository, either of which would mean the
package was consumed from the source tree. `tools/make_test_pack` produces the
signed pack, its trust registry, and a synthetic ROM; it must sign, because an
optimized build refuses unsigned packs and the development key. Anything that
could not be exercised is reported as unavailable with a reason, not omitted.

## Release gate

No stable build is published until every item in
[Project status](PROJECT_STATUS.md#release-blockers) is closed and the
following evidence is archived:

1. clean configure/build/test/install/consume logs for every supported platform;
2. logs proving the release/protocol version contract and baselines agree;
3. a provisioned production trust registry and evidence that the documented
   rotation and revocation procedure is exercised by the shipping host;
4. complete transitive notices and source-offer obligations for shipped code;
5. a security review of pack parsing, paths, limits, scripting, FFI, and
   dynamic library loading;
6. release notes, checksums, provenance, the rehearsed
   [rollback procedure](RELEASE_ROLLBACK.md), and a supported version matrix.

Skipped tests must be reported as skipped. Missing hardware, ROMs, cores, or
fixtures must never be represented as a passing oracle. Release artifacts must
not contain ROMs, BIOS images, commercial game assets, private keys, or
emulator cores the project is not authorized to ship.
