# Contributing to AYTHER Engine

Thank you for contributing to AYTHER Engine. The repository combines Rust and
C++20 and maintains a strict ABI/FFI boundary, so every change must be small,
reproducible, and accompanied by the corresponding test oracle.

## Principles

1. A commit must represent a single coherent change.
2. Do not mix functional changes with refactors unless they are essential.
3. Every bug fix must add or update a test that fails before the fix.
4. Do not add ROMs, binary libretro cores, private keys, or third-party
   copyrighted material without explicit authorization.
5. Do not add dependencies through absolute paths or directories outside the
   repository.
6. A change to shared types must update Rust, C++, the bridge, and the ABI tests
   in the same change.
7. Public documentation must describe only current evidence, clearly labeling
   specified and planned behavior.

## Development environment

Set up and validate the toolchain using the
[development environment guide](docs/DEVELOPMENT_ENVIRONMENT.md). The
reproducible environment is defined through `rust-toolchain.toml`,
`CMakePresets.json`, and `vcpkg.json`. Do not commit `CMakeUserPresets.json` or
machine-specific paths.

Required tools:

- Git;
- CMake 3.21 or later;
- Ninja;
- Rust 1.95.0;
- LLVM/Clang 18;
- vcpkg;
- Vulkan SDK for the renderer;
- PowerShell 7.

## Workflow

1. Create a short-lived branch from `main`.
2. Implement a single functional unit.
3. Format the code.
4. Run the affected tests and the relevant integration tests.
5. Update the documentation and changelog when appropriate.
6. Open a pull request explaining the scope, risks, and validation.

[Conventional Commits](https://www.conventionalcommits.org/) are recommended:

- `feat(core): ...`
- `feat(engine): ...`
- `fix(ffi): ...`
- `test(engine): ...`
- `build(cmake): ...`
- `docs: ...`
- `chore(repo): ...`

## Quality checks

The current minimum is:

- Rust: the four locked commands in
  [Build, test, and release](docs/BUILD_TEST_RELEASE.md).
- C++ boundary: configuration and build with a headless CMake/Ninja preset.
  This currently verifies `Ayther::core`; do not report the engine as built.
- ABI/FFI: `ayther.core.ffi`, including layout and handle-lifecycle checks.
- Install: consume `Ayther::core` from a project outside the source tree when
  changing packaging or public headers.

As the corresponding components become available, the minimum expands to:

- CTest unit and headless session integration tests;
- Renderer: shader compilation and separate GPU tests.
- SDK: installation and use from a project outside the checkout.
- Documentation: valid links and executable examples.

Do not report a test as passing if it was skipped because a core, ROM, or GPU
was unavailable. Skips must remain visible in CI.

## C++ documentation

Write all new first-party C++ comments and API documentation in English. Use
Doxygen-compatible comments for public declarations, but document only
information the declaration cannot express clearly:

- ownership and whether pointers or references are borrowed;
- preconditions, postconditions, invalidation, and lifetime;
- thread safety, thread affinity, and callback reentrancy;
- units, coordinate systems, byte order, and ABI layout;
- failure behavior and the meaning of status or empty results;
- non-obvious invariants, security boundaries, and measured performance costs.

Prefer precise names, strong types, RAII, and small interfaces over comments
that restate implementation steps. Keep third-party headers under their
controlling upstream documentation and license.

## Pull requests

Each pull request must include:

- the problem and objective;
- affected files or components;
- API or ABI impact;
- tests performed;
- known risks;
- review instructions when the change requires optional hardware or fixtures.

## Licenses and dependencies

The project code uses MPL-2.0. Every new dependency must declare terms
compatible with the intended distribution and update `NOTICE.md`. Vendored code
must retain its controlling license and attribution. See
[Legal and distribution boundaries](docs/LEGAL_AND_DISTRIBUTION.md).
