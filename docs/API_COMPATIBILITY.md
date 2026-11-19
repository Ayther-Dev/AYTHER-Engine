# API and compatibility

**Status:** pre-release. Engine `0.1.0`; the latest prerelease tag is listed in
[CHANGELOG](../CHANGELOG.md) and on the GitHub releases page.

AYTHER Engine exposes a Rust crate, typed `cxx` declarations, a flat C ABI, and
an installed C++ package. All of them are pre-release surfaces. The installed
Runtime-facing C++ declarations carry one narrow guarantee: published `0.1.x`
declarations and documented behavior stay source-compatible through the `0.1`
minor line. No C++ binary compatibility is promised.

This page is the reference for versions and compatibility windows. Ownership,
threading and error rules for the C++ API live in the
[Engine public API contract](ENGINE_PUBLIC_API_CONTRACT.md) and the
[C++ API reference](CPP_API_REFERENCE.md).

## Surfaces

| Surface | Location | Intended use | Stability |
|---|---|---|---|
| Rust API | `core/src/lib.rs` and modules | in-repository core development | pre-release |
| Typed CXX bridge | `core/src/ffi.rs` | type-oriented Rust/C++ integration | pre-release; not installed as a supported package |
| Flat C ABI | `include/ayther/ayther_core_ffi.h` | C/C++ static-library consumers and hot paths | broad, legacy-compatible, unstable |
| CMake package | `Ayther::core`, `Ayther::engine`, `Ayther::ymfm`, optional `Ayther::vpx` | installed native consumers | `0.1.x` source-compatible; no C++ ABI promise |
| Native emulation provider | `include/ayther/engine/native_emulation.hpp`, `AytherSession::Config::provider` | hosts that select the Tessera core | implemented, optional at build time, pre-release |
| Libretro extension ABI | `include/ayther/libretro_host/ayther_api.h` | AYTHER-aware libretro cores | negotiated, see [Emulator extension ABI](EMULATOR_EXTENSION_ABI.md) |
| Core probe protocol | `ayther_core_probe`, `engine/core_probe.hpp` | deciding whether a libretro core is usable | protocol version `2`, see [Core probe contract](CORE_PROBE_CONTRACT.md) |
| Pack manifest | schema `2` | content metadata | implemented, not frozen |

The installed header inventory is `cmake/AytherPublicHeaders.txt`; the
generated [Public API index](PUBLIC_API_INDEX.md) lists it. Appearing in that
inventory does not by itself make a header stable.

The Rust definitions are authoritative for exported functions and `#[repr(C)]`
layouts. A shared-type change must update the header and ABI tests atomically.

Content identities are compatibility surfaces even when no type layout
changes. Their definitions and known-answer tests are in the
[Pack identity specification](IDENTITY_SPECIFICATION.md).

## Release and protocol versions

| Axis | Current value | Source of truth |
|---|---:|---|
| AYTHER release | `0.1.0` | Cargo workspace, CMake `PROJECT_VERSION`, `include/ayther/ayther_version.h` |
| Flat C ABI revision | `9` | `CORE_C_ABI_REVISION` in `core/src/lib.rs`, returned by `ayther_core_version()` |
| Pack manifest schema | `2` | `AYTHER_PACK_MANIFEST_SCHEMA` |
| Pack container format | `1` | `AYTHER_PACK_FORMAT` |
| Libretro extension ABI | `1.10` | `AYTHER_ABI_VERSION_LATEST` in `ayther_api.h` |
| Tessera C API (minimum) | `1.1` | `cmake/AytherTesseraLock.cmake`; package pinned by `dependencies/tessera.lock.json` |
| Core probe protocol | `2` | `protocol_version` in the `AYTHER_STATUS` line |
| SDK C API revision | `1` | `AYTHER_SDK_C_API_VERSION` |
| Take recording (`.arp`) | `11` | `src/ayther_recording.cpp` |

The release value follows SemVer. During `0.x`, a new minor version may break
source or binary compatibility; patch versions stay source-compatible within
the same minor. Protocol revisions are not SemVer and must not be inferred from
the release value. See [ADR 0002](adr/0002-release-and-protocol-version-contract.md)
and [ADR 0003](adr/0003-runtime-engine-public-api-ownership.md).

Prerelease tags (`v0.1.0-rc.N`) are distribution identity. Artifact names and
provenance keep the full tag; `engine::version()` reports the numeric `0.1.0`.

### Flat C ABI revisions

Revisions 6 to 9 are additive: no existing signature or layout changed. A
consumer that calls a symbol must link a core of at least the revision that
introduced it.

| Revision | Adds |
|---:|---|
| 6 | `ayther_pack_format_supported()` |
| 7 | `ayther_pack_open_trusted()` for explicit production trust registries |
| 8 | `ayther_sprite_pattern_hash()` |
| 9 | `ayther_pack_verify_all()`; some revision-8 builds also export it, so only revision 9 guarantees it |

### Take recording format

Version 10 stores the take's stable `take_id`; version 11 appends the emulator
provenance (`emulator_id`, `emulator_version`, `determinism_contract`), exposed
as `AYTHER_RECORDING_PROVENANCE`. Files up to version 10 load as `gpgx` with an
empty version and contract 1. A reader rejects a file newer than its own
version instead of guessing. `AytherRecording` grows with the format, so a
frontend must rebuild against the headers of the Engine it links and may guard
with `AytherRecording::abi_size()`.

## Resolution tier values

The C++ `PackRenderTier` enum, manifest `[tiers].included` entries, and the Rust
selector share these values: `0=HD`, `1=Full HD`, `2=2K`, `3=4K`, `4=8K`. Height
selection rounds up at 720, 1080, 1440, and 2160 pixels, then falls back
according to pack contents. Packs that used an older four-tier numbering
(`2=4K`, `3=8K`) must be rebuilt; no manifest field distinguishes them. Flat
packs are unaffected.

