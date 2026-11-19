# Engine public API contract

**Status:** implemented for the installed C++ Engine surface: typed input,
session (libretro and native Tessera providers), renderer, layers, pack
inspection, libretro core probing, build capabilities, audio and render
observation, visual state, and the Vulkan render-image handoff. Pre-release;
see [API and compatibility](API_COMPATIBILITY.md) for version windows.

The ownership decision, authorized consumers, and `0.1.x` guarantees are
recorded in [ADR 0003](adr/0003-runtime-engine-public-api-ownership.md).

## Public boundary

The Runtime-facing C++ entry points live under `include/ayther/engine/` and are
included with installed paths:

```cpp
#include <ayther/engine/engine.hpp>            // umbrella
#include <ayther/engine/capabilities.hpp>
#include <ayther/engine/core_probe.hpp>
#include <ayther/engine/input.hpp>
#include <ayther/engine/native_emulation.hpp>
#include <ayther/engine/pack.hpp>
#include <ayther/engine/vulkan_interop.hpp>
```

`engine.hpp` publishes the typed Engine modules and the installed C++ session,
renderer, and layer facades; a consumer that wants the complete C++ API needs no
other AYTHER header. The installed list is `cmake/AytherPublicHeaders.txt`,
rendered in the [Public API index](PUBLIC_API_INDEX.md).

The flat `include/ayther/` C++ facades remain installed and reachable through
the umbrella during `0.1.x`. New focused APIs belong under `ayther/engine/`. The
flat C ABI is available separately through `ayther_core_ffi.h`.

Public headers are self-contained and compile as the first include in an
otherwise empty C++20 translation unit. They may include the C++ standard
library, documented third-party API headers (Vulkan, for `vulkan_interop.hpp`),
or other installed AYTHER headers. They never include Tessera or libretro
headers, and never include paths containing `src/`, `libretro_host/`,
`vulkan_backend/`, `private/`, `internal/`, or `detail/`
(`tools/check_public_headers.cmake` enforces this). `native_emulation.hpp`
forward-declares the Engine-private `detail::TesseraBackend` class; consumers
cannot construct or use it.

## Emulation providers

`AytherSession::Config::provider` selects the emulator explicitly:

| Provider | Emulator | Configuration | Availability |
|---|---|---|---|
| `EmulationProvider::libretro` (default) | a user-supplied libretro core | `core_path`, `rom_path`, `core_options`, `patch_path` | every build |
| `EmulationProvider::tessera_native` | AYTHER Tessera, Mega Drive machine | `native_config` (`engine::NativeConfig`: backend id, content bytes, machine configuration, persistent memory) | builds with `AYTHER_ENABLE_TESSERA=ON` (off by default; platforms in [Tessera native provider](TESSERA_NATIVE.md)) |

- There is no fallback between providers. A native failure is returned as an
  error; the session never retries with libretro.
- With `tessera_native`, `patch_path`, `core_options`, `rom_path`,
  `audio_observer`, and `render_observer` are rejected at creation with
  `unsupported_capability`.
- In a build without Tessera, every `NativeEmulation` operation returns
  `NativeStatus::unsupported_capability`, so creating a native session fails
  the same way. An installed package records the choice in the CMake variable
  `Ayther_HAS_TESSERA`; `probe_capabilities()` does not report it.
- `set_input()` and `step()` drive both providers. The native provider also
  exposes a typed direct driver through `native_emulation()`, which returns null
  for the libretro provider.

Native configuration formats, the version lock, and what works on the native
path are documented in [Tessera native provider](TESSERA_NATIVE.md).

## Language, exceptions, and RTTI

- C++20 is the minimum language version.
- Functions marked `noexcept` do not throw and do not terminate the process on
  an environmental failure.
- Expected failures in fallible APIs use a typed result and a stable error
  code. Exceptions never cross the Runtime-to-Engine boundary.
- Public behavior does not depend on RTTI. Concrete implementation types are
  not exposed for `dynamic_cast` or `typeid`.

## Ownership and lifetime

- Value types are owned by the caller after return.
- Owning objects are move-only RAII handles (`AytherSession` through
  `std::unique_ptr`, `CoreProbe`, `PackWatcher`, `NativeEmulation`,
  `NativeSnapshot`). Destructors release Engine-owned resources; copying
  ownership is forbidden.
- Borrowed references, spans, string views, and native handles name their owner
  and validity interval in the declaring header. A borrowed value never extends
  the owner's lifetime.
