# C++ API and implementation contracts

**Status:** pre-release, installable engine contracts.

This document describes the C++ source under `include/ayther/` and `src/`. It
records contracts that are easy to miss at call sites: ownership, lifetime,
thread affinity, failure behavior, and performance-sensitive boundaries. It is
not a compatibility guarantee; see [API and compatibility](API_COMPATIBILITY.md)
for that, and the [Engine public API contract](ENGINE_PUBLIC_API_CONTRACT.md)
for the rules of the installed `ayther/engine/` surface.

> [!WARNING]
> The root build publishes `Ayther::core` and `Ayther::engine`, and CI runs
> compile, link, install, and out-of-tree consumer checks plus AddressSanitizer
> and UndefinedBehaviorSanitizer over the CPU test suite. GPU, audio-device,
> and real-core integration are not covered by a complete cross-platform
> matrix.

## Documentation conventions

- Comments and public API documentation are written in English.
- Names and types express behavior where possible; comments explain rationale,
  invariants, ownership, units, synchronization, and non-obvious costs.
- `T*` and `T&` are non-owning unless a boundary explicitly says otherwise.
- Owning resources use RAII types or an explicitly documented create/free pair.
- A returned view or pointer documents the operation that invalidates it.
- Fallible operations document whether they return `Result`, `NativeResult`, a
  status value, `nullptr`, or an empty result.
- Hot-path claims require measurement and state the workload used.

These follow the [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines),
especially the Philosophy, Interfaces, Functions, Resource management,
Concurrency, Source files, and Performance sections.

## Supported and provisional surfaces

| Surface | Status | Contract |
|---|---|---|
| `Ayther::core` and `ayther_core_ffi.h` | Installed, unstable | Flat C ABI backed by Rust. Opaque handles use paired allocation and release functions. |
| `Ayther::engine` and `ayther_sdk.h` | Installed, provisional | Higher-level C facade over a native session. |
| `engine/capabilities.hpp`, `engine/core_probe.hpp`, `engine/input.hpp`, `engine/pack.hpp` | Installed, provisional | Typed queries for versions, core metadata, input, packs, validation, tiers, and watching. Raw core and libretro declarations do not cross this surface. |
| `AytherSession` | Installed, provisional | Primary C++ orchestration facade over either emulation provider. Single-owner, single-thread driven. |
| `engine/native_emulation.hpp` | Installed, provisional | Typed driver for the native Tessera provider; answers `unsupported_capability` in builds without Tessera. |
| `engine/audio_observer.hpp` and related audio QA headers | Installed, contract 1.0 | Borrowed views of facts and PCM; non-owning binding with no transport and no decision feedback. |
| `engine/render_observer.hpp`, `engine/visual_state.hpp` | Installed, provisional | [Render observation](RENDER_OBSERVATION.md) and [Visual state](VISUAL_STATE.md). |
| `AytherRenderer` and `engine/vulkan_interop.hpp` | Installed, provisional | Offscreen Vulkan renderer and borrowed-handle handoff contract. |
| Audio player, renderer internals, Vulkan backend, video, libretro host, observation source, Tessera backend | Source-tree internal | Not installed; no standalone compatibility promise. |

## Ownership and lifetime map

| Type or value | Ownership | Lifetime and invalidation |
|---|---|---|
| `std::unique_ptr<AytherSession>` | Exclusive | Returned by `AytherSession::create`; destruction releases session-owned state. |
| Rust opaque handles | Exclusive through `unique_handle` or paired C functions | Never dereference. Release with the matching API function only. |
| `FrameView` returned by `step()` | Borrowed | Valid until the next operation that advances, resets, rewinds, reloads, or destroys the session. Copy data that must outlive that boundary. |
| `FrameView::native_original` and `native_frame()` | Borrowed from Tessera | Pixel and PCM spans expire at the next successful native step, reset, restore, or close, and when the session is destroyed. |
| `NativeEmulation *` from `native_emulation()` | Borrowed from the session | Null for the libretro provider. Valid until the session is destroyed. |
| `engine::NativeSnapshot` | Exclusive, move-only reference | Holds its own reference; remains readable after the native session closes. |
| Memory returned by `RetroRunner` | Borrowed from the loaded core | Invalid after core reset, unload, or any operation the core documents as reallocating memory. |
| `engine::PackView` returned by `AytherSession::pack()` | Borrowed | Trivially copyable and non-owning. Invalid after the session reloads, replaces, or destroys its pack. |
| `engine::PackInfo` and `engine::PackValidationResult` | Owned values | Strings and findings remain valid independently of temporary archive and report handles. |
| `engine::PackWatcher` | Exclusive, move-only RAII | `create()` starts the platform watcher; destruction stops it. `poll()` is non-blocking. |
| `VulkanContextView` handles | Borrowed by Engine from the host | Device-dependent Engine resources and submitted work must finish before the host destroys its device or allocator. |
| Callback `user` pointers in the C facade | Borrowed | Must remain valid until the callback is removed or the session is destroyed. Callbacks must not retain transient frame pointers. |

