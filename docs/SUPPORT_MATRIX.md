# Support matrix

**Last measured:** 2026-08-30, at commit `846081e`.

This matrix separates three things that are easy to blur together, because
blurring them is how a support promise becomes untrue:

- **Verified** — run on this checkout, with the result recorded below.
- **CI-attempted** — a job in `.github/workflows/ci.yml` targets it, but the
  job has not been observed passing. A configured job is not a passing job.
- **Unverified** — nothing has measured it. Not a commitment.

Nothing here is a support commitment yet. `0.1.x` is pre-release; see
[Project status](PROJECT_STATUS.md) for the open release blockers.

## Operating systems

| OS | Core | Engine | Status | Evidence |
| --- | --- | --- | --- | --- |
| Windows 11 x86_64 | yes | yes | Verified | The per-preset results below, measured on this machine |
| Linux x86_64 | yes | yes | CI-attempted, never green | The only CI run in this repository's history (`33339406053`) failed every Linux native job. `Headless (Linux)` passed; `native`, `native-vpx`, ASan, UBSan, and C++ coverage all failed to compile |
| macOS | — | — | Unverified | No presets, no jobs, no commitment |
| Mobile, WebAssembly, consoles | — | — | Unverified | Not a target of this repository |

## Architectures

| Architecture | Status | Evidence |
| --- | --- | --- |
| x86_64 | Verified on Windows; not yet green on Linux | Every preset and job targets `x86_64` |
| aarch64 / ARM | Unverified | No preset, no job, and no cross-compilation has been attempted |

There is no 32-bit target. The vendored VP9 decoder ships an `x86` import
library, but nothing in this repository builds or tests a 32-bit configuration.

## Compilers and toolchain

| Component | Verified version | Notes |
| --- | --- | --- |
| Clang (`clang-cl` driver) | 22.1.6, target `x86_64-pc-windows-msvc` | The Windows compiler for every preset |
| Clang (`clang++` driver) | Not yet green | The Linux compiler for every preset; see the Linux row above |
| rustc / cargo | 1.95.0 | Pinned by `rust-toolchain.toml` |
| CMake | 4.3.3 | Presets require 3.21 or newer |
| Ninja | 1.13.2 | The generator for every preset |

MSVC's `cl.exe` is **not** supported. `CMakeLists.txt` warns when the compiler
is not Clang. The warnings policy has an MSVC-style branch (`/W4 /WX`) because
`clang-cl` sets `MSVC` in CMake; that branch has never been exercised with
`cl.exe` itself, and claiming `cl.exe` support on the strength of it would be
claiming something nobody has run.

## GPU backend

| Backend | Status | Evidence |
| --- | --- | --- |
| Vulkan | Verified on one device | 8 of 8 GPU oracles passed on an NVIDIA GeForce RTX 3060 Laptop GPU, driver 616.224.0, Vulkan API 1.4.351 |
| Vulkan on Linux / Mesa | Never executed | The `GPU (Linux, opt-in)` job is defined to run under `xvfb` with `mesa-vulkan-drivers`, but it is opt-in and has been skipped in every run so far |
| Direct3D, Metal, OpenGL | Not implemented | The renderer is Vulkan-only |

One device is one device. A single NVIDIA laptop GPU is evidence that the
oracles pass somewhere, not that the driver matrix is covered; AMD, Intel, and
older drivers are unmeasured. The GPU jobs record the device and driver that
answered and fail when the suite is skipped rather than run, so a green GPU job
always names the hardware behind it.

## VPX / VP9 configurations

| Configuration | Status | Evidence |
| --- | --- | --- |
| Disabled (default) | Verified | `windows-native`: 46 of 46 CTests |
| Windows, bundled libvpx | Verified | `windows-native-vpx`: 49 of 49 CTests, decoder built by `tools/build_libvpx.ps1` |
| Linux, system libvpx | CI-attempted, never green | `linux-native-vpx` resolves `libvpx-dev` through `pkg-config` and nothing is bundled into the artifact, but the job has not yet passed |

The two platforms provision the decoder differently on purpose, and it changes
what ships: the Windows artifact carries the libvpx archive, headers, and
notices, while the Linux one links the system library and carries none of them.

## Measured results

All from commit `846081e` on 2026-08-30, Windows 11 x86_64, clang-cl 22.1.6.

Two columns, because they differ and the difference matters. *Developer
machine* is this checkout, which has the fork emulator cores present in
`third_party/cores/`. *Clean clone* is a fresh `git clone` with nothing
supplied, which is what a new contributor and CI actually get.

| Suite | Developer machine | Clean clone |
| --- | --- | --- |
| Rust (`cargo test --workspace --all-targets --locked`) | 400 passed, 1 ignored | same |
| `windows-headless` | 3 of 3 | 3 of 3 |
| `windows-native` | 46 passed, 0 failed | 42 passed, 0 failed, 4 skipped |
| `windows-native-vpx` | 49 passed, 0 failed | not re-measured |
| `windows-native-coverage` (unoptimised, `/Od`) | 46 passed, 0 failed | not re-measured |
| `windows-native-gpu` | 8 of 8 | not re-measured |
| Flat C FFI contract checks | 132 of 132 | 132 of 132 |
| Line coverage, Rust | 75.24% (12,727 / 16,915) | — |
| Line coverage, C++ | 62.00% (11,936 / 19,251) | — |

Zero tests fail in either column. Four skip on a clean clone.

### The four that skip, and why

`audio_mute`, `audio_output`, `render_output`, and `subsystem_routing` each
hard-code a path to `third_party/cores/genesis_plus_gx_libretro_vram.dll`, a
fork emulator core that `.gitignore` excludes and that a developer supplies
themselves. Absent that file they return CTest's skip code 77.

This is a gap in the tests, not a missing capability. Five oracles hard-code
that path; the other two, `ayther_abi_smoke` and `abi_read_parity`, first
consult `AYTHER_ABI_CORE`, which the build already points at the in-repo
`ayther_test_core`, so they run everywhere. The same fallback has not been
extended to these four. The test core does carry an `audio_mute_mask`, so the
fallback is plausible rather than blocked, but whether it makes each assertion
*meaningful* has not been established, and a test that runs without measuring
anything would be worse than one that honestly skips.

Until that is done, a clean-clone `windows-native` run is 42 passed and 4
skipped. A skip is not an approval. `abi_negociacion`, which used to skip for
this same reason, is the precedent for closing it: it now builds its core from
source in `tools/test_core/`.

## The state of Linux

Linux is a target of this repository, not yet a supported one. Every Linux
native job failed in the only CI run on record, all for the same reason:
`src/ayther_renderer.cpp` used `std::fmod` and `std::lround` without including
`<cmath>`. Windows compiles that because its standard library headers pull
`<cmath>` in transitively; libstdc++ does not, so the same source was portable
by accident.

That specific defect is fixed — the include was added in `3acfdab` — but the
fix has never been pushed or re-validated, so no Linux build has yet been
observed succeeding. Until one is, treat every Linux row above as an intention
backed by a configured job, not as evidence.

## Deep paths on Windows

A checkout whose path is long enough to push intermediate build artifacts past
the 260-character limit fails to link with `LNK1104`, naming a Cargo build
script it cannot open. This was hit at a path of roughly 250 characters. It is a
Windows limit rather than a defect in this repository, but the failure names a
missing `.exe` and reads like a toolchain problem, so it is recorded here: keep
the checkout reasonably close to the drive root, or enable long paths.
