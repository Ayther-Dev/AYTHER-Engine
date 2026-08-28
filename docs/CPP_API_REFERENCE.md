# C++ API and implementation contracts

**Status:** pre-release, installable engine contracts

**Last reviewed:** 2026-08-27

This document describes the C++ source currently under `include/ayther/` and
`src/`. It records contracts that are otherwise easy to miss at call sites:
ownership, lifetime, thread affinity, failure behavior, and performance-sensitive
boundaries. It is not a compatibility guarantee; the installed engine remains
pre-release.

> [!WARNING]
> The root build publishes `Ayther::core` and `Ayther::engine`, and Windows
> compile/link/install/consume checks pass. The C++ layer has not passed a
> complete sanitizer, renderer, audio, GPU, or emulator-integration matrix.

## Documentation conventions

The C++ sources follow these documentation rules:

- comments and public API documentation are written in English;
- names and types express behavior where possible; comments explain rationale,
  invariants, ownership, units, synchronization, and non-obvious costs;
- `T*` and `T&` are non-owning unless a boundary explicitly says otherwise;
- owning resources use RAII types or an explicitly documented create/free pair;
- a returned view or pointer must document the operation that invalidates it;
- fallible operations document whether they return `Result`, a status value,
  `nullptr`, or an empty result;
- hot-path claims require measurement and must state the workload used.

These conventions align with the C++ Core Guidelines principles of explicit
interfaces, resource safety, type safety, immutability by default, and measured
optimization.

Standards basis: [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines),
especially the Philosophy, Interfaces, Functions, Resource management,
Concurrency, Source files, and Performance sections.

## Supported and provisional surfaces

| Surface | Status | Contract |
|---|---|---|
| `Ayther::core` and `ayther_core_ffi.h` | Buildable, installable, unstable | Flat C ABI backed by Rust. Opaque handles use paired allocation and release functions. |
| `Ayther::engine` and `ayther_sdk.h` | Buildable, installable, provisional | Higher-level C facade over a native session. |
| `AytherSession` | Installed, provisional | Primary C++ orchestration facade. Single-owner and single-thread driven. |
| Audio, renderer, Vulkan, video, recording, and libretro helpers | Source-tree internal | Implementation components are not installed and have no standalone compatibility promise. |

## Ownership and lifetime map

| Type or value | Ownership | Lifetime and invalidation |
|---|---|---|
| `std::unique_ptr<AytherSession>` | Exclusive | Returned by `AytherSession::create`; destruction releases session-owned state. |
| Rust opaque handles | Exclusive through `unique_handle` or paired C functions | Never dereference. Release with the matching API function only. |
| `FrameView` returned by `step()` | Borrowed | Valid until the next operation that advances, resets, rewinds, reloads, or destroys the session. Copy data that must outlive that boundary. |
| Memory returned by `RetroRunner` | Borrowed from the loaded core | Invalid after core reset, unload, or any operation documented by the core as reallocating memory. |
| `AyArchive*` passed to renderer, audio, or video helpers | Borrowed | The caller must keep the archive alive for the complete call or cached operation that documents retention. |
| SDL and Vulkan handles | Owned by their wrapper or owning subsystem | Destruction order matters. Device-dependent resources must be released before their `VkContext`. |
| Callback `user` pointers in the C facade | Borrowed | Must remain valid until the callback is removed or the session is destroyed. Callbacks must not retain transient frame pointers. |

## Primary session contract

`AytherSession` coordinates the emulator host, Rust identity/substitution
handles, scripting, pack state, audio, recording, rewind, and frame extraction.

Creation is transactional from the caller's perspective:

1. `create()` receives immutable configuration.
2. It allocates the Rust-side handles.
3. It loads the libretro core and user-provided ROM.
4. It applies optional in-memory patches and core options before game loading.
5. It initializes optional audio and pack state.
6. It returns either an owning session or a typed error.

The session is not thread-safe. Drive `set_input()`, `step()`, reset, rewind,
pack changes, and recording operations from one owning thread. Frontends may
copy immutable frame data to another thread after respecting the `FrameView`
lifetime boundary.

Failure to activate optional replacement content should preserve native output
when safe. Core/ROM loading, invalid required configuration, failed patches, and
trust or schema failures are not optional and must remain observable errors.

## Frame pipeline

One successful `step()` represents exactly one emulated frame:

1. apply current input;
2. execute the libretro frame;
3. capture the supported memory and event observations;
4. derive tile, sprite, background, and audio identities;
5. execute bounded scripting and apply overrides;
6. resolve replacement content;
7. stage and route audio on the frame timeline;
8. publish a borrowed `FrameView`.

Deterministic inputs, pack data, and emulator state must produce deterministic
identity and scheduling decisions. Presentation and device timing must not alter
identity generation.

## Subsystem contracts

### Emulator host

