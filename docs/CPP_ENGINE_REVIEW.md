# C++ engineering review

**Status:** critical review; build/package findings resolved, runtime findings open

**Assessment date:** 2026-08-28

## Executive assessment

The C++ layer contains substantial domain logic and several sound design
choices: explicit facade boundaries, widespread use of `unique_ptr`, typed
enums, PImpl for major facades, idempotent cleanup in several subsystems, and
capacity reuse in measured hot paths. It is not ready to be treated as a
production engine target. Configure/build/install/consume verification now
exists; the largest remaining risks are manual GPU ownership, process-visible
callback dispatch, an oversized session implementation, mutable function-static
render scratch state, and missing behavior/sanitizer oracles.

The recommendations below follow the C++ Core Guidelines. Performance items are
hypotheses until a benchmark or profiler confirms them.

Reference: [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines).

## Priority findings

### 1. Complete native target established; behavior claims still require tests

**Severity:** resolved for build integration; critical test gap remains

The root build now compiles all 24 engine sources, resolves every declared
native dependency, exports `Ayther::engine`, and passes Windows package-consumer
link checks with VPX disabled and enabled. Sanitizers, exhaustive symbol use,
platform branches, and ownership paths are not continuously checked.

**Remaining correction:** enable high warning levels, warnings-as-errors in CI,
clang-tidy, AddressSanitizer/UndefinedBehaviorSanitizer where supported, and
separate CPU and GPU tests. Buildability must not be presented as proof of the
runtime contract.

### 2. Replace raw owning GPU pointers

**Severity:** high

`VkSprite` stores and mutates owning `VkTexture*` values and performs several
manual `new`, `shutdown`, `delete`, and null-reset sequences. Error branches and
cache eviction must reproduce the exact same order.

**Evidence:** `include/ayther/vulkan_backend/vk_sprite.h` (`TexEntry`) and
`src/vulkan_backend/vk_sprite.cpp` around texture creation, upload failure,
video texture replacement, eviction, and shutdown.

**Correction:** introduce a move-only texture owner that binds the Vulkan
cleanup operation to object lifetime. If destruction requires `VkContext`, use
an owner carrying a non-owning context reference or a custom deleter whose
lifetime is constrained by the renderer. Store that owner directly in cache
entries. This applies RAII and removes duplicated cleanup paths.

### 3. Make Vulkan destruction structurally unavoidable

**Severity:** high

Several Vulkan wrappers have default destructors and require callers to invoke
`shutdown(context)` explicitly. A missed call leaks resources; a late call may
use a dead context. The type system does not encode the required order.

**Evidence:** `AytherRenderer`, `VkTexture`, `VkIndexedPlane`, and
`VkRenderTarget` explicitly state that shutdown must be called.

**Correction:** group device-dependent resources under one move-only owner that
cannot outlive `VkContext`, or store the device/allocator handles required for
destruction inside each wrapper. Keep `shutdown()` idempotent for early release,
but make the destructor safe and authoritative.

### 4. Eliminate process-visible callback dispatch state

**Severity:** high

`RetroRunner` routes C callbacks through a static `s_instance_` pointer and
reassigns it before operations. This is non-reentrant and creates a data race if
two runners are driven concurrently. Reasserting the pointer narrows the window
but does not create an ownership or synchronization guarantee.

**Correction:** prefer a callback API with an explicit user-data pointer. Where
libretro prevents that, isolate each core in a dedicated process or a dedicated
loaded-module dispatch object with a formally enforced single-owner execution
contract. A `thread_local` pointer can improve thread isolation but does not by
itself solve callbacks that escape the initiating call.

### 5. Decompose `AytherSession::Impl`

**Severity:** high

`src/ayther_session.cpp` exceeds eleven thousand lines and its implementation
object owns emulator observation, identity, packs, scripts, audio, video,
recording, rewind, export, compatibility, and authoring state. This makes
invariants difficult to review and encourages temporal coupling.

**Correction:** retain `AytherSession` as the facade, but delegate to cohesive
owners such as `EmulationObserver`, `PackRuntime`, `FrameIdentityPipeline`,
`AudioRuntime`, `VideoRuntime`, and `RecordingController`. Pass narrow typed
inputs between them. Each component should have an independently testable state
machine and one error policy.

### 6. Move render scratch buffers out of function-static storage

**Severity:** high

