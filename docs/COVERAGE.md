# Coverage policy

Coverage is a required CI gate, reported independently for the Rust core and
the first-party C++ surface. Each language has a total floor and a floor for
the lines a change touches. `tools/check_coverage.py` enforces both and lists
the exact files and line ranges left uncovered.

## Thresholds

Thresholds live in `.github/coverage-thresholds.json`. Changing them requires
a reviewed commit together with a new baseline measurement.

| Component | Minimum total | Minimum changed lines | Measured scope |
| --- | ---: | ---: | --- |
| Rust | 74% | 80% | `core/src/` |
| C++ | 50% | 70% | `include/`, `src/`, `tests/`, `tools/` |

C++ changed lines are held to 70% rather than 80% because the renderer has
device- and driver-dependent branches that a single software Vulkan device
cannot exercise.

## Measurements

| Component | Commit | Source | Line coverage |
| --- | --- | --- | ---: |
| Rust | `92b2538` (2026-08-30) | baseline, `cargo llvm-cov` | 75.24% (12,727 / 16,915) |
| C++ | `92b2538` (2026-08-30) | baseline, `windows-native-coverage` | 62.00% (11,936 / 19,251) |
| Rust | `10100fe` (2026-11-19) | CI run `44506963494` | 78.71% (15,715 / 19,965) |
| C++ | `10100fe` (2026-11-19) | CI run `44506963494`, Linux CPU/GPU merged with Windows Tessera-native | 81.01% (56,335 / 69,537) |

The thresholds were set from the baseline and have not been raised since.

All figures are the ones the gate reports, which are what the thresholds are
compared against. They are not the figures `llvm-cov report` prints: that
summary sums the LCOV `LF`/`LH` fields, and llvm-cov emits one `DA` record per
region, so a line with several regions is counted several times. The gate
merges by line number; a line is covered when any of its counters ran.

## How the gates work

Rust uses one job, `Rust coverage gate`, running `cargo llvm-cov` over the
workspace.

C++ combines two collectors before the gate:

- `C++ coverage collection (Linux)` builds `linux-native-coverage` with clang
  18 (the compiler and `llvm-profdata`/`llvm-cov` must be one LLVM), runs the
  CPU suite, then runs the `gpu` suite against Mesa's Lavapipe software Vulkan
  device into the same profile set.
- The Windows native job builds `windows-native-coverage` with
  `AYTHER_ENABLE_TESSERA=ON` and runs the four `ayther.integration.tessera_*`
  tests (`session`, `pack`, `presentation`, `controls`) against fixtures
  generated from the locked Tessera, because Linux cannot exercise the
  Windows-only Tessera integration. It fails unless all four are registered.

`C++ coverage gate` concatenates the Linux `cpp.lcov` and the Windows
`native.lcov` and enforces the unchanged C++ thresholds. A missing platform
report fails the gate.

The changed-line gate compares each report with the pull-request base (or the
previous commit on a branch push). Only changed lines LLVM identifies as
executable enter the denominator; a change with no executable lines is
reported as such. The two gates are independent, and the changed-line gate is
the one that stops regressions: a large, well-covered codebase absorbs an
uncovered change into its average almost invisibly.

### Linux GPU coverage on a headless runner

- SDL's X11 development headers are installed before vcpkg builds SDL, which
  otherwise compiles without its X11 backend.
- GPU tests use SDL's `offscreen` video driver; Lavapipe exposes
  `VK_EXT_headless_surface`, so no X server is involved.
- `SDL_VULKAN_LIBRARY` points SDL at the Vulkan loader the test executables
  link, so SDL surface creation and Engine instance calls share one loader.
- The runner discovers Mesa's installed `lvp_icd*.json` manifest, sets
  `VK_DRIVER_FILES` and `VK_ICD_FILENAMES` to it, and checks Lavapipe with
  `vulkaninfo` before CTest.
- `check_gpu_matrix.ps1` rejects an empty or skipped GPU run.

Release budget tests gate timing only in optimized builds. Sanitizer and
coverage builds keep behavior and equality checks but report the timing
budget as `not-gated-instrumented`.

## Scope and exclusions

Rust coverage includes `core/src/`; `cargo llvm-cov` omits dependency sources
outside the workspace. C++ coverage includes first-party production code,
headers, tests, and tools under `src/`, `include/`, `tests/`, and `tools/`.

The C++ collectors exclude only:

- `third_party/`, `vcpkg_installed/`, and `_deps/`: vendored or
  package-manager sources maintained outside this project;
- `CMakeFiles/`: generated compiler-identification and build-system sources.

Any new exclusion needs a documented generated-code or third-party
justification in this file.

