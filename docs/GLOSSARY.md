# Glossary

**Last reviewed:** 2026-08-27

| Term | Meaning |
|---|---|
| AYTHER Engine | Open-source core and native orchestration technology documented by this repository |
| `ayther_core` | Rust library for identity, substitution, packs, scripting, patches, and audio-format processing |
| `Ayther::core` | Partial installed CMake target for the static Rust core and C header |
| `AytherSession` | Planned C++ facade that owns one emulation and enhancement session |
| `.ay` pack | ZIP-based container for metadata and optional replacement content; never a ROM container |
| asset identity | Deterministic key derived from observed graphics, audio, or contextual state |
| substitution | Resolution of an identity to optional replacement content |
| manifest schema | Versioned structure of `manifest.toml`; highest implemented value is currently `2` |
| integrity index | `integrity.toml` digest metadata covered by a pack signature |
| compatibility grade | Exact, warning-bearing, experimental, or incompatible result for a pack/context pair |
| CXX bridge | Typed Rust/C++ declarations generated through the `cxx` ecosystem |
| flat C ABI | Pointer-oriented `extern "C"` API declared by `ayther_core_ffi.h` |
| FFI | Foreign-function interface between Rust and C or C++ |
| BYOR | Bring Your Own ROM; the project does not supply game images |
| BYOC | Bring Your Own Core; the project does not assume a right to distribute emulator cores |
| libretro core | Native emulator module supplied under its own terms and treated as untrusted code |
| ROM patch | IPS or BPS transformation applied to an in-memory copy of user-supplied data |
| headless | Build or test path that does not require rendering or GPU execution |
| trust store | Set of keys and policies accepted for a purpose; no production pack trust store exists yet |
| release blocker | Condition that must be resolved before a stable or supported release |