`ayther_renderer.cpp` uses multiple mutable `static std::vector` instances to
retain capacity across frames. This avoids allocations but shares mutable state
across renderer instances and threads, preventing reentrancy and making lifetime
costs invisible.

**Correction:** add a `FrameScratch` member owned by each renderer, reserve from
observed high-water marks, and clear between frames. This preserves capacity
reuse without global writable state and allows memory to be reclaimed or
budgeted deliberately.

### 7. Centralize audio format and timing constants

**Severity:** medium

The 44.1 kHz, stereo, signed-16 format appears in device specifications,
conversion calls, duration calculations, offset calculations, mixer comments,
and literal WAV header bytes. These values form one contract but are encoded in
several representations.

**Correction:** define a strongly typed `AudioFormat` and named constants for
sample rate, channel count, bytes per sample, and bytes per frame. Generate WAV
headers from those values. Use unit-bearing helpers for conversions between
seconds, emulation frames, sample frames, scalar samples, and bytes.

### 8. Replace unchecked environment parsing and scattered switches

**Severity:** medium

Environment variables control diagnostic and performance behavior in several
translation units. Video thread count uses `std::atoi`, which cannot distinguish
invalid input from zero and does not report overflow.

**Correction:** parse once into a validated immutable runtime-options object,
using `std::from_chars`. Inject that object into the relevant subsystem. Keep
environment variables as a thin startup adapter, not as hidden dependencies in
hot or library code.

### 9. Replace ad-hoc diagnostic output with structured logging

**Severity:** medium

Many components call `fprintf(stderr, ...)` directly, some from performance-
sensitive paths and with mixed user-facing languages. Callers cannot route,
classify, suppress, or test these diagnostics consistently.

**Correction:** define a lightweight logging interface with severity, component,
stable event identifier, and structured fields. Inject or register a sink at the
engine boundary. Keep the fallback sink allocation-free and rate-limit repeated
events.

### 10. Harden platform path discovery

**Severity:** medium

Configuration paths are derived directly from environment variables and fall
back to the current directory. User-visible directory names and compatibility
probes are embedded in implementation code.

**Correction:** isolate path discovery behind a platform service, use native
known-folder APIs on Windows, validate that returned paths are absolute and
writable, and return an explicit error instead of silently selecting `.` for
persistent configuration.

### 11. Normalize remaining implementation commentary

**Severity:** medium

The newly defined public contracts are in English, but a substantial amount of
first-party implementation commentary and diagnostic text still uses mixed
language and inconsistent terminology. That increases review cost and makes
generated source documentation uneven.

**Correction:** translate implementation comments component by component as
each subsystem becomes buildable. Preserve technical rationale, measurements,
units, and safety warnings; remove obsolete chronology and task references.
Standardize user-visible diagnostics through the structured logging work rather
than translating isolated string literals in place.

The installed `ayther_session.h` and `ayther_core_ffi.h` still contain hundreds
of Spanish comment lines. They are explicitly excluded from the new English-
documentation CTest until their contracts can be translated and reviewed in
cohesive sections. This is visible migration debt, not an assertion that those
headers meet the policy.

### 12. Remove presentation strings and fixed capacities from engine types

**Severity:** medium

`AytherLayerStack` constructs built-in layers with Spanish display labels such
as `"Plano B"`, `"Panorámica"`, and `"Primer plano"`. The public layer structs
also encode path, name, animation, and screen limits as fixed C arrays (`256`,
`24`, `3`, and `8` elements). The labels couple localization to domain state;
the capacities are undocumented protocol limits that can truncate authoring
data or force ABI-breaking changes.

**Correction:** keep stable language-neutral layer identifiers in the engine
and resolve localized labels in the frontend. Name every unavoidable wire limit
and validate it at the boundary. For the C++ API, prefer `std::span`,
`std::string_view`, or owning containers as appropriate; preserve fixed arrays
only where the C ABI or serialized format requires their exact layout.

### 13. Make shader resources an explicit runtime service

**Severity:** medium

The CMake target and package now declare and install the shader resources, but
the renderer still constructs filenames such as `sprite.vert.spv` and
`video.frag.spv` from a caller-provided directory. Renaming, embedding, or
versioning shaders therefore requires coordinated string edits and failures are
reported late during Vulkan initialization.

**Correction:** introduce a `ShaderRepository` or resource manifest that maps a
typed shader role to validated bytes. Validate the complete required set at
startup, permit an embedded-resource implementation, and retain a filesystem
implementation for development overrides.

## Design patterns to adopt