- `FrameView` is valid until the next call that mutates session state. For the
  native provider, `NativeFrame` pixel and PCM spans expire at the next
  successful step, reset, restore, or close; queries and failed operations do
  not invalidate them. A `NativeSnapshot` holds its own reference and stays
  readable after the session closes.
- Vulkan handles remain owned by the party named by `vulkan_interop.hpp`.
  Exporting or borrowing a handle never transfers ownership implicitly.

## Thread safety and synchronization

- `version()`, `core_abi_revision()`, and `probe_capabilities()` are safe for
  concurrent calls.
- Mutable Engine objects are single-owner and single-threaded unless their
  declaring header says otherwise. This applies to `AytherSession` with either
  provider and to `NativeEmulation`, whose calls the owner serializes.
- Retained `NativeSnapshot` reads and reference operations are thread-safe. A
  caller retains a second reference before handing a snapshot to another thread.
- Audio observer callbacks may be called concurrently by different producers;
  see [Audio QA observation](#audio-qa-observation).
- A Vulkan interop contract states the queue, command-buffer, semaphore,
  image-layout, and destruction responsibilities for every borrowed or
  transferred handle. Runtime may not infer synchronization from a raw handle.

## Errors

`version()` and `probe_capabilities()` are total, side-effect-free `noexcept`
queries. They report the linked artifact, not device availability, so they have
no environmental failure path.

APIs that inspect a loader, GPU, display, audio device, filesystem,
configuration source, or emulator are fallible:

- Session, pack, and core-probe operations return `ayther::Result<T>` with an
  `ErrorCode` and an owned diagnostic.
- Native-provider operations return `engine::NativeResult`. Its `code` is an
  `engine::NativeStatus`; it also carries the failing phase, OS error domain and
  code, frame identity, transaction, and cancellation reason. Output objects are
  preserved on failure except where the header documents otherwise.
  `buffer_too_small` writes no payload; `end_of_trace` is explicit and does not
  advance.
- `AytherSession::create(cfg, native_result)` returns both the `Result` and the
  exact `NativeResult` of native preparation. A failed native advance inside
  `step()` is reported in `FrameView::native_result`. Each later native query
  has its own result; keep the one that matters.

Diagnostics and logging are not substitutes for a returned error.

## Version and capability identity

`ayther::engine::version()` returns the numeric release compiled into the
linked Engine library (`0.1.0` for this line). It is implemented out of line, so
consumer compile definitions cannot change it. Prerelease tags
(`v0.1.0-rc.N`) are distribution identity, kept in archive names, provenance,
and release metadata, not a field of `Version`. Logs and UI format the linked
value through the API instead of embedding a version literal.

`ayther::engine::core_abi_revision()` forwards the linked Rust core's non-zero
flat C ABI revision. It is independent from SemVer. Tests compare it with
`AYTHER_CORE_C_ABI_REVISION` and compare the numeric version with the release
macros and CMake project version.

`probe_capabilities()` returns build capabilities only. It reads no registry,
singleton, environment variable, configuration file, filesystem path, or device
state, and it does not load Vulkan, create a window, open an audio device, or
start a thread. It is callable before any Engine instance and behaves the same
on a machine without a GPU or display.

| Field | Meaning |
|---|---|
| `renderer` | Renderer compiled into Engine (`vulkan` or `none`) |
| `hardware_acceleration` | The compiled renderer uses hardware acceleration when initialized; not a device-presence check |
| `external_image_import` | A public external-image import contract is compiled |
| `libretro_video` | The artifact implements the libretro video callback path |
| `libretro_audio` | The artifact implements the libretro audio callback paths |

Vulkan device suitability belongs to renderer initialization. Native-provider
availability and the backends it offers are reported by
`NativeEmulation::backends()`, not by this structure.

## Libretro core probing

`probe_core(path)` loads one user-selected library without initializing SDL,
loading a ROM, or calling `retro_init`. Success returns a move-only `CoreProbe`;
its destructor unloads the library. `CoreProbe::info()` returns a `CoreInfo`
whose name, version, extension list, API version, and path/extraction flags
were copied while the library was loaded. No `retro_system_info` pointer or
platform handle is public.

The factory returns `Result<CoreProbe>`. `ErrorCode::Io` means the platform
loader rejected the file; `ErrorCode::BadFormat` means the library loaded but
lacks `retro_api_version` or `retro_get_system_info`. The diagnostic names the
platform error or missing symbols. A failed call releases any handle it
acquired.

`CoreInfo::serialize()` and `CoreProbe::serialize()` return the same compact
JSON object, with core-supplied strings escaped. They do not frame a protocol
line.

`probe_core_v2(path)` implements the [Core probe contract](CORE_PROBE_CONTRACT.md):
it returns a `ProbeReport` by value and never throws. It checks the file and its
PE or ELF architecture, loads the library, and, for a core that exports
`ayther_get_interface`, calls `retro_set_environment`, `retro_init`,
`ayther_get_interface`, and `retro_deinit` without loading content. It compares
the announced capabilities with `kRequiredCapabilities`. Because a core can hang
or crash in `retro_init`, products call it in a child process with a time limit;
the installed `bin/ayther_core_probe` command is that child.

Loading a native library executes code under platform-loader rules and is not
a sandbox or trust decision. Core probing applies only to the libretro provider;
the Tessera provider is linked at build time and is not probed.

## Audio QA observation

`engine/audio_observer.hpp` publishes borrowed views of audio facts, causes,
shared-state ordering, occurrences, and PCM, plus a non-owning binding installed
through `Config::audio_observer`. It depends only on C++20; it needs no Runtime,
SDL, disk, UI, RTTI, or transport format. Contract version `1.0` is independent
of the release and of the core ABI; other versions are rejected until supported
explicitly. Declaring a type does not announce instrumentation that is not
implemented.

- Every span and string, including those nested in variants, belongs to the
  producer and expires when the callback returns. The consumer copies what it
  needs into bounded storage and keeps its context alive until every producer
  is disconnected and stopped.
- Callbacks must not re-enter or mutate Engine, perform I/O, serialize, wait,
  or allocate without bound. A lost copy is recorded separately and never
  changes an audio decision. The binding changes only while producers are
  stopped; a null callback disables that stream.
- Fact identities are run-local (nonzero producer and sequence); the host adds
  its run identity when copying. An occurrence is distinct from its business
  key. PCM ranges are half-open in whole sample frames with explicit rate and
  timeline. Arrival order is not causal order. Unknown or not-applicable fields
  carry a reason instead of an invented value.
- Declared limits: 256 causes and state orders, 128 fields per fact, 4096 bytes
  per string, 64 KiB per encoded fact, 256 KiB of PCM per view, 192 kHz, and 8
  channels. The binding does not validate them; producers and the bridge must.

The catalog schema and what is observed are in
[Audio observation](AUDIO_OBSERVATION.md). Audio observers are unavailable with
the native provider.

### HD audio state and frozen production

These `AytherSession` operations support QA capture and checkpoint restore of
HD audio without advancing the emulated game:

| Operation | Contract |
|---|---|
| `audio_initial_snapshot()` | Owned copy of the current HD boundary, including restored requests and markers |
| `prepare_fresh_hd_audio()` | Starts a new HD session, keeping loaded assignments; fails when the backend cannot confirm that queued audio from the previous run was removed |
| `audio_hd_detector_windows_state()` / `restore_audio_hd_detector_windows()` | Copy and transactionally replace detector, edges, windows, anchors, and learning; opaque versioned payload up to 1 MiB, collections up to 4096 entries |
| `audio_hd_voices_state()` / `restore_audio_hd_voices()` | Copy voices and shared PCM without consuming them; replace atomically up to 256 voices and 64 MiB of PCM without mixing or queuing |
| `audio_hd_requests_pending_state()` / `restore_audio_hd_requests_pending()` | Copy and replace logical requests, triggers, and main staging (up to 16 MiB of pending PCM, contiguous batches, provenance of up to 256 original batches); restore first empties any inherited SDL backlog |
| `freeze_audio_production()` | Idempotently fixes the last frame and the half-open bound of produced main samples; afterwards the session runs no steps and the player accepts no new input, voices, priming silence, or restores |
| `finalize_audio_hd_voices_for_test()` | After freezing only: emits one `test_end` per active voice and retires them without mixing; a repeated call reports zero |
| `drain_frozen_audio()` | After freezing and with no active voices: delivers staged main audio inside the bound without producing samples; a failed SDL delivery keeps the block for retry |

`audio_hd_state.hpp` versions the HD state separately. Its validator checks the
opaque game-state identity, frame, and the five required sections before a
restore can mutate Engine. PCM identities are local to the state; occurrence
identity and business key stay separate, so two voices may share a key and PCM
without merging cursors or parameters. A copy is incomplete, and says so, when
an SDL queue is non-empty, an auxiliary delivery is in progress, or an auxiliary
conversion phase is non-zero: SDL cannot be inspected without consuming it.

## Render observation and visual state

`Config::render_observer` and `publish_render_observation()` are specified in
[Render observation](RENDER_OBSERVATION.md); `export_visual_state()` and
`restore_visual_state()` in [Visual state](VISUAL_STATE.md). The render observer
is unavailable with the native provider.

## Renderer ownership boundary

`<ayther/ayther_renderer.h>` is the public renderer surface, implemented by
`Ayther::engine`. `AytherRenderer` records offscreen work into a caller-provided
command buffer. It does not create or destroy the Vulkan instance, physical
device, logical device, surface, graphics queue, or swapchain, and it never
presents. Those objects remain owned by the host for the renderer's lifetime.

`init()` accepts the borrowed `VulkanContextView`, canvas dimensions, and the
installed shader directory. The renderer owns its offscreen targets, texture
caches, pipelines, comparison image, and readback resources. `shutdown()`
releases them deterministically before the host destroys the Vulkan context.
If it is omitted, the destructor performs the same cleanup through the retained
borrowed context, which must therefore still be alive.

The renderer consumes `FrameView` from either provider; it never sees which
emulator produced the frame beyond `FrameView::pixel_encoding`.

## Vulkan render-image handoff

`RenderImageView` is a trivially copyable, non-owning snapshot returned by
`AytherRenderer::render_image()` and `compare_render_image()`. It publishes the
borrowed `VkImage`, `VkImageView`, and `VkSampler`, the image format,
two-dimensional extent, handoff layout, barrier source stage and access masks,
and the exclusive owning queue-family index. The image, its memory, view, and
sampler remain owned by Engine. Runtime never destroys or frees them.

The main render target's handles stay valid until renderer resize, shutdown, or
destruction. Comparison views also end on comparison release or any recapture;
callers query a new view even when the implementation reuses the same handles.
A copied `RenderImageView` does not extend that interval. Before an
invalidating operation, Runtime completes all GPU access and discards or
rebinds descriptors that reference the old view.

Both render targets are handed over in the view's `layout`, currently
`VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` with fragment-shader read access as
the ready scope. Runtime may transition to another compatible use, but restores
the published layout and keeps exclusive ownership in `queue_family_index`
before the next Engine access. Queue-family transfer is outside this contract.

`RenderImageView` transfers no semaphore, fence, event, command buffer, or
queue. When Engine production and Runtime consumption share a command buffer,
command order and the documented barriers are the synchronization. Across
submissions, Runtime owns the signal/wait chain in both directions: it waits
for Engine production before reading and makes its own completion visible
before Engine reuses, resizes, releases, or destroys the image.

`is_valid()` checks handle and metadata presence, not GPU completion.
Destroying the C++ value performs no Vulkan work.

## Dependency containment

`capabilities.hpp` exposes only fixed-width standard types. `core_probe.hpp`
exposes standard filesystem, ownership, and string types plus `ayther::Result`.
`native_emulation.hpp` exposes only standard types. Libretro, Tessera, SDL,
platform loader headers, threads, and logging remain implementation
dependencies.

`vulkan_interop.hpp` deliberately exposes Vulkan types, so the installed
`Ayther::engine` target carries `Vulkan::Vulkan` as a public usage requirement.
`Tessera::core` is a private link dependency; when the package was built with
Tessera, `AytherConfig.cmake` resolves `find_dependency(Tessera CONFIG)` so the
static link closes. Other third-party dependencies remain private unless a
public declaration requires them.

## Compatibility and deprecation

Source compatibility holds within `0.1.x`. Binary compatibility across compiler
versions, standard libraries, build modes, flags, or toolchains is not promised;
consumers rebuild against the headers and library shipped together.

A public symbol is deprecated before removal, with documentation naming the
replacement and the first version in which removal is allowed. During `0.1.x`,
removal or a source-incompatible semantic change waits for `0.2.0`. Security or
correctness defects that cannot be preserved safely are documented as explicit
compatibility exceptions.

The maintained consumers during `0.1.x` are AYTHER Runtime, AYTHER Play when it
uses the same reviewed modules, and this repository's package and conformance
tests. Other consumers may not expand the boundary without an Engine review.
Engine maintainers own the declarations, implementation, installation rules,
documentation, and contract tests.
