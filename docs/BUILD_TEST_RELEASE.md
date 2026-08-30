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

## The test emulator core

`abi_negociacion` used to return CTest's skip code on every clean checkout. Its
positive half needed a core exporting `ayther_get_interface`, the only such core
lived in another repository, and `/third_party/cores/*` is gitignored -- so CI
measured the negotiation exactly never. A skipped test nobody can un-skip is a
note, not coverage.

`tools/test_core/` is that core, owned here and built from source on both
platforms. It emulates nothing: it produces a deterministic function of the ROM
bytes, the frame number, and the input, which is what an ABI oracle and a
determinism oracle actually need. It is built twice from one source --
`ayther_test_core` exports the ABI entry point and `ayther_test_core_stock` does
not -- so both halves of "the negotiation is additive" run on cores this
repository can ship.

All five ABI oracles now execute rather than skip: negotiation, read parity,
control writes, multilayer recomposition, and frame delta.

One fixture choice is worth knowing about. The parsed-sprite table the core
reports is a function of the ROM and the slot, NOT of the frame. A libretro core
is process-global, so the E-5 oracle's session and its control runner share the
one instance and step it alternately; a frame-varying table would compare two
different frames and disagree for a reason about the harness rather than the
engine. The cost is that this fixture cannot catch a stale published copy. The
frame-varying signals live in RAM, video, and audio, which no two observers read
alternately.

## End-to-end determinism

`tools/e2e_determinism/` runs the product rather than a seam: synthetic ROM,
test core, signed pack, and a scripted input track go into a real session, and
frames, audio, events, and work RAM come out hashed.

```text
ctest --preset windows-native -R e2e_determinismo
```

It asserts two different things. RUN-TO-RUN, two sessions in one process must
agree -- that catches nondeterminism from uninitialised memory, pointer values,
iteration order, or time. RUN-TO-GOLDEN, the hashes must equal constants pinned
in `tools/e2e_determinism/CMakeLists.txt` -- that is the half that catches a
difference BETWEEN platforms, because two runs agreeing on one machine prove
nothing about the other. CI running the same binary on Windows and on Linux is
what turns those constants into a cross-platform claim.

Only integer data is hashed. A float that rounded differently on two targets
would fail this test for a reason that has nothing to do with the engine.

A mismatch against the pinned hashes is a finding, not a number to refresh: it
means the engine changed behaviour, the test core changed, or the platforms
disagree, and which one it is has to be decided before re-pinning.

## GPU matrix

The GPU jobs are opt-in, on Windows and on Linux with Mesa, and they run through
`tools/check_gpu_matrix.ps1` rather than calling CTest directly:

```text
pwsh tools/check_gpu_matrix.ps1 -Preset windows-native-gpu
pwsh tools/check_gpu_matrix.ps1 -Preset linux-native-gpu -Launcher 'xvfb-run --auto-servernum'
```

The wrapper exists because the GPU oracles skip themselves when no Vulkan device
answers, and a run that skipped all eight and exited 0 is indistinguishable, in
a green tick, from one that rendered eight frames and compared them. So it
**fails** when the suite was skipped rather than executed, when fewer tests ran
than expected, or when no device was reported at all.

It records which device and driver answered -- `vk_context` logs the device
name, type, vendor, driver version, and Vulkan API version on every context
creation -- and writes a report naming anything omitted. The report is uploaded
whether the job passed or failed, because it matters most on the run that went
wrong.

## Release artifact scope

v0.1.x distributes **three artifact families**, on Windows and Linux:

| Archive | Targets | Headers | Shaders | Native dependencies |
|---|---|---|---|---|
| `ayther-core-<tag>-<platform>.zip` | `Ayther::core` | `ayther_core_ffi.h`, `ayther_version.h` | none | none |
| `ayther-engine-<tag>-<platform>.zip` | `Ayther::core`, `Ayther::engine`, `Ayther::ymfm` | the above plus the eleven-header engine allowlist and vendored ymfm | compiled SPIR-V | SDL3, Vulkan, VMA, vk-bootstrap, toml++, zstd |
| `ayther-engine-vpx-<tag>-<platform>.zip` | the above plus `Ayther::vpx` | the above plus `vpx/` when bundled | compiled SPIR-V | the above plus libvpx |

Windows builds and bundles libvpx through `tools/build_libvpx.ps1`, so the
archive, headers, and notices ship inside the package. Linux links the system
libvpx through `pkg-config` and ships none of it.

**A core-only package is never presented as the complete engine.** That is not
a convention, it is enforced in three places:

- `tools/check_release_payload.ps1` runs against the unpacked archive and checks
  in both directions. The engine kinds must contain their targets, headers,
  shaders, and dependency notices; the core kind must **demonstrably not**
  contain the engine archive, the ymfm archive, the engine target export, the
  shaders, or any of the eleven engine headers.
- `tools/gen_release_sbom.ps1` takes the artifact family, so an SBOM cannot
  describe a core-only tree as an "AYTHER Engine" distribution.
- `tools/gen_release_notes.ps1` writes the scope table into the release body,
  including the plain statement that `ayther-core` is not the engine.

Each family has its own out-of-tree consumer. `tests/package_consumer` asks for
`COMPONENTS engine` and links `Ayther::engine`; `tests/package_consumer_core`
asks for no components, links `Ayther::core`, and needs no toolchain file at
all -- if it ever did, the core package would have stopped being core-only.

## Publishing a release candidate

