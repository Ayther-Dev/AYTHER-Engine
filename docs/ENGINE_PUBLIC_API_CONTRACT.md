# Engine public API contract

**Status:** implemented for the installed C++ Engine surface, including typed
input, session, renderer, layers, pack inspection, Libretro core probing, build
capabilities, and the Vulkan render-image handoff.

The ownership decision, authorized consumers, and `0.1.x` guarantees are
recorded in [ADR 0003](adr/0003-runtime-engine-public-api-ownership.md).

## Public boundary

The primary Runtime-facing C++ entry point lives under
`include/ayther/engine/` and is included with installed paths such as:

```cpp
#include <ayther/engine/engine.hpp>
#include <ayther/engine/capabilities.hpp>
#include <ayther/engine/core_probe.hpp>
#include <ayther/engine/input.hpp>
#include <ayther/engine/vulkan_interop.hpp>
```

`engine.hpp` is the umbrella include. It directly publishes the typed Engine
modules and the installed C++ session, renderer, and layer facades; consumers
that want the complete C++ API need no additional AYTHER header.

The existing flat `include/ayther/` C++ facades remain installable and are
reachable through the umbrella during the 0.1.x line so current consumers
retain source compatibility. New focused APIs belong under `ayther/engine/`;
the flat C ABI remains available separately through `ayther_core_ffi.h`.

Public headers must be self-contained and compile as the first include in an
otherwise empty C++20 translation unit. They may include the C++ standard
library, documented third-party API headers, or other installed
`ayther/engine/` headers. They must not include paths containing `src/`,
`libretro_host/`, `vulkan_backend/`, `private/`, `internal/`, or `detail/`. A
future `detail/` directory requires an explicit contract, installation rule,
and compatibility coverage before use.

## Language, exceptions, and RTTI

- C++20 is the minimum language version.
- Functions marked `noexcept` do not allocate, do not throw, and terminate no
  process on an environmental failure.
- Expected failures in fallible APIs use a typed result and stable error code.
  Exceptions never cross the Runtime-to-Engine boundary.
- Public behavior must not depend on RTTI. Public interfaces do not require
  consumers to enable RTTI, and concrete implementation types are not exposed
  for `dynamic_cast` or `typeid`.

## Ownership and lifetime

- Value types are owned by the caller after return.
- Owning objects are represented by move-only RAII handles. Their destructors
  release Engine-owned resources; copying ownership is forbidden.
- Borrowed references, spans, string views, and native handles name their owner
  and validity interval in the declaring header. A borrowed value never
  extends the owner's lifetime.
- Vulkan handles remain owned by the party named by `vulkan_interop.hpp`.
  Exporting or borrowing a handle never transfers ownership implicitly.

## Thread safety and synchronization

- Immutable value operations and the version/capability queries are safe for
  concurrent calls.
- Mutable Engine objects are single-owner and single-threaded unless their
  declaring header explicitly says otherwise.
- A Vulkan interop contract must state the queue, command-buffer, semaphore,
  image-layout, and destruction responsibilities for every borrowed or
  transferred handle. Runtime may not infer synchronization from a raw handle.

## Errors

`version()` and `probe_capabilities()` are total, side-effect-free `noexcept`
queries. They report the linked artifact, not current device availability, so
there is no environmental failure path. APIs that inspect a loader, GPU,
display, audio device, filesystem, configuration source, or emulator core are
fallible and must return either an unavailable capability or a typed error.
Diagnostics and logging are not substitutes for a returned error.

## Version and capability identity

`ayther::engine::version()` reports the numeric release embedded in the linked
Engine artifact: `0.1.0` for this line. The release-candidate distribution keeps
the identifier `v0.1.0-rc.5` in its immutable tag, archive names, provenance,
and release metadata. The `rc.5` suffix is distribution identity rather than a
fourth field of `Version`, so logs and UI format the linked numeric value through
the API instead of embedding another version literal.

`ayther::engine::core_abi_revision()` forwards the linked Core's non-zero ABI
revision. It is independent from SemVer and must be incremented whenever an
incompatible Engine/Core function signature or shared data layout changes.
Tests compare the public query with `AYTHER_CORE_C_ABI_REVISION` and compare the
linked numeric version with the canonical release macros and CMake project
version.

## Compatibility and deprecation

The release candidate guarantees source compatibility within the 0.1.x line.
It does not guarantee binary compatibility across compiler versions, C++
standard libraries, build modes, compiler flags, or platform toolchains.
Consumers must rebuild against the headers and library shipped together.

A public symbol is deprecated before removal, with documentation naming the
replacement and the first version in which removal is permitted. During 0.1.x,
removal or a source-incompatible semantic change is deferred to 0.2.0. Security
or correctness defects that cannot be preserved safely are documented as an
explicit compatibility exception.

The maintained consumers during `0.1.x` are AYTHER Runtime, AYTHER Play when it
uses the same reviewed modules, and this repository's package/conformance
tests. Lab, SDK tools, and third parties may not expand the boundary without a
new Engine review. AYTHER Engine maintainers own the declarations,
implementation, installation rules, documentation, and contract tests.

## Artifact version and capabilities

`version()` returns the version compiled into the linked Engine library. It is
implemented out of line, so Runtime compile definitions cannot change it.

