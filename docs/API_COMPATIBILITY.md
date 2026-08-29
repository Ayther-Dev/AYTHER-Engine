# API and compatibility

**Status:** unstable pre-release interfaces

**Last reviewed:** 2026-08-27

AYTHER Engine currently exposes a Rust crate, typed `cxx` declarations, a flat
C ABI, and an installed C++ engine package. None is stable. Source, binary,
behavior, ownership, and file-format compatibility may change before the first
supported release.

## Surfaces

| Surface | Location | Intended use | Stability |
|---|---|---|---|
| Rust API | `core/src/lib.rs` and modules | in-repository core development | pre-release |
| Typed CXX bridge | `core/src/ffi.rs` | type-oriented Rust/C++ integration | pre-release; not installed as a supported package |
| Flat C ABI | `include/ayther/ayther_core_ffi.h` | C/C++ static-library consumers and hot paths | broad, legacy-compatible, unstable |
| CMake package | `Ayther::core`, `Ayther::engine`, `Ayther::ymfm`, optional `Ayther::vpx` | installed native consumers | pre-release |
| Pack manifest | schema `2` | content metadata | implemented, not frozen |

The Rust definitions are authoritative for exported functions and `#[repr(C)]`
layouts. A shared-type change must update the header and ABI tests atomically.

## Ownership and lifetime

- A pointer returned by a `*_new`, `*_open`, or `*_load` function is owned by
  the caller and must be released by its matching `*_free` or `*_close` call.
- Freeing a null handle is documented as a no-op; other nullability must be
  checked per function.
- Borrowed `const char*` results remain owned by the producing object and may be
  invalidated when that object is mutated or freed. Copy them when persistence
  is required.
- Buffer functions write at most `cap` elements and may return the total amount
  available, which can exceed `cap`. Callers must check each function's unit:
  bytes, elements, frames, or characters.
- No handle is thread-safe unless explicitly documented. The default is one
  owning thread, normally the emulation thread.

Rust cannot validate the provenance of a foreign pointer. Passing dangling,
misaligned, overlapping, or incorrectly sized memory is caller error and can
cause undefined behavior.

## Layout and language rules

Cross-boundary values use `#[repr(C)]`, fixed-width integer types, and explicit
padding where required. The C++ test asserts representative sizes and offsets;
it is not yet an exhaustive ABI oracle. Public declarations must remain valid
in both C11 and C++20.

Do not expose Rust enums, `bool` layout assumptions, `usize`, references,
trait objects, unwinding, or allocator-owned containers directly without a
documented C representation. Panics must not unwind across FFI.

## Release and protocol version contract

| Axis | Current value | Contract |
|---|---:|---|
| AYTHER release | `0.1.0` | shared by Cargo, CMake, vcpkg, SDK, engine validation, and Lua |
| Flat C ABI revision | `6` | independent counter returned by `ayther_core_version()` |
| Pack schema | `2` | manifest metadata grammar understood by the reader/writer |
| Pack container format | `1` | physical `.ay` envelope and root-layout capability |
| Emulator extension ABI | `1.10` | independently negotiated core protocol |
| SDK C API revision | `1` | independent C facade contract |

The release value follows SemVer. During `0.x`, minor versions may break source
or binary compatibility and patch versions remain compatible within the same
minor. Protocol revisions are not SemVer and must not be inferred from the
release value. See [ADR 0002](adr/0002-release-and-protocol-version-contract.md).

Flat C ABI revision 6 adds `ayther_pack_format_supported()` without changing
existing signatures or layouts. Consumers that use only the revision-5 symbol
set remain source-compatible; consumers of the new probe must relink against a
revision-6 core.

## Compatibility work required before stable release

The first supported release must define:

1. a complete ABI policy covering symbol addition/removal, structure growth, calling
   conventions, compiler/runtime compatibility, and deprecation duration;
3. a pack-schema policy with readable/writable ranges and deterministic upgrade
   behavior;
4. script capability negotiation for APIs that evolve independently of the host release;
5. feature negotiation for optional emulator and renderer capabilities;
6. an automated symbol and layout baseline checked in CI.

Until then, pin consumers to a specific source revision and rebuild all native
participants together. Never load a library merely because
`ayther_core_version()` is nonzero or numerically close to an expected value.

## Error handling

The flat ABI uses a mixture of null handles, booleans, numeric counts, sentinel
values, and caller-provided error buffers. Integrators must follow each function
contract rather than assuming one global convention. A future stable surface
should normalize status codes, diagnostics, and ownership without silently
changing existing meanings.

See [Architecture](ARCHITECTURE.md) for component ownership and
[Build, test, and release](BUILD_TEST_RELEASE.md) for the current ABI oracle.