The version contract accepts a pre-release suffix. `CMakeLists.txt`,
`include/ayther/ayther_version.h`, `Cargo.toml`, and `vcpkg.json` all carry the
core `MAJOR.MINOR.PATCH`; `Cargo.toml` and `vcpkg.json` may additionally carry
the full tag, because only those two understand SemVer pre-release.

```text
pwsh tools/check_release_version.ps1 -Tag v0.1.0-rc.1
git tag -a v0.1.0-rc.1 -m 'AYTHER v0.1.0-rc.1'
git push origin v0.1.0-rc.1
```

Pushing the tag is the only manual step; everything after it is the workflow.
The publish job is gated on the `release` environment, so it waits for a
reviewer before any asset is signed or uploaded.

Before the tag is pushed, a repository administrator must have configured:

- the `release` environment with required reviewers;
- tag protection for `v*`, so the tag cannot be moved after publication;
- branch protection and CODEOWNERS over `.github/workflows/`, so the release
  mechanics cannot be changed without review.

Without those controls the workflow mechanics alone are not a protected release
boundary, and the resulting artifacts should not be treated as one.

## Consuming a release candidate as a frontend

The release workflow's package-consumer gate proves an artifact LINKS.
`tools/check_rc_consumer.ps1` proves it RUNS:

```text
build/<preset>/bin/make_test_pack pack.ay trust.toml crc32:rc000001 rom.md
pwsh tools/check_rc_consumer.ps1 -Prefix <unpacked-artifact>   -Core build/<preset>/bin/ayther_test_core.dll -Rom rom.md   -Pack pack.ay -TrustRegistry trust.toml
```

It configures and builds `tests/package_consumer` against an installed prefix,
then has it create a session, open a **trusted** pack, step frames, and report
what the renderer and the audio device did. The consumer prints basenames only,
and the script refuses a report containing any absolute path into this
repository -- such a path is both unreproducible for the reader and evidence
that the package was consumed from the source tree rather than from an install.
The script also refuses a prefix inside the checkout for the same reason.

`tools/make_test_pack` produces the signed pack, its trust registry, and a
synthetic ROM, so the release job depends on no external content. It has to be
signed: an optimized build refuses an unsigned pack and refuses the development
key, so signed content plus its registry is the only combination that opens.

Anything that could not be exercised is reported as unavailable with a reason
rather than omitted. A report that quietly dropped the audio device because
there was none would read, later, exactly like one where audio worked.

## Configuration matrix

| Preset family | Intended role | Current confidence |
|---|---|---|
| `*-headless` | core, bridge, and CPU/ABI checks | Windows verified; Linux pending |
| `*-release` | optimized core-only package (`ayther-core`) | Windows build/test/install/consume verified; Linux pending |
| `*-release-engine` | optimized complete engine package (`ayther-engine`) | Windows build/test/install/package/consume verified; Linux pending |
| `*-release-engine-vpx` | optimized complete engine with VP9 (`ayther-engine-vpx`) | Windows verified; Linux pending |
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
and then builds **six** artifacts: three families on each of Windows and Linux.
It derives `SOURCE_DATE_EPOCH` from the tagged commit, remaps build paths,
packages each install tree twice, and requires byte-identical SHA-256 digests
before publication.

Every matrix entry runs, in this order: configure, build, the native CTest
suite for that preset, install into an isolated prefix, SBOM, package twice,
reproducibility check, unpack the archive into an empty directory, verify the
unpacked payload against the family its name advertises, then configure, build,
and run an out-of-tree consumer against it. Everything after packaging is
checked on the UNPACKED ARCHIVE rather than the install tree it came from, so a
packaging bug cannot slip past. A failure at any step drops that artifact, and
the publish job refuses to run unless all six are present and correctly named.

Each candidate carries an SPDX 2.3 SBOM generated from the locked Cargo graph
and exact installed files. The protected publish job creates SHA-256 checksums,
keyless Sigstore bundles for every asset, SLSA build provenance, and signed SBOM
attestations, then creates a GitHub pre-release. Private signing keys and
long-lived CI credentials are not used; OIDC credentials are short-lived.

Repository administrators must configure the `release` environment with
required reviewers, protect `v*` tags, and restrict workflow changes with
CODEOWNERS/branch protection. Without those repository controls, the workflow
mechanics alone are not a protected release boundary.

`tools/verify_release_artifact.ps1` runs the whole acceptance sequence from a
clean checkout of the tag -- download, checksum, Sigstore, provenance, unpack,
payload check, and an out-of-tree consumer build:

```text
pwsh tools/verify_release_artifact.ps1 -Tag v0.1.0-rc.1 -Product ayther-engine
```

A consumer verifies a downloaded archive with all three independent records:

```text
sha256sum --check CHECKSUMS.sha256
gh attestation verify ayther-engine-v0.1.0-rc.1-linux-x86_64.zip \
  --repo Ayther-Dev/AYTHER-Engine
cosign verify-blob \
  --bundle ayther-engine-v0.1.0-rc.1-linux-x86_64.zip.sigstore.json \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  --certificate-identity-regexp '^https://github.com/Ayther-Dev/AYTHER-Engine/.github/workflows/release.yml@refs/tags/v[0-9].*$' \
  ayther-engine-v0.1.0-rc.1-linux-x86_64.zip
```

The physical GPU/driver matrix, real-emulator fixtures, and the remaining
security review are still release blockers; automated publications
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
