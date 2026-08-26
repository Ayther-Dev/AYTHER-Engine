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

## Development environment

- CMake 3.21 or later.
- Ninja.
- Rust 1.95.0, pinned by `rust-toolchain.toml`.
- LLVM/Clang 18; `clang-cl` on Windows.
- vcpkg with a versioned baseline.
- Vulkan SDK for the renderer and GPU tests.
- PowerShell 7 for development and packaging scripts.

Follow the [development environment setup guide](docs/DEVELOPMENT_ENVIRONMENT.md)
for platform-specific installation, vcpkg setup, toolchain verification, and
CMake preset usage. Do not add absolute paths or dependencies on a checkout
outside the repository.

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
