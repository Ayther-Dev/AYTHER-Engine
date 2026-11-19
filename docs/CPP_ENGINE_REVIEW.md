# C++ engineering review

**Status:** open findings only. Resolved findings are removed; their history is
in git and the [CHANGELOG](../CHANGELOG.md).

**Last verified against the code:** 2026-11-19

## Scope and assessment

This review tracks structural risks in the C++ layer (`include/ayther/`,
`src/`) that are not yet fixed. It follows the
[C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines).
Performance items are hypotheses until a benchmark or profiler confirms them.

Already in place and therefore not listed as findings: warnings-as-errors for
Engine targets (`/W4 /WX`, `-Wall -Wextra -Wpedantic -Werror`), a pinned
clang-tidy job, AddressSanitizer and UndefinedBehaviorSanitizer over the CPU
test suite in CI, RAII ownership for Vulkan textures and render targets,
thread-scoped libretro callback dispatch, per-instance renderer scratch
storage, validated `AYTHER_*` runtime options (`runtime_options.h`), and
structured logging through `ayther::log` with a host-installable sink.

The C++ layer is not yet a production engine target. The main remaining risks
are the size of the session implementation, an incomplete GPU and real-core
test matrix, and migration debt in the legacy public headers.

## Open findings

### 1. Decompose `AytherSession::Impl`

**Severity:** high

The session facade is in `src/ayther_session.cpp` (about 11,600 lines) and its
private state and runtime in `src/ayther_session_impl.inl` (about 4,700
lines). Cohesive private components already exist under `src/session/`:
`EmulationObserver` (ABI subscriptions, frame mirrors, legacy observation
fallback), `RecordingController` (live-take state machine, histories,
keyframes), `PackRuntime` (activation, trust registry, profiles, asset lookup,
validation), the native-presentation helpers, the render-observation builder,
and the visual-state codec. `PackRuntime`, `RecordingController`, the builder,
and the codec have unit tests that run without an emulator core, audio device,
or Vulkan context. Emulator observation goes through one `ObservationSource`
for both providers.

The implementation object still owns identity, scripts, audio, video, rewind,
export, compatibility, and authoring state, and the native-provider work grew
the facade further.

**Correction:** keep `AytherSession` as the facade and extract
`FrameIdentityPipeline`, `AudioRuntime`, and `VideoRuntime`, which do not exist
yet. Pass narrow typed inputs between components. Each component should have an
independently testable state machine and one error policy.

### 2. Complete the GPU, audio-device, and real-core test matrix

**Severity:** high

CPU tests run under sanitizers, but GPU tests, audio-device paths, and real-core
integration are not exercised on every supported platform. Buildability and
CPU tests must not be presented as proof of the runtime contract.

**Correction:** separate GPU and device tests into labelled suites that run on
hardware-backed runners, and add a real-core integration matrix per platform.

### 3. Remove mutable function-static scratch storage outside the renderer

**Severity:** medium

The renderer now owns per-instance scratch buffers, but mutable
`static std::vector` instances remain in `src/audio_player.cpp` (priming
zeros) and `src/ayther_session_impl.inl` (synthesis scratch). They share state
across instances and threads, prevent reentrancy, and hide lifetime costs.

**Correction:** move them into per-instance scratch members, reserve from
observed high-water marks, and add an architecture check that rejects mutable
function-static containers in first-party engine code.

### 4. Centralize audio format and timing constants

**Severity:** medium

The 44.1 kHz, stereo, signed-16 format appears as literals in device
specifications, conversion calls, duration and offset calculations, mixer
comments, and WAV header bytes. The native provider adds a second rate (48 kHz)
that is resampled to the player's rate, which makes the duplication riskier.

**Correction:** define a strongly typed `AudioFormat` with named constants for
sample rate, channel count, bytes per sample, and bytes per frame. Generate WAV
headers from those values. Use unit-bearing helpers for conversions between
seconds, emulation frames, sample frames, scalar samples, and bytes.

### 5. Harden platform path discovery

**Severity:** medium

`src/ayther_config.cpp` derives configuration paths from `USERPROFILE`,
`APPDATA`, `HOME`, and `XDG_CONFIG_HOME`, and falls back to the current
directory (`"."`) when they are absent.

**Correction:** isolate path discovery behind a platform service, use
known-folder APIs on Windows, validate that returned paths are absolute and
writable, and return an explicit error instead of silently selecting `.` for
persistent configuration.

### 6. Translate the legacy public headers and implementation commentary

