# AYTHER Engine

Real-time audiovisual remastering engine for retro 2D games.

AYTHER Engine separates the logic of the emulated game from its visual and
audio presentation. The repository contains the Rust core (`ayther_core`), the
C++20 engine (`ayther_engine`), and the infrastructure required to build, test,
and install both components.

## Status

The repository is in its initial development stage. Development proceeds
through small, verifiable changes. A stable version will not be considered
released until the first build, test, and installation cycle is complete.

## Planned components

| Component | Technology | Responsibility |
|---|---|---|
| `core/` | Rust | Identity, packs, substitutions, scripting, and FFI |
| `engine/` | C++20 | Session, libretro host, audio, rendering, and public facade |
| `tests/` | Rust/C++ | Unit, ABI/FFI, integration, and conformance tests |
| `sdk/` | C/C++/Rust | Examples, fixtures, and public API reference |

## Planned environment

- CMake 3.21 or later.
- Ninja.
- Stable Rust, pinned by `rust-toolchain.toml` once the core is added.
- LLVM/Clang 18; `clang-cl` on Windows.
- vcpkg with a versioned baseline.
- Vulkan SDK for the renderer.
- PowerShell 7 for development and packaging scripts.

Reproducible setup, build, and installation instructions will be added
alongside each component. Do not add absolute paths or dependencies on a
checkout outside the repository.

## Development

Before contributing, see [CONTRIBUTING.md](CONTRIBUTING.md). Each commit must
have a defined scope, describe the modified files, and record the validation
performed.

## Security

Security issues must be reported privately according to
[SECURITY.md](SECURITY.md). Do not disclose vulnerabilities through issues.

## License

AYTHER Engine is distributed under the Mozilla Public License 2.0. See
[LICENSE](LICENSE). Third-party attributions and licenses will be maintained in
`NOTICE.md` as dependencies are added.
