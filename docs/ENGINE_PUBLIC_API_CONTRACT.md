# Engine public API contract

**Status:** implemented for version and core capability probing; the remaining
Runtime-facing modules are specified destinations and are not public yet.

## Public boundary

The Runtime-facing C++ API lives exclusively under
`include/ayther/engine/` and is included with installed paths such as:

```cpp
#include <ayther/engine/engine.hpp>
#include <ayther/engine/capabilities.hpp>
```

`configuration.hpp`, `output_profile.hpp`, and `vulkan_interop.hpp` are the
required destination modules for the corresponding audited dependencies. They
must not be published as empty placeholders or private-header forwarding
wrappers. Each becomes public only with its complete behavioral contract and
tests.

The existing flat `include/ayther/` SDK headers remain installable during the
0.1.x line so current consumers retain source compatibility. They are not the
Runtime-to-Engine boundary and Runtime must not add new uses of them. A flat
header can move into the new boundary only through a reviewed public module.

Public headers must be self-contained and compile as the first include in an
otherwise empty C++20 translation unit. They may include the C++ standard
library, documented third-party API headers, or other installed
`ayther/engine/` headers. They must not include paths containing `src/`,
`private/`, `internal/`, or `detail/`. A future `detail/` directory requires an
explicit contract, installation rule, and compatibility coverage before use.

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
- `Version::prerelease` refers to immutable Engine-owned static storage and is
  valid until process termination.
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

## Core probing

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
environment probe. Keeping it separate is what makes core probing deterministic
and safe in headless processes.

## Dependency containment

The probing headers expose only `<cstdint>` and `<string_view>`. Vulkan, SDL,
threads, logging, and filesystem facilities remain implementation dependencies
and do not propagate through this module. Future public modules must document
any third-party type they expose and keep CMake dependencies `PRIVATE` unless a
public declaration truly requires the consumer to see them.