## Primary session contract

`AytherSession` coordinates the emulator, Rust identity and substitution
handles, scripting, pack state, audio, recording, rewind, and frame extraction.

Provider selection is explicit through `Config::provider`:

- `EmulationProvider::libretro` (default) loads `core_path` and `rom_path`.
- `EmulationProvider::tessera_native` creates a Tessera session from
  `Config::native_config` before any libretro initialization. The overload
  `create(cfg, native_result)` returns the exact `NativeResult`. There is no
  provider fallback.

[Tessera native provider](TESSERA_NATIVE.md) documents configuration, formats,
and which session features work on the native path.

Creation is transactional from the caller's perspective:

1. `create()` receives immutable configuration and rejects options the
   selected provider does not support (for `tessera_native`: `patch_path`,
   `core_options`, `rom_path`, `audio_observer`, `render_observer`).
2. It allocates the Rust-side handles.
3. It creates the emulator: loads the libretro core and ROM, applying optional
   in-memory patches and core options before game loading; or creates the
   Tessera session.
4. It initializes optional audio and pack state.
5. It returns either an owning session or a typed error.

The session is not thread-safe with either provider. Drive `set_input()`,
`step()`, reset, rewind, pack changes, recording, and the native driver from one
owning thread. Frontends may copy immutable frame data to another thread after
respecting the `FrameView` lifetime boundary.

`engine/input.hpp` publishes the libretro joypad bit positions as
`RetroPadButton`. `input_mask()` converts a button for the raw `std::uint16_t`
`set_input()` overload without exposing a libretro header; `InputState` is the
typed multi-button value. With the native provider, the RetroPad mask is mapped
to the controls the native configuration declares (A=Y, B=B, C=A, as in Genesis
Plus GX).

Failure to activate optional replacement content preserves native output when
safe. Emulator creation, invalid required configuration, failed patches, and
trust or schema failures are not optional and remain observable errors.

## Frame pipeline

One successful `step()` represents exactly one emulated frame, with either
provider:

1. apply current input;
2. execute the frame (libretro `retro_run`, or a complete Tessera input frame);
3. capture the supported memory and event observations through the session's
   `ObservationSource`;
4. derive tile, sprite, background, and audio identities;
5. execute bounded scripting and apply overrides;
6. resolve replacement content;
7. stage and route audio on the frame timeline;
8. publish a borrowed `FrameView`.

With the native provider, `step()` converts the Tessera original into the same
frame-buffer path the libretro provider uses, so `FrameView` keeps the
`libretro` pixel encoding; a failed advance is reported in
`FrameView::native_result`. `poke()` and cheats write work RAM through the
native control path.

The native provider also has a direct driver, `native_emulation()`, that takes a
complete input state for the next frame or a validated bound trace and
preserves EOF, pending transactions, confirmation, and cancellation. After
driving it directly, `native_frame()` refreshes the borrowed originals as
`rgba8` (`FrameView::pixel_encoding`, `native_original`); that path composes
only the pack's tile rules and states the reason in
`FrameView::native_original_reason`. Use `step()` for full pack composition.

Deterministic inputs, pack data, and emulator state produce deterministic
identity and scheduling decisions. Presentation and device timing do not alter
identity generation.

## Subsystem contracts

### Emulator host

Public consumers inspect a libretro core with the move-only
`ayther::engine::CoreProbe`, which owns one dynamic library handle and copies
`CoreInfo` before exposing metadata. Platform handles, symbol lookup, and
libretro structures remain private.

Internally, the session reads its emulator through one `ObservationSource`
(`include/ayther/observation/observation_source.h`, not installed). Its
primitives mirror `ayther_interface_v1`; `RetroRunner` implements it from a
libretro core's extension descriptor and `TesseraBackend` from a native
session. Snapshot validation, sprite reads, audio polling, and controls are
implemented once on that interface, so both providers build `FrameView`
through the same code.

`RetroRunner` binds the libretro C callback model to an object instance, owns
ROM bytes and core lifecycle, and exposes borrowed memory views. Callback
dispatch is thread-scoped: a `CallbackScope` binds the active runner in a
`thread_local` for the duration of each synchronous entry point and restores
the previous one on exit. Whether two runners can coexist also depends on the
core binary's own global state, which Engine does not control.