| Pattern | Application |
|---|---|
| RAII resource owner | Vulkan images, descriptor pools, command resources, dynamic libraries, decoder contexts |
| Facade plus cohesive services | Keep `AytherSession` small while isolating audio, video, pack, recording, and observation logic |
| State machine | Session lifecycle, video decode, recording, rewind, pack activation, asynchronous texture jobs |
| Strategy | Output scaling, audio routing, replacement matching, fallback policy |
| Command queue | Render-thread uploads and device-resource destruction requested by workers |
| Observer with scoped subscription | Frame/audio extension callbacks; return a move-only subscription token |
| Strong value types | Frame index, sample frame, byte count, pixel extent, channel, generation, asset identifier |
| Result/expected error flow | File parsing, decoder setup, dynamic loading, resource creation, configuration discovery |
| Per-instance scratch arena | Frame-local vectors and temporary render batches without mutable global state |
| Resource repository | Typed lookup and startup validation for shaders and pack assets |
| Configuration adapter | Parse environment variables once, then inject immutable validated options |

Avoid applying patterns merely to add indirection. Each abstraction should
remove a concrete ownership ambiguity, illegal state, duplicated policy, or
measured cost.

## Performance opportunities requiring measurement

1. **Texture decode and upload copies.** The asynchronous path reads compressed
   bytes, decodes to a library buffer, channel-swaps or flips in place, and then
   copies into a completion vector. Measure whether a pooled destination buffer
   or direct decode target removes a significant copy without complicating
   ownership.
2. **String-keyed asset caches.** Repeated hashing and allocation of full paths
   may be material in large scenes. Measure heterogeneous lookup with
   `string_view`, normalized asset identifiers, or interned keys.
3. **Per-call vectors in inline audio algorithms.** Sequence anchoring and bus
   analysis allocate several vectors. If these run every frame, provide caller-
   owned scratch storage or reuse capacity after profiling confirms pressure.
4. **Linear voice and cache scans.** Current vectors are likely optimal for
   small counts because of locality. Establish realistic voice/asset count
   distributions before replacing them with maps.
5. **Decoder parallelism.** A fixed maximum thread count is a reasonable guard,
   but optimal concurrency depends on resolution, CPU topology, other engine
   work, and memory bandwidth. Benchmark representative content and expose a
   validated policy rather than a universal constant.
6. **Session working set.** Splitting the implementation allows each subsystem
   to own compact contiguous frame data and makes cache behavior measurable.

## Documentation corrections applied

- established an English contract reference for ownership, lifetime, threading,
  errors, and component boundaries;
- documented `AytherSession`, `AudioPlayer`, renderer, Vulkan context, dynamic
  loader, and libretro runner at their public declarations;
- defined English documentation rules for future C++ changes;
- documented SDK ownership, borrowed lifetimes, lazy pack verification,
  capability reporting, and version compatibility in English;
- documented layer gating, blend/tint/animation semantics, mutation results,
  and borrowed-pointer invalidation in English;
- added `[[nodiscard]]`, `noexcept`, explicit casts, and a typed sentinel where
  the declarations can mechanically express their contracts;
- added a CTest language guard for the nine public headers completed in this
  migration, while keeping the two legacy-header exclusions explicit;
- excluded the third-party libretro protocol header from project-specific
  rewriting;
- kept early-state, compatibility, security, and legal limitations explicit.

## Recommended enforcement

- run clang-format and clang-tidy with a checked-in configuration;
- enable warnings appropriate to Clang/MSVC and GCC/Clang builds;
- compile every first-party header as the first include in a minimal translation
  unit;
- add Doxygen warning checks for undocumented members in the public allowlist;
- run ASan/UBSan on CPU tests and validation layers plus dedicated GPU tests;
- add architecture tests that reject raw owning pointers and mutable function-
  static containers in first-party engine code;
- benchmark before and after every performance change and retain the workload,
  hardware, compiler, and variance with the result.

## Structural graph observations

The scoped first-party public-header graph contains 147 extracted nodes in 26
communities. No surprising cross-file shortcut was detected by the structural
pass. The lowest-cohesion communities were Vulkan Rendering (0.1111) and
Session and Layers (0.142857), and 43 nodes were weakly connected or isolated.
These numbers are navigation evidence rather than proof of a defect, but they
reinforce the manual findings that session responsibilities and rendering
ownership need narrower interfaces. The generated artifacts are stored under
`build/cpp-review-graph/graphify-out/`.
