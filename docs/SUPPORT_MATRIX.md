# Support matrix

**Status:** pre-release; nothing here is a support commitment

**Last reviewed:** 2026-11-19, against `main` at `6d1c2e8` (`v0.1.0-rc.28`)

This matrix records which configurations are built and tested, and where the
evidence comes from. It uses three levels:

- **CI-verified** — a required job in `.github/workflows/ci.yml` or a release
  workflow builds and tests the configuration, and the referenced run passed.
- **Built, not tested** — produced or compiled, but no test run executes it.
- **Unverified** — nothing measures it.

`0.1.x` is pre-release; see [Project status](PROJECT_STATUS.md) for the open
release blockers. For current results, read the latest required CI run rather
than counts copied into documents.

## Evidence

| Run | Scope | Result |
|---|---|---|
| [CI 44529951482](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/44529951482) | Full pull-request matrix for #62, the head of `v0.1.0-rc.28` | All required jobs passed; the opt-in GPU job was skipped |
| [Release 44543445802](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/44543445802) | `v0.1.0-rc.28`: four packages built, consumed from a fresh runner, signed, published | Passed |
| [Native release 44543445839](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/44543445839) | `native-v0.1.0-rc.28`: Windows VPX + Tessera package | Passed |
| [Runtime integration 44553987318](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/44553987318) | Pinned AYTHER Runtime built and tested against the published packages | Passed |

Pushes to `main` run a reduced job set; the full matrix runs on pull requests.

## Operating systems and architectures

| Platform | Core | Engine | Tessera provider | Status |
|---|---|---|---|---|
| Windows x86_64 | yes | yes | yes | CI-verified |
| Linux x86_64 | yes | yes | no | CI-verified |
| macOS | — | — | — | Unverified; no presets or jobs |
| aarch64 / ARM, 32-bit x86 | — | — | — | Unverified; no presets or jobs |
| Mobile, WebAssembly, consoles | — | — | — | Not a target |

## CI configurations

| Configuration | Windows | Linux |
|---|---|---|
| Rust quality gates (fmt, clippy, tests, docs, audit) | CI-verified | CI-verified |
| Headless (core + C++ ABI oracle) | CI-verified | CI-verified |
| Native engine, VPX disabled, + package consumer | Built by the release workflow only | CI-verified |
| Native engine, VPX enabled, + package consumer | CI-verified, including the Tessera provider tests | CI-verified |
| ASan + UBSan | — | CI-verified |
| Fuzz smoke | — | CI-verified |
| Rust and C++ coverage gates | — | CI-verified ([Coverage policy](COVERAGE.md)) |
| GPU oracles | Opt-in, skipped by default | Opt-in, skipped by default |

The release workflow builds, tests, packages, and consumes both variants on both
platforms; see [Build, test, and release](BUILD_TEST_RELEASE.md).

## Release packages

| Package | Platforms | Tag | Contains Tessera |
|---|---|---|---|
| `ayther-engine` | Windows x86_64, Linux x86_64 | `v0.1.0-rc.N` | No |
| `ayther-engine-vpx` | Windows x86_64, Linux x86_64 | `v0.1.0-rc.N` | No |
| `ayther-engine-vpx-native` | Windows x86_64 | `native-v0.1.0-rc.N` | Yes ([Tessera native provider](TESSERA_NATIVE.md#release-packaging)) |

Regular packages ship with SPDX SBOMs, a SHA-256 checksum file, Sigstore
bundles, and SLSA provenance.

## Toolchain

| Component | Version | Notes |
|---|---|---|
| Clang, `clang-cl` driver (Windows) | LLVM 22.1.8 | Pinned by `tools/install_pinned_llvm.ps1` in CI and release jobs |
| Clang, `clang++` driver (Linux) | Runner default | Not pinned |
| Rust | 1.95.0 | Pinned by `rust-toolchain.toml` |
| CMake | 3.21 or newer | Required by `CMakeLists.txt` and the presets |
| Ninja | — | Generator for every preset |

MSVC `cl.exe` is not supported; configure warns when the compiler is not Clang.
The warnings policy has an MSVC-style branch because `clang-cl` sets `MSVC` in
CMake; that branch has not been exercised with `cl.exe`.

## GPU backend

| Backend | Status |
|---|---|
| Vulkan | Implemented. GPU oracles run through the opt-in `GPU` job or the `*-native-gpu` presets; `tools/check_gpu_matrix.ps1` records the answering device and driver and fails when the suite is skipped |
| Direct3D, Metal, OpenGL | Not implemented |

No hardware matrix is maintained. A GPU result is evidence for the recorded
device and driver only.

## VP9 (VPX)

| Platform | Decoder source |
|---|---|
| Windows | libvpx built locally by `tools/build_libvpx.ps1` and bundled with its headers and notices |
| Linux | System `libvpx` found through `pkg-config`; nothing is bundled |

## Emulation providers

| Provider | Systems | Status |
|---|---|---|
| libretro, AYTHER-aware core ([extension ABI](EMULATOR_EXTENSION_ABI.md) 1.10) | Mega Drive / Genesis is the system the observation model is built and tested for | CI-verified with the in-repo test core; real-core oracles need the locked Genesis Plus GX fork core |
| libretro, stock core | Any libretro system | Loads and runs with reduced observation |
| Tessera native | Mega Drive / Genesis | CI-verified on Windows x64 ([Tessera native provider](TESSERA_NATIVE.md)) |

## Tests that need external inputs

Some oracles need inputs this repository does not ship and report CTest skip
code 77 without them: the Genesis Plus GX fork core named by
`third_party/cores/core.lock`, Tessera fixtures, or GPU hardware. A skip is not a
pass. The ABI oracles use the in-repo `tools/test_core` instead and always run.

## Known environment limits

A Windows checkout deep enough to push build artifacts past the 260-character
path limit fails to link with `LNK1104`, naming a Cargo build script. Keep the
checkout near the drive root or enable long paths.