Dynamic libraries and ROMs are untrusted inputs. Symbol resolution, capability
negotiation, buffer bounds, and versioned extensions are checked before use;
see [Emulator extension ABI](EMULATOR_EXTENSION_ABI.md).

### Packs and core ABI

Public C++ consumers include `engine/pack.hpp`. `inspect_pack()` opens a pack
temporarily and copies its manifest metadata into `PackInfo`; `validate_pack()`
copies every diagnostic into `PackValidationResult`. An optional
trust-registry path can be supplied when inspection must authenticate signed
fixtures.

`AytherSession::pack()` returns `PackView`, never `AyArchive*`. The view may be
passed to `AytherRenderer`, used to choose a render tier before assets are
loaded, or queried for owned metadata. It must not outlive a pack reload or its
session. `PackWatcher` encapsulates the platform thread and raw watcher handle;
destroy it before the session.

`PackView::select_render_tier_for_height()` selects HD through 720 pixels,
Full HD through 1080, 2K through 1440, 4K through 2160, and 8K above that. It
uses the smallest included tier at least as large as the requested tier, or the
largest available tier when none qualifies. Flat packs are unchanged. The same
mapping applies to `ayther_pack_set_tier_for_height()` in the C ABI.

`engine::core_abi_revision()` reports the linked core ABI revision without the
C header. Game-specific work-RAM interpretation is outside Engine. The flat
`ayther_core_ffi.h` surface remains for callers that select the C ABI and for
Engine internals.

### Audio

`AudioPlayer` owns the SDL device and streams. `init()` is explicit and
fallible; `shutdown()` is idempotent and also called by the destructor.
Emulator batches and replacement voices share a frame-aligned staging timeline.

Mix-ready PCM is interleaved signed 16-bit stereo at 44.1 kHz. The native
provider's original PCM carries its own rate (48 kHz for the Mega Drive
machine); when device audio is enabled, the session resamples it to 44.1 kHz
with phase carried across frames. Units are explicit: APIs distinguish sample
frames, scalar samples, emulation frames, seconds, and absolute stream
positions. Borrowed PCM pointers are valid only for the duration of the call
unless copied into a documented cache.

`HdMixer` is a deterministic buffer mixer and does not call SDL. Shared PCM uses
`shared_ptr<const vector<int16_t>>` because several voices may reuse one decoded
asset and active voices may outlive cache invalidation.

### Rendering and Vulkan

`AytherRenderer` consumes `FrameView` data and owns device-dependent render
resources. It is separate from the session so headless execution does not
require Vulkan.

The renderer and its Vulkan helpers are thread-affine to the caller's render
thread. The caller passes a borrowed `engine::VulkanContextView` and keeps its
instance, physical device, logical device, graphics queue, queue family, and
VMA allocator valid until every dependent object is released. GPU work must be
synchronized before resources referenced by submitted commands are destroyed
or replaced. `shutdown(context_view)` releases deterministically before the
host context is torn down; otherwise the destructor releases initialized
resources, so the borrowed context must still be alive.

Asynchronous sprite decoding owns CPU buffers until the render thread pumps the
completed uploads. Worker shutdown wakes the condition variable and joins the
thread before queues, caches, or the owning object are destroyed.

The renderer targets an offscreen image with color-attachment, transfer-source,
transfer-destination, and sampled usage. The frontend owns presentation.

The lifecycle is:

1. the application creates and owns its Vulkan context and presentation state;
2. it passes a borrowed `VulkanContextView` to `AytherRenderer::init`, which
   creates device-dependent resources for one extent;
3. each frame, the session produces `FrameView` and the renderer records into a
   caller-provided command buffer;
4. the frontend presents, samples, or reads back the offscreen target;
5. resize waits for conflicting GPU work, then recreates the target and every
   dependent framebuffer or descriptor;
6. `shutdown(context_view)` releases renderer resources before the host context
   is destroyed.

Render and presentation share one command buffer and submission. Independent
renderer submission and a dedicated render thread are not part of the current
contract. Layout transitions must cover every use of the offscreen image; a
missing barrier is a correctness failure.

Plane-aware video composition and widescreen behavior have dedicated contracts:
[Cinematic plane composition](CINEMATIC_PLANE_COMPOSITION.md) and
[Widescreen composition](WIDESCREEN.md). Sprite and plane replacements follow
the [Replacement composition rules](REPLACEMENT_COMPOSITION.md); the
[Render observation](RENDER_OBSERVATION.md) and [Visual state](VISUAL_STATE.md)
contracts cover per-frame inspection and checkpoint restore.

### Video

