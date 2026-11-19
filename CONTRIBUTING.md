# Contributing to AYTHER Engine

AYTHER Engine combines Rust and C++20 behind a strict ABI/FFI boundary, so
every change must be small, reproducible, and accompanied by the test that
proves it.

## Principles

1. A commit represents a single coherent change.
2. Do not mix functional changes with refactors unless the refactor is
   essential to the change.
3. Every bug fix adds or updates a test that fails before the fix.
4. Do not add ROMs, BIOS images, libretro core binaries, private keys, or
   third-party copyrighted material without explicit authorization.
5. Do not add dependencies through absolute paths or directories outside the
   repository.
6. A change to shared types updates Rust, C++, the CXX bridge, and the ABI
   tests in the same change.
7. Documentation describes current behavior, and labels specified and planned
   behavior as such.

## Development environment

Set up the toolchain with the
[development environment guide](docs/DEVELOPMENT_ENVIRONMENT.md). The
environment is defined by `rust-toolchain.toml`, `CMakePresets.json`, and
`vcpkg.json`. Do not commit `CMakeUserPresets.json` or machine-specific paths.

Required tools:

- Git;
- CMake 3.21 or later and Ninja;
- Rust 1.95.0 (selected by `rust-toolchain.toml`);
- LLVM/Clang: `clang-cl` 22 or later on Windows, Clang 18 or later on Linux;
- clang-format 18, the version CI checks with;
- vcpkg for native presets;
- Visual Studio Build Tools with the C++ workload on Windows;
- the Vulkan SDK for renderer work;
- PowerShell 7.

## Workflow

1. Before starting, check open and recent pull requests so the work is not
   duplicated.
2. Create a short-lived branch from an up-to-date `origin/main`.
3. Implement a single functional unit with its tests.
4. Format the code (`cargo fmt`; clang-format 18 for C and C++).
5. Run the affected tests and the relevant integration tests.
6. Update the documentation, and add an entry under `[Unreleased]` in
   [CHANGELOG.md](CHANGELOG.md) for any user-visible change.
7. Open a pull request against `main`. It merges only when the
   `Required CI gate` check passes on a branch that is up to date with `main`.

[Conventional Commits](https://www.conventionalcommits.org/) are used, for
example:

- `feat(session): ...`
- `fix(tessera): ...`
- `perf(session): ...`
- `test(engine): ...`
- `build(cmake): ...`
- `ci(release): ...`
- `docs: ...`

## Quality checks

Before opening a pull request, run the checks that apply to the change, as
listed in [Build, test, and release](docs/BUILD_TEST_RELEASE.md):

- Rust: the four locked `cargo` commands.
- Documentation, dependencies, or license metadata: the repository policy
  checks, including `pwsh tools/check_doc_references.ps1`.
- C++: configure, build, and test the affected preset (`windows-native` or
  `linux-native` for engine code; `*-headless` for the core and ABI).
- Public headers or packaging: install and run `tests/package_consumer/` from
  outside the source tree.
- Tessera integration: the `ayther.integration.tessera_*` tests, as described
  in [Tessera native provider](docs/TESSERA_NATIVE.md).

CI runs the complete set, including sanitizers, fuzz smoke, and the coverage
gates in [Coverage policy](docs/COVERAGE.md). New code must meet the
changed-line coverage floor.

Do not report a test as passing if it was skipped because a core, ROM, GPU, or
fixture was unavailable. Skips must remain visible.

## C++ documentation

Write all first-party C++ comments and API documentation in English. Use
Doxygen-compatible comments for public declarations, and document only what
the declaration cannot express:

- ownership, and whether pointers or references are borrowed;
- preconditions, postconditions, invalidation, and lifetime;
- thread safety, thread affinity, and callback reentrancy;
- units, coordinate systems, byte order, and ABI layout;
- failure behavior and the meaning of status codes or empty results;
- non-obvious invariants, security boundaries, and measured performance costs.

Prefer precise names, strong types, RAII, and small interfaces over comments
that restate the implementation. Third-party headers stay under their upstream
documentation and license.

## Pull requests

Each pull request states:

- the problem and the objective;
- the affected files or components;
- API or ABI impact (flat C ABI revision, take format, pack schema);
- the tests performed;
- known risks;
- review instructions when the change needs optional hardware or fixtures.

Changes to `.github/workflows/`, `.github/CODEOWNERS`, or the release tools also
record the information required by
[GOV-2026-001](docs/GOVERNANCE_EXCEPTIONS.md#gov-2026-001-single-maintainer-code-owner-review)
while that exception is active.

## Licenses and dependencies

Project code is MPL-2.0. A new dependency must have terms compatible with the
intended distribution, and `NOTICE.md` must be regenerated with
`pwsh tools/gen_notice.ps1`, never edited by hand. Vendored code keeps its
controlling license and attribution. See
[Legal and distribution boundaries](docs/LEGAL_AND_DISTRIBUTION.md).