Tests and tools are deliberately not excluded, and they lift the headline
number: test and tool code is almost fully executed by definition. At the
`92b2538` baseline the C++ total split as follows (`llvm-cov report` figures,
so they sum to its 62.28% summary rather than the gate's 62.00%):

| Scope | Covered / total | Line coverage |
| --- | ---: | ---: |
| `src/` | 5,234 / 12,219 | 42.83% |
| `include/` | 919 / 1,222 | 75.20% |
| `tests/` | 4,133 / 4,212 | 98.12% |
| `tools/` | 2,224 / 2,435 | 91.33% |
| engine only (`src/` + `include/`) | 6,153 / 13,441 | 45.78% |

The total is therefore not "the engine's coverage". A separate engine-only
threshold has not been introduced. The split has not been re-measured since the
baseline.

## Critical positive and negative paths

| Path | Positive cases | Negative cases |
| --- | --- | --- |
| Trust | live and rotating keys open signed packs in `pack_trust.rs` and `pack_trust_ffi_test.cpp` | unknown, expired, revoked, wrong-scope, and tampered signatures are refused |
| FFI | valid handles, ABI layouts, reads, and lifecycle calls in `ayther_core_ffi_test.cpp` | null handles, invalid arguments, missing entries, and out-of-range operations fail safely |
| Pack loading | signed fixtures open, expose metadata/assets, reload, and validate in `archive_vfs.rs` and `pack_runtime_test.cpp` | missing, malformed, traversal, high-expansion, extra-entry, and tampered packs are rejected |
| Runtime options | defaults and valid environment overrides in `runtime_options_test.cpp` | malformed, trailing, negative, and out-of-range values invalidate the option set or fall back safely |

## Local reproduction

Rust:

```sh
mkdir -p coverage/rust
cargo llvm-cov --workspace --all-targets --locked --lcov \
  --output-path coverage/rust/rust.lcov
cargo llvm-cov report --summary-only
cargo llvm-cov report --html --output-dir coverage/rust/html
python3 tools/check_coverage.py --component rust \
  --lcov coverage/rust/rust.lcov --base origin/main
```

C++ on Windows. The instrumentation supplies `/Od /Zi`, so the preset keeps
`RelWithDebInfo`; a Debug build would select the debug CRT and mismatch the
release-built vcpkg tree. `llvm-profdata` and `llvm-cov` must come from the
same LLVM as `clang-cl`.

```powershell
cmake --preset windows-native-coverage
cmake --build --preset windows-native-coverage
$env:LLVM_PROFILE_FILE = "$PWD/build/windows-native-coverage/coverage-profiles/%m-%p.profraw"
ctest --preset windows-native-coverage
bash tools/collect_cpp_coverage.sh build/windows-native-coverage coverage/cpp
python tools/check_coverage.py --component cpp --lcov coverage/cpp/cpp.lcov --base origin/main
```

To reproduce the Windows Tessera-native collector, configure the same preset
with `-DAYTHER_ENABLE_TESSERA=ON`, `Tessera_DIR`,
`AYTHER_TESSERA_TEST_SOURCE_DIR`, and `AYTHER_TESSERA_TEST_FIXTURE_DIR` (see
[Tessera native provider](TESSERA_NATIVE.md)), build and run the four
`ayther.integration.tessera_*` tests, then run
`tools/collect_cpp_coverage_windows.ps1 -BuildDirectory build/windows-native-coverage -OutputDirectory coverage/cpp-windows`.
Run `tools/check_coverage.py` on the concatenation of the Linux `cpp.lcov` and
the Windows `native.lcov` to reproduce the combined gate.

C++ on Linux, with clang 18 and its LLVM tools:

```sh
cmake --preset linux-native-coverage \
  -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18
cmake --build --preset linux-native-coverage
mkdir -p build/linux-native-coverage/coverage-profiles
export LLVM_PROFILE_FILE="$PWD/build/linux-native-coverage/coverage-profiles/%m-%p.profraw"
ctest --preset linux-native-coverage
lvp="$(find /usr/share/vulkan/icd.d -maxdepth 1 -name 'lvp_icd*.json' -print -quit)"
SDL_VIDEODRIVER=offscreen LIBGL_ALWAYS_SOFTWARE=1 \
  SDL_VULKAN_LIBRARY="$PWD/build/linux-native-coverage/vcpkg_installed/x64-linux/debug/lib/libvulkan.so" \
  VK_DRIVER_FILES="$lvp" VK_ICD_FILENAMES="$lvp" \
  pwsh -File ./tools/check_gpu_matrix.ps1 \
    -Preset linux-native-coverage-gpu \
    -ReportFile coverage-gpu-report.md
LLVM_TOOLS_VERSION=18 bash tools/collect_cpp_coverage.sh build/linux-native-coverage coverage/cpp
python3 tools/check_coverage.py --component cpp \
  --lcov coverage/cpp/cpp.lcov --base origin/main
```