`probe_capabilities()` returns build capabilities only. It reads no registry,
singleton, environment variable, configuration file, filesystem path, or
device state. It does not load or initialize Vulkan, create a window, open an
audio device, or start a thread. Therefore it is callable before any Engine
instance and behaves identically on a machine without a GPU or display.

For the current native artifact:

| Field | Meaning |
|---|---|
| `renderer` | Renderer implementation compiled into Engine (`vulkan` or `none`) |
| `hardware_acceleration` | The compiled renderer uses hardware acceleration when initialized; it is not a device-presence check |
| `external_image_import` | A public external-image import contract is compiled and available |
| `libretro_video` | The Engine artifact implements the libretro video callback path |
| `libretro_audio` | The Engine artifact implements the libretro audio callback paths |

Vulkan loader/device suitability belongs to Engine creation or a future typed
environment probe. Keeping it separate is what makes capability probing
deterministic and safe in headless processes.

## Libretro core probing

`probe_core(path)` loads one user-selected native library without initializing
SDL, loading a ROM, or calling `retro_init`. Success returns a move-only
`CoreProbe`; its destructor unloads the library. `CoreProbe::info()` returns a
`CoreInfo` whose name, version, extension list, API version, and path/extraction
flags were copied while the library was loaded. No `retro_system_info` pointer
or platform handle is public.

The factory returns `Result<CoreProbe>`. `ErrorCode::Io` means the platform
loader rejected the file, while `ErrorCode::BadFormat` means the library loaded
but omitted `retro_api_version` or `retro_get_system_info`. The owned diagnostic
contains the platform error or missing symbol names. A failed factory call
releases any handle acquired before the failure.

`CoreInfo::serialize()` and `CoreProbe::serialize()` return the same compact
JSON object. Strings supplied by the core escape quotes, reverse solidi, and
all JSON control characters. Protocol framing such as `AYTHER_STATUS`, event
names, and process exit codes remains a Runtime responsibility.

Loading a native library executes code under platform-loader rules and is not
a sandbox or trust decision. `CoreProbe` is single-owner; destruction or moves
must not race with access to its information.

## Renderer ownership boundary

`<ayther/ayther_renderer.h>` is the canonical public renderer surface and every
declared method is implemented by `Ayther::engine`. `AytherRenderer` records
offscreen work into a caller-provided command buffer; it does not create or
destroy the Vulkan instance, physical device, logical device, surface, graphics
queue, or swapchain, and it never presents. Those objects remain owned by the
host application through the complete renderer lifetime.

`init()` accepts the borrowed `VulkanContextView`, canvas dimensions, and the
installed shader directory. The renderer owns its offscreen targets, texture
caches, pipelines, comparison image, and readback resources. `shutdown()`
provides deterministic release before the host destroys the Vulkan context. If
it is omitted, the destructor performs the same cleanup using the retained
borrowed context; consequently that context must still be alive during renderer
destruction.

## Vulkan render-image handoff

`RenderImageView` is a trivially copyable, non-owning snapshot returned by
`AytherRenderer::render_image()` and `compare_render_image()`. It publishes the
borrowed `VkImage`, `VkImageView`, and `VkSampler`, together with the
image format, two-dimensional extent, handoff layout, barrier source stage and
access masks, and the exclusive owning queue-family index. The image, its
memory, its view, and its sampler remain owned by Engine. Runtime never calls a
Vulkan destruction or memory-release function for those handles.

The main render target's handles remain valid until renderer resize, shutdown,
or destruction. Comparison views additionally end on comparison release or any
recapture; callers must query a new view even when the implementation reuses the
same handles and size. A copied `RenderImageView` does not extend that interval.
Before an invalidating operation, Runtime must complete all GPU access and
discard or rebind descriptors that reference the old view.

The producer hands both render targets over in the `layout` recorded in the
view; currently this is `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` with fragment
shader/read access as the ready scope. Runtime may transition to transfer source
or another compatible use, but before the next Engine access it must restore
the published layout and keep exclusive ownership in `queue_family_index`.
Queue-family transfer is outside this contract and requires an additional API
that coordinates the release and acquire operations in both directions.

No semaphore, fence, event, command buffer, or queue handle is transferred by
`RenderImageView`. When Engine production and Runtime consumption are recorded
in the same command buffer, command order and the documented image barriers are
the synchronization contract. Across submissions, Runtime supplies and owns the
signal/wait chain in both directions. It must wait for Engine production before
reading the image and make its own completion visible before Engine reuses,
resizes, releases, or destroys it.

The view's image, image view, and sampler are required for a valid handoff.
`is_valid()` checks handle and metadata presence, not GPU completion. Destroying
the C++ value performs no Vulkan work.

## Dependency containment

The capabilities header exposes only fixed-width standard types. The core-probe
header exposes standard filesystem, ownership, and string types plus the
installed `ayther::Result` contract. Libretro, SDL, platform loader headers,
threads, and logging remain implementation dependencies and do not propagate
through those modules.

`vulkan_interop.hpp` deliberately exposes Vulkan native types. Therefore the
installed `Ayther::engine` target carries `Vulkan::Vulkan` as a public usage
requirement and `AytherConfig.cmake` resolves Vulkan before importing the Engine
target. Other third-party dependencies remain private unless a future public
declaration truly requires the consumer to see them.