**Severity:** medium

`include/ayther/ayther_session.h` and `include/ayther/ayther_core_ffi.h` still
contain Spanish comment lines, as does a large share of first-party
implementation commentary under `src/`. The documentation-language CTest
(`ayther.quality.documentation_language`) guards only the headers listed in
`_ayther_english_documented_headers` in `tests/CMakeLists.txt`; these two
legacy headers are deliberately excluded until their contracts are translated
and reviewed in cohesive sections. This is visible migration debt, not an
assertion that they meet the policy.

**Correction:** translate component by component, preserving rationale,
measurements, units, and safety warnings and removing chronology and task
references. Add each translated header to the CTest list.

### 7. Remove presentation strings and fixed capacities from engine types

**Severity:** medium

`AytherLayerStack` in `include/ayther/ayther_layers.h` constructs built-in
layers with Spanish display labels such as `"Plano B"`, `"Panorámica"`, and
`"Primer plano"`, and `AytherLayer::name` is a fixed `char[24]`. Other public
layer structs encode path, name, animation, and screen limits as fixed C
arrays. The labels couple localization to domain state; the capacities are
undocumented protocol limits that can truncate authoring data or force
ABI-breaking changes.

**Correction:** keep language-neutral layer identifiers in the engine and
resolve localized labels in the frontend. Name every unavoidable wire limit and
validate it at the boundary. Prefer `std::span`, `std::string_view`, or owning
containers in the C++ API; keep fixed arrays only where the C ABI or serialized
format requires them.

### 8. Make shader resources an explicit runtime service

**Severity:** medium

The package installs the shader resources, but the renderer still builds
filenames such as `sprite.vert.spv` from a caller-provided directory
(`src/ayther_renderer.cpp`). Renaming, embedding, or versioning shaders needs
coordinated string edits, and failures surface late during Vulkan
initialization.

**Correction:** introduce a shader repository or manifest that maps a typed
shader role to validated bytes, validate the complete set at startup, allow an
embedded implementation, and keep a filesystem implementation for development
overrides.

## Design patterns to adopt

| Pattern | Application |
|---|---|
| RAII resource owner | Descriptor pools, command resources, dynamic libraries, decoder contexts |
| Facade plus cohesive services | Keep `AytherSession` small while isolating audio, video, identity, and authoring logic |
| State machine | Session lifecycle, video decode, rewind, asynchronous texture jobs |
| Strategy | Output scaling, audio routing, replacement matching, fallback policy |
| Command queue | Render-thread uploads and device-resource destruction requested by workers |
| Observer with scoped subscription | Frame and audio extension callbacks returning a move-only subscription token |
| Strong value types | Frame index, sample frame, byte count, pixel extent, channel, generation, asset identifier |
| Result/expected error flow | File parsing, decoder setup, dynamic loading, resource creation, configuration discovery |
| Per-instance scratch arena | Frame-local vectors and temporary batches without mutable global state |
| Resource repository | Typed lookup and startup validation for shaders and pack assets |

Avoid patterns that only add indirection. Each abstraction should remove a
concrete ownership ambiguity, illegal state, duplicated policy, or measured
cost.

## Performance opportunities requiring measurement

1. **Texture decode and upload copies.** The asynchronous path reads compressed
   bytes, decodes to a library buffer, channel-swaps or flips in place, and
   copies into a completion vector. Measure whether a pooled destination buffer
   or direct decode target removes a significant copy.
2. **String-keyed asset caches.** Repeated hashing and allocation of full paths
   may matter in large scenes. Measure heterogeneous lookup with
   `string_view`, normalized asset identifiers, or interned keys.
3. **Per-call vectors in inline audio algorithms.** Sequence anchoring and bus
   analysis allocate several vectors. If they run every frame, provide
   caller-owned scratch storage after profiling confirms pressure.
4. **Linear voice and cache scans.** Vectors are likely optimal for small counts.
   Establish realistic voice and asset counts before replacing them with maps.
5. **Decoder parallelism.** Optimal concurrency depends on resolution, CPU
   topology, other engine work, and memory bandwidth. Benchmark representative
   content and expose a validated policy rather than a universal constant.

## Recommended enforcement

- compile every first-party header as the first include in a minimal
  translation unit;
- add Doxygen warning checks for undocumented members of installed headers;
- add architecture tests that reject raw owning pointers and mutable
  function-static containers in first-party engine code;
- benchmark before and after every performance change and keep the workload,
  hardware, compiler, and variance with the result.
