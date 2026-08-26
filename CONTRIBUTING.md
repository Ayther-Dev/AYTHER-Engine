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
5. Do not add dependencies through absolute paths or sibling checkouts.
6. A change to shared types must update Rust, C++, the bridge, and the ABI tests
   in the same commit.

## Development environment

The reproducible environment will be defined through `rust-toolchain.toml`,
`CMakePresets.json`, and `vcpkg.json`. Do not commit `CMakeUserPresets.json` or
machine-specific paths.

Planned requirements:

- Git;
- CMake 3.21 or later;
- Ninja;
- stable Rust;
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

As components are added, the expected minimum will be:

- Rust: `cargo fmt --check`, `cargo clippy`, and `cargo test`.
- C++: configuration and build with CMake/Ninja.
- ABI/FFI: tests for sizes, offsets, and handle lifecycles.
- CTest: unit tests and headless integration tests.
- Renderer: shader compilation and separate GPU tests.
- SDK: installation and use from a project outside the checkout.
- Documentation: valid links and executable examples.

Do not report a test as passing if it was skipped because a core, ROM, or GPU
was unavailable. Skips must remain visible in CI.

## Pull requests

Each pull request must include:

- the problem and objective;
- affected files or components;
- API or ABI impact;
- tests performed;
- known risks;
- review instructions when the change requires optional hardware or fixtures.

## Licenses and dependencies

The project code uses MPL-2.0. Every new dependency must declare a compatible
license and update `NOTICE.md` when appropriate. Vendored code must retain its
original license and attribution.