`CoreLoader` owns one dynamic library handle. `RetroRunner` binds the libretro
C callback model to an object instance, owns ROM bytes and core lifecycle, and
exposes borrowed memory views. Dynamic libraries and ROMs are untrusted inputs.
Symbol resolution, capability negotiation, buffer bounds, and versioned AYTHER
extensions must be checked before use.

The callback bridge currently relies on process-visible dispatch state. Until
that design is replaced or formally constrained, callers must not drive two
runner instances concurrently.

### Audio

`AudioPlayer` owns the SDL device and streams. `init()` is explicit and
fallible; `shutdown()` is idempotent and is also called by the destructor.
Emulator batches and replacement voices share a frame-aligned staging timeline.

Current mix-ready PCM is interleaved signed 16-bit stereo at 44.1 kHz. Units
must be explicit: APIs distinguish sample frames, scalar samples, emulation
frames, seconds, and absolute stream positions. Borrowed PCM pointers are valid
only for the duration of the call unless copied into a documented cache.

`HdMixer` is a deterministic buffer mixer and does not call SDL. Shared PCM uses
`shared_ptr<const vector<int16_t>>` because several voices may reuse one decoded
asset and active voices may outlive cache invalidation.

### Rendering and Vulkan

`AytherRenderer` consumes `FrameView` data and owns device-dependent render
resources. It is intentionally separate from the session so headless execution
does not require Vulkan.

The renderer and its Vulkan helpers are thread-affine to the caller's render
thread. The caller must ensure that `VkContext` outlives every dependent object
and that GPU work is synchronized before resources referenced by submitted
commands are destroyed or replaced. Explicit `shutdown(context)` requirements
are provisional and must be treated as mandatory.

Asynchronous sprite decoding owns CPU buffers until the render thread pumps the
completed uploads. Worker shutdown must wake the condition variable and join the
thread before queues, caches, or the owning object are destroyed.

### Video

`VideoSource` abstracts random or ranged reads without transferring ownership
of external archives. `VideoClip` exclusively owns its source and decoder state.
Opening, probing, indexing, decoding, and seeking are fallible. Callers must
check dimensions and byte counts before allocating or copying frame data.

### Recording and rewind

Recording data is deterministic state, not a general media container. Readers
must validate lengths before allocation and treat compressed payloads as
untrusted. Rewind owns a bounded ring of compressed states; enabling it has a
memory cost proportional to state size, capture cadence, and requested history.

### Component serialization

The component TOML helpers convert typed native structures to and from pack
documents. Parse functions append or replace data only as documented and must
not leave partially trusted state active after a fatal parse error. Schema and
compatibility policy remain owned by the core; native parsing must not silently
invent a newer public contract.

## Header groups

| Group | Headers | Purpose |
|---|---|---|
| Facades and contracts | `ayther_session.h`, `ayther_sdk.h`, `ayther_result.h`, `ayther_sdk_version.h` | Session orchestration, C facade, errors, compatibility |
| Identity and composition | `ayther_layers.h`, `ayther_animation.h`, `ayther_audio_events.h`, `ayther_mode3.h`, `widescreen.h`, `parallax_bands.h`, `pano_bands.h`, `panorama_cover.h` | Frame interpretation and replacement composition |
| Audio | `audio_player.h`, `audio_hd_mixer.h`, `audio_live_resume.h`, `audio_match_rule.h`, `audio_seq_anchor.h`, `audio_bus_balance.h`, `audio_asset_level.h`, `voice_router.h`, `psg_synth.h` | Capture, matching, synthesis, routing, mixing, analysis |
| Video and rendering | `ayther_video.h`, `ayther_renderer.h`, `vulkan_backend/*.h` | Decode, GPU upload, composition, readback |
| Runtime state | `ayther_recording.h`, `rewind_buffer.h`, `failure_escalation.h`, `output_profile.h`, `ayther_config.h` | Persistence, recovery, policy, output geometry, configuration |
| Emulator integration | `libretro_host/core_loader.h`, `libretro_host/retro_runner.h`, `libretro_host/ayther_api.h` | Dynamic loading, libretro lifecycle, versioned extensions |
| Rust boundary | `ayther_core_ffi.h`, `ayther_unique_handle.h` | Flat ABI and RAII ownership adapters |

`libretro_host/libretro.h` is a third-party protocol header. Preserve its
upstream documentation and licensing; project-specific contracts belong in the
wrapper headers instead.

## Error model

- C++ facade operations use `Result<T>` where a typed failure is part of the
  contract.
- Narrow internal helpers may use `bool`, `nullptr`, or empty values only when
  the missing diagnostic is not needed by their caller.
- The C API returns `AyStatus` and exposes stable buffers only for the lifetime
  documented by that API.
- Destructors and cleanup paths must not throw.
- Logging is diagnostic output, not an error-transport mechanism.

## Compatibility and legal boundary

This documentation does not authorize distribution of ROMs, BIOS images,
commercial assets, emulator cores, private keys, or other content without the
necessary rights. In-memory patching does not transfer rights to the underlying
game. Dynamic core loading does not establish that a core is safe, compatible,
or distributable.
