# Glossary

**Last reviewed:** 2026-11-19

| Term | Meaning |
|---|---|
| AYTHER Engine | Open-source emulation-observation and real-time HD asset substitution engine documented by this repository |
| AYTHER Runtime | Separate product that hosts game sessions on top of Engine; the main consumer of its release candidates |
| AYTHER Tessera | AYTHER's own clean-room emulator core, developed in its own repository and consumed by Engine as an optional native provider |
| `ayther_core` | Rust library for identity, substitution, packs, scripting, patches, and audio-format processing |
| `Ayther::core` | Installed CMake target for the static Rust core and its C header |
| `Ayther::engine` | Installed CMake target for the C++ engine |
| `AytherSession` | C++ facade that owns one emulation and enhancement session |
| `AytherRenderer` | Vulkan renderer that consumes a `FrameView` and renders to an offscreen image |
| `FrameView` | Plain-data, CPU-visible result of one session frame |
| emulation provider | Source of frames, memory, and sound for a session: a libretro core or the Tessera native provider (`AytherSession::Config::provider`) |
| libretro core | Native emulator module loaded at run time, supplied under its own terms and treated as untrusted code |
| AYTHER-aware core | Libretro core that also exports the emulator extension ABI |
| stock core | Libretro core without the emulator extension ABI; gives reduced observation |
| emulator extension ABI | Independently versioned protocol (`ayther_interface_v1`) exposed by an AYTHER-aware libretro core; current version 1.10 |
| native provider | The Tessera provider, `EmulationProvider::tessera_native`; built with `AYTHER_ENABLE_TESSERA=ON` |
| `ObservationSource` | Internal Engine interface through which the session reads any emulator; implemented by `RetroRunner` (libretro) and `TesseraBackend` (Tessera) |
| `RetroRunner` | Internal libretro host that loads a core and implements `ObservationSource` |
| `TesseraBackend` | Internal adapter that drives a Tessera native session and implements `ObservationSource` |
| Tessera C API | Versioned C interface of the installed `Tessera::core` package; Engine accepts 1.1 or a later 1.x |
| Tessera lock | `dependencies/tessera.lock.json`: the exact Tessera commit, API, version, release tag, and package SHA-256 that Engine accepts |
| native candidate | Windows x64 Engine package with the Tessera provider, tagged `native-v0.1.0-rc.N` |
| typed driver | `engine::NativeEmulation`, the direct native-session interface returned by `native_emulation()` |
| `NativeResult` | Result of a native operation: status code, phase, frame and epoch, and transaction |
| parity bench | `tools/parity_bench`, which compares the libretro and Tessera providers on frames, identities, and cross-provider packs |
| cross-pack | Parity check that bakes a pack from one provider's identities and plays it on the other |
| exception accounting | `parity_bench check` rule: every divergence must be declared, and every declared divergence must still occur |
| Genesis Plus GX (gpgx) | Libretro Mega Drive core whose AYTHER-aware fork is the reference for observation equivalence |
| emulation generation | Identifier of the emulator that produced a take (for example `tessera` plus the backend build ID) |
| `.arp` take | Recorded input stream with keyframes, replayable by the session |
| component | Logical animation, audio event, background, or anchored entity built from observed identities and context |
| `.ay` pack | ZIP-based container for metadata and optional replacement content; never a ROM container |
| asset identity | Deterministic key derived from observed graphics, audio, or contextual state |
| substitution | Resolution of an identity to optional replacement content |
| manifest schema | Versioned structure of `manifest.toml`; highest implemented value is `2` |
| integrity index | `integrity.toml` digest metadata covered by a pack signature |
| trust registry | Explicit set of public keys, scopes, validity windows, and revocations a host accepts for packs |
| compatibility grade | Exact, warning-bearing, experimental, or incompatible result for a pack/context pair |
| CXX bridge | Typed Rust/C++ declarations generated through the `cxx` ecosystem |
| flat C ABI | Pointer-oriented `extern "C"` API declared by `ayther_core_ffi.h`, versioned by its own revision number |
| FFI | Foreign-function interface between Rust and C or C++ |
| BYOR | Bring Your Own ROM; the project does not supply game images |
| BYOC | Bring Your Own Core; the project does not distribute third-party emulator cores |
| ROM patch | IPS or BPS transformation applied to an in-memory copy of user-supplied data |
| headless | Build or test path that does not require rendering or GPU execution |
| skip code 77 | CTest return code a test uses when an optional external input is missing; a skip is not a pass |
| release blocker | Condition that must be resolved before a stable or supported release |
