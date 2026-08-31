# Coverage policy

Coverage is a required CI gate, reported independently for the Rust core and
the first-party native C++ surface. Both jobs publish an LCOV file, a textual
summary, a gate report with uncovered line ranges, and an HTML source report.

## Initial baseline and thresholds

Thresholds live in `.github/coverage-thresholds.json`; changing them requires a
reviewed commit together with a new baseline measurement.

| Component | Baseline | Measured line coverage | Minimum total | Minimum changed lines |
| --- | --- | ---: | ---: | ---: |
| Rust | `92b2538`, `cargo llvm-cov` | 75.24% (12,727 / 16,915) | 74% | 80% |
| C++ | `92b2538`, `windows-native-coverage` | 62.00% (11,936 / 19,251) | 50% | 70% |

Both numbers are the ones the gate itself reports, which is the only figure
worth writing down: it is what the threshold is compared against.

They are NOT the figures `llvm-cov report` prints. That summary sums the LCOV
`LF`/`LH` fields, and llvm-cov emits one `DA` record per region, so a line with
several regions is counted several times. The gate merges by line number -- a
line is covered when any of its counters ran -- which is the ordinary
definition of line coverage. The summary reports 74.93% for Rust and 62.28% for
C++ against the gate's 75.24% and 62.00%; the difference is entirely that
double counting.

The C++ baseline was measured on Windows, because that is the platform
available to whoever recorded it. CI measures on Linux, where the compiled file
set differs. That is why the floor sits twelve points under the measurement
rather than immediately beneath it: the first Linux run should replace this row
with its own numerator and denominator and tighten the floor to match. Setting
it just below a Windows number would have made the first Linux run red for a
reason that is not a regression.

The changed-line gate compares each report with the pull-request base (or the
previous commit on a branch push). Only changed lines LLVM identifies as
executable enter that denominator. A change with no executable lines is
reported as such and does not manufacture a percentage.

The two gates are independent, and the changed-line one is the barrier that
actually stops regressions: a large, well-covered codebase absorbs an uncovered
new change into its average almost invisibly. Measured on a synthetic
repository, a change adding three uncovered lines to a 97%-covered file fails
the gate on the changed-line rule while the total rule passes comfortably.

C++ changed lines are held to 70% rather than 80% for a stated reason: the
coverage job runs the CPU test suite, and tests labelled `gpu` are excluded
because the runners promise no Vulkan device. Code under `src/vulkan_backend/`
therefore cannot be covered by this job at all, and an 80% rule would block
legitimate renderer work for a reason unrelated to test quality. Raising it
requires either a GPU coverage run or accepting that friction deliberately.

## Scope and exclusions

Rust coverage includes `core/src/`; `cargo llvm-cov` omits dependency sources
outside the workspace. Native coverage includes first-party production code,
headers, tests, and tools under `src/`, `include/`, `tests/`, and `tools/`.

The native collector excludes only:

- `third_party/`, `vcpkg_installed/`, and `_deps/`: vendored or package-manager
  sources maintained outside this project;
- `CMakeFiles/`: generated compiler-identification and build-system sources.

Tests and tools are intentionally not excluded, and that has a consequence
worth stating rather than discovering: test and tool code is almost fully
executed by definition, so it lifts the headline number well above what the
engine alone scores. The C++ baseline breaks down as

| Scope | Covered / total | Line coverage |
| --- | ---: | ---: |
| `src/` | 5,234 / 12,219 | 42.83% |
| `include/` | 919 / 1,222 | 75.20% |
| `tests/` | 4,133 / 4,212 | 98.12% |
| `tools/` | 2,224 / 2,435 | 91.33% |
| total, as gated | 12,510 / 20,088 | 62.28% |
| engine only (`src/` + `include/`) | 6,153 / 13,441 | 45.78% |

(These are the `llvm-cov report` figures, so they sum to the 62.28% summary
rather than the gate's deduplicated 62.00%; the split between scopes is the
point here, not the last decimal.)

So 62% is not "the engine is 62% covered". The engine is closer to 46%, and the
total is the number the gate happens to enforce. Splitting the two into separate
thresholds would be the honest next step; it is not done here because the
acceptance for this change limits exclusions to generated and third-party code,
and carving the tests out of the denominator is neither.

Any new exclusion needs a documented generated-code or third-party
justification in this file.

## Critical positive and negative paths

| Path | Positive cases | Negative cases |
| --- | --- | --- |
| Trust | live and rotating keys open signed packs in `pack_trust.rs` and `pack_trust_ffi_test.cpp` | unknown, expired, revoked, wrong-scope, and tampered signatures are refused |
| FFI | valid handles, ABI layouts, reads, and lifecycle calls in `ayther_core_ffi_test.cpp` | null handles, invalid arguments, missing entries, and out-of-range operations fail safely |
| Pack loading | signed fixtures open, expose metadata/assets, reload, and validate in `archive_vfs.rs` and `pack_runtime_test.cpp` | missing, malformed, traversal, high-expansion, extra-entry, and tampered packs are rejected |
| Runtime | defaults and valid environment overrides in `runtime_options_test.cpp` | malformed, trailing, negative, and out-of-range values invalidate the option set or fall back safely |

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

C++ on Windows, which is what the recorded baseline used. The instrumentation
supplies `/Od /Zi` itself, so the preset deliberately does not switch the build
type to Debug: that would select the debug CRT and mismatch the release-built
vcpkg tree.

```powershell
cmake --preset windows-native-coverage
cmake --build --preset windows-native-coverage
$env:LLVM_PROFILE_FILE = "$PWD/build/windows-native-coverage/coverage-profiles/%m-%p.profraw"
ctest --preset windows-native-coverage
bash tools/collect_cpp_coverage.sh build/windows-native-coverage coverage/cpp
python tools/check_coverage.py --component cpp --lcov coverage/cpp/cpp.lcov --base origin/main
```

`llvm-profdata` and `llvm-cov` must be the ones matching the compiler; a
different LLVM version cannot read the profiles.

C++ on Linux with LLVM tools available:

```sh
cmake --preset linux-native-coverage
cmake --build --preset linux-native-coverage
mkdir -p build/linux-native-coverage/coverage-profiles
LLVM_PROFILE_FILE="$PWD/build/linux-native-coverage/coverage-profiles/%m-%p.profraw" \
  ctest --preset linux-native-coverage
bash tools/collect_cpp_coverage.sh build/linux-native-coverage coverage/cpp
python3 tools/check_coverage.py --component cpp \
  --lcov coverage/cpp/cpp.lcov --base origin/main
```