`VideoSource` abstracts random or ranged reads without transferring ownership
of external archives. `VideoClip` exclusively owns its source and decoder state.
Opening, probing, indexing, decoding, and seeking are fallible. Callers check
dimensions and byte counts before allocating or copying frame data.

### Recording and rewind

Recording data is deterministic state, not a general media container. Readers
validate lengths before allocation and treat compressed payloads as untrusted.
Takes record their emulator provenance (`gpgx` or `tessera`); see
[API and compatibility](API_COMPATIBILITY.md#take-recording-format).

Rewind owns a bounded ring of compressed states. With the libretro provider
they are core serialization states; with the native provider they are Tessera
checkpoints, whose size varies from frame to frame. Memory cost is
proportional to state size, capture cadence, and requested history.

### Determinism and headless oracles

`AytherSession` steps without Vulkan or an audio device. A headless oracle can
load a legally distributable fixture, run an ordered input stream, serialize
state, restore it, and compare re-simulated frames and observations.

Deterministic code uses emulation frames or recorded stream positions. Wall
time, presentation timing, device callbacks, filesystem ordering, and GPU
results never feed back into session state. The guarantee is conditional on
deterministic behavior from the selected emulator and all supplied inputs.
`emulation_generation()` names the emulator, version, and determinism contract
of a session so recordings and packs can record their origin.

### Component serialization

The component TOML helpers convert typed native structures to and from pack
documents. Parse functions append or replace data only as documented and do
not leave partially trusted state active after a fatal parse error. Schema and
compatibility policy are owned by the core; native parsing does not invent a
newer public contract.

## Header groups

This table covers the source tree. The installed subset is listed in the
[Public API index](PUBLIC_API_INDEX.md).

| Group | Headers | Purpose |
|---|---|---|
| Facades and contracts | `ayther_session.h`, `ayther_sdk.h`, `ayther_result.h`, `ayther_sdk_version.h`, `engine/engine.hpp` | Session orchestration, C facade, errors, compatibility |
| Identity and composition | `ayther_layers.h`, `ayther_animation.h`, `ayther_audio_events.h`, `ayther_mode3.h`, `widescreen.h`, `parallax_bands.h`, `pano_bands.h`, `panorama_cover.h` | Frame interpretation and replacement composition |
| Audio | `audio_player.h`, `audio_hd_mixer.h`, `audio_live_resume.h`, `audio_match_rule.h`, `audio_seq_anchor.h`, `audio_bus_balance.h`, `audio_asset_level.h`, `voice_router.h`, `psg_synth.h`, `engine/audio_*.hpp`, `engine/music_*.hpp` | Capture, matching, synthesis, routing, mixing, analysis, QA observation |
| Video and rendering | `ayther_video.h`, `ayther_renderer.h`, `engine/render_observer.hpp`, `engine/vulkan_interop.hpp` | Decode, GPU upload, composition, readback, handoff |
| Runtime state | `ayther_recording.h`, `rewind_buffer.h`, `failure_escalation.h`, `output_profile.h`, `ayther_config.h`, `runtime_options.h`, `engine/visual_state.hpp` | Persistence, recovery, policy, output geometry, configuration |
| Emulator integration | `engine/core_probe.hpp`, `engine/native_emulation.hpp`, `emulation_generation.h`, `observation/observation_source.h`, `libretro_host/retro_runner.h`, `libretro_host/ayther_api.h` | Core probing, native provider, provenance, observation surface, libretro lifecycle, versioned extension |
| Rust boundary | `ayther_core_ffi.h`, `ayther_unique_handle.h` | Flat ABI and RAII ownership adapters |

`libretro_host/libretro.h` is a third-party protocol header. Preserve its
upstream documentation and licensing; project-specific contracts belong in the
wrapper headers.

## Error model

- C++ facade operations use `Result<T>` where a typed failure is part of the
  contract. Native-provider operations use `engine::NativeResult`, which keeps
  the Tessera status, phase, OS error, frame identity, and transaction context.
- Narrow internal helpers may use `bool`, `nullptr`, or empty values only when
  the caller does not need the missing diagnostic.
- The C API returns `AyStatus` and exposes stable buffers only for the lifetime
  documented by that API.
- Destructors and cleanup paths do not throw.
- Logging is diagnostic output, not an error-transport mechanism. Engine code
  logs through `ayther::log` (severity, component, stable event id, typed
  fields) and a host may install its own sink.

## Compatibility and legal boundary

This documentation does not authorize distribution of ROMs, BIOS images,
commercial assets, emulator cores, private keys, or other content without the
necessary rights. In-memory patching does not transfer rights to the underlying
game. Dynamic core loading does not establish that a core is safe, compatible,
or distributable. See [Legal and distribution boundaries](LEGAL_AND_DISTRIBUTION.md).