## Ownership and lifetime

- A pointer returned by a `*_new`, `*_open`, or `*_load` function is owned by
  the caller and must be released by its matching `*_free` or `*_close` call.
- Freeing a null handle is a no-op; other nullability must be checked per
  function.
- Borrowed `const char*` results remain owned by the producing object and may be
  invalidated when that object is mutated or freed. Copy them when persistence
  is required.
- Buffer functions write at most `cap` elements and may return the total amount
  available, which can exceed `cap`. Check each function's unit: bytes,
  elements, frames, or characters.
- No handle is thread-safe unless explicitly documented. The default is one
  owning thread, normally the emulation thread.

Rust cannot validate the provenance of a foreign pointer. Passing dangling,
misaligned, overlapping, or incorrectly sized memory is caller error and can
cause undefined behavior.

C++ owners of opaque Rust handles use the `unique_handle<T, Free>` adapter so a
successful allocation acquires its matching destructor immediately. Loose raw
owning handles are not an accepted engine-lifetime pattern.

## Layout and language rules

Cross-boundary values use `#[repr(C)]`, fixed-width integer types, and explicit
padding where required. The C++ tests assert representative sizes and offsets;
they are not an exhaustive ABI oracle. Public C declarations must remain valid
in both C11 and C++20.

Do not expose Rust enums, `bool` layout assumptions, `usize`, references,
trait objects, unwinding, or allocator-owned containers directly without a
documented C representation. Panics must not unwind across FFI.

C++ exceptions must likewise not escape through C entry points. Expected
failures use explicit status or result values; cleanup functions and
destructors are non-throwing.

## Compatibility window

The window is the span of other versions a given build interoperates with. It
is stated per axis, as what the code enforces.

| Axis | This build accepts | This build produces | Enforced by |
| --- | --- | --- | --- |
| AYTHER release | itself only | `0.1.0` | no cross-release check |
| Flat C ABI | the revision it was compiled against | revision `9` | `ayther_core_version()`, checked by the caller |
| Pack manifest schema | any schema `<= 2` | schema `2` | `archive_vfs.rs`, rejecting a larger declared schema |
| Pack container format | any format `<= 1` | format `1` | `archive_vfs.rs`, rejecting a larger declared format |
| Libretro extension ABI | major `1`, minor `<= 10` | negotiates the highest common minor | `ayther_get_interface` negotiation |
| Tessera C API | major `1`, minor `>= 1` | n/a | configure-time check plus `tessera_api_query_v1` at session creation |
| Take recording | versions `2` to `11` | version `11` | `AytherRecording` reader |
| SDK C API | revision `1` | revision `1` | `AYTHER_SDK_C_API_VERSION` |

The pack bounds are one-sided. A pack declaring a higher schema or format is
refused; a lower one is accepted and no floor is enforced. Accepting every old
pack is a consequence of never having removed anything, not a tested
guarantee. The first removal must add a minimum and tests for it.

When Engine is built with `AYTHER_ENABLE_TESSERA=ON`, configuration also
verifies that the installed Tessera package matches
`dependencies/tessera.lock.json` unless `AYTHER_TESSERA_ALLOW_UNLOCKED=ON` is
set for local development. See [Tessera native provider](TESSERA_NATIVE.md).

### What the window does not cover

There is no binary-compatibility window across AYTHER releases. No automated
symbol or layout baseline compares one release against the previous one. Pin
every native participant to one source revision and rebuild them together. Two
artifacts from different commits are not a supported combination merely
because `ayther_core_version()` returns the same number: the counter moves only
on deliberate ABI edits and cannot detect an accidental layout change.

## Removal, deprecation, and security fixes

For the Runtime-facing C++ API, a public symbol is documented as deprecated
with its replacement before removal. A symbol published in `0.1.x` remains
until `0.2.0`; `find_package(Ayther 0.1)` enforces the same minor-line window
with `SameMinorVersion`. Security or correctness defects may be repaired
incompatibly when preserving behavior would be unsafe. Such an exception is
recorded in the release notes and here, and should produce a comprehensible
diagnostic when a safe failure path exists.

Header and library artifacts are versioned as a pair. Source compatibility does
not make mixed commits or mixed build modes valid. On MSVC, a Debug consumer
cannot link a Release Engine library (`_ITERATOR_DEBUG_LEVEL` mismatch); build
both in the same mode.

## Work required before a stable release

1. An ABI policy covering symbol addition and removal, structure growth,
   calling conventions, compiler/runtime compatibility, and deprecation length.
2. A pack-schema policy with readable/writable ranges and deterministic upgrade
   behavior.
3. Script capability negotiation for APIs that evolve independently of the
   host release.
4. Feature negotiation for optional emulator and renderer capabilities.
5. An automated symbol and layout baseline checked in CI.

Never load a library merely because `ayther_core_version()` is nonzero or
numerically close to an expected value.

## Error handling

The flat ABI uses a mixture of null handles, booleans, numeric counts, sentinel
values, and caller-provided error buffers. Follow each function's contract
rather than assuming one global convention. The C++ API uses `ayther::Result`
for libretro-provider and pack operations and `engine::NativeResult` for the
native provider; see [Engine public API contract](ENGINE_PUBLIC_API_CONTRACT.md#errors).
A future stable surface should normalize status codes, diagnostics, and
ownership without silently changing existing meanings.

See [Architecture](ARCHITECTURE.md) for component ownership and
[Build, test, and release](BUILD_TEST_RELEASE.md) for the current ABI tests.
