# API and compatibility

**Status:** unstable pre-release interfaces

**Last reviewed:** 2026-08-30

AYTHER Engine currently exposes a Rust crate, typed `cxx` declarations, a flat
C ABI, and an installed C++ engine package. These remain pre-release surfaces.
The installed Runtime-facing C++ declarations have one narrow guarantee:
published `0.1.x` declarations and behavior remain source-compatible through
the `0.1` minor line. No C++ binary compatibility is promised.

## Surfaces

| Surface | Location | Intended use | Stability |
|---|---|---|---|
| Rust API | `core/src/lib.rs` and modules | in-repository core development | pre-release |
| Typed CXX bridge | `core/src/ffi.rs` | type-oriented Rust/C++ integration | pre-release; not installed as a supported package |
| Flat C ABI | `include/ayther/ayther_core_ffi.h` | C/C++ static-library consumers and hot paths | broad, legacy-compatible, unstable |
| CMake package | `Ayther::core`, `Ayther::engine`, `Ayther::ymfm`, optional `Ayther::vpx` | installed native consumers | `0.1.x` source-compatible; no C++ ABI promise |
| Pack manifest | schema `2` | content metadata | implemented, not frozen |

The Rust definitions are authoritative for exported functions and `#[repr(C)]`
layouts. A shared-type change must update the header and ABI tests atomically.

Content identities are compatibility surfaces even when no type layout
changes. Their exact definitions and known-answer tests are documented in
[Pack identity specification](IDENTITY_SPECIFICATION.md).

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

C++ owners of opaque Rust handles use the `unique_handle<T, Free>` adapter so a
successful allocation acquires its matching destructor immediately. Loose raw
owning handles are not an accepted engine-lifetime pattern.

## Layout and language rules

Cross-boundary values use `#[repr(C)]`, fixed-width integer types, and explicit
padding where required. The C++ test asserts representative sizes and offsets;
it is not yet an exhaustive ABI oracle. Public declarations must remain valid
in both C11 and C++20.

Do not expose Rust enums, `bool` layout assumptions, `usize`, references,
trait objects, unwinding, or allocator-owned containers directly without a
documented C representation. Panics must not unwind across FFI.

C++ exceptions must likewise not escape through C entry points. Expected
failures use explicit status or result values; cleanup functions and
destructors are non-throwing.

## Release and protocol version contract

| Axis | Current value | Contract |
|---|---:|---|
| AYTHER release | `0.1.0` | shared by Cargo, CMake, vcpkg, SDK, engine validation, and Lua |
| Flat C ABI revision | `7` | independent counter returned by `ayther_core_version()` |
| Pack schema | `2` | manifest metadata grammar understood by the reader/writer |
| Pack container format | `1` | physical `.ay` envelope and root-layout capability |
| Emulator extension ABI | `1.10` | independently negotiated core protocol |
| SDK C API revision | `1` | independent C facade contract |

The release value follows SemVer. During `0.x`, a new minor version may break
source or binary compatibility; patch versions remain source-compatible within
the same minor. Concretely, the published `0.1.x` C++ contract remains
source-compatible until `0.2.0`. Protocol revisions are not SemVer and must not
be inferred from the release value. See [ADR 0002](adr/0002-release-and-protocol-version-contract.md)
and [ADR 0003](adr/0003-runtime-engine-public-api-ownership.md).

Flat C ABI revision 6 adds `ayther_pack_format_supported()` without changing
existing signatures or layouts. Consumers that use only the revision-5 symbol
set remain source-compatible; consumers of the new probe must relink against a
revision-6 core.

Flat C ABI revision 7 adds `ayther_pack_open_trusted()` so production hosts can
provide a public-key registry explicitly. Existing entry points and layouts are
unchanged; callers of the new symbol must relink against a revision-7 core.

## Compatibility window

The window is the span of other versions a given build interoperates with. It
is stated per axis, because these axes move independently, and it is stated as
what the code actually enforces rather than as an intention.

| Axis | This build accepts | This build produces | Enforced by |
| --- | --- | --- | --- |
| AYTHER release | itself only | `0.1.0` | nothing; there is no cross-release check |
| Flat C ABI | the revision it was compiled against | revision `7` | `ayther_core_version()`, checked by the caller |
| Pack manifest schema | any schema `<= 2` | schema `2` | `archive_vfs.rs`, rejecting a larger declared schema |
| Pack container format | any format `<= 1` | format `1` | `archive_vfs.rs`, rejecting a larger declared format |
| Emulator extension ABI | major `1`, minor `<= 10` | negotiates the highest common minor | `ayther_get_interface` negotiation |
| SDK C API | revision `1` | revision `1` | `AYTHER_SDK_C_API_VERSION` |

Read the pack rows precisely: the bound is one-sided. A pack declaring a
*higher* schema or format is refused, because it may need semantics this build
does not implement; a pack declaring a *lower* one is accepted, and no floor is
enforced. Nothing has ever been dropped, so the absence of a floor has not yet
cost anything — but "we accept every old pack" is currently a consequence of
never having removed anything, not a tested guarantee. The first removal is
what will require a real minimum and the tests to go with it.

The extension ABI is the one axis with genuine negotiation: host and core agree
on a minor within major `1`, and `abi_negociacion` exercises both halves,
including a core that does not speak the extension at all and must still load.

### What the window does not cover

There is no binary-compatibility window across AYTHER releases. A `0.x` minor
version may break source and binary contracts, and no automated symbol or
layout baseline compares one release against the previous one — item 5 of the
list below is exactly that missing check. Until it exists, the practical rule
is unchanged: pin every native participant to one source revision and rebuild
them together. Two artifacts from different commits are not a supported
combination merely because `ayther_core_version()` returns the same number,
since the revision counter moves only on deliberate ABI edits and cannot detect
an accidental layout change.

## Compatibility work required before stable release

The first supported release must define:

1. a complete ABI policy covering symbol addition/removal, structure growth, calling
   conventions, compiler/runtime compatibility, and deprecation duration;
2. a pack-schema policy with readable/writable ranges and deterministic upgrade
   behavior;
3. script capability negotiation for APIs that evolve independently of the host release;
4. feature negotiation for optional emulator and renderer capabilities;
5. an automated symbol and layout baseline checked in CI.

Until then, pin consumers to a specific source revision and rebuild all native
participants together. Never load a library merely because
`ayther_core_version()` is nonzero or numerically close to an expected value.

### Removal, deprecation, and security fixes

For the Runtime-facing C++ API, a public symbol must be documented as
deprecated with its replacement before removal. A symbol published in `0.1.x`
remains until `0.2.0`; `find_package(Ayther 0.1)` enforces the same minor-line
window with `SameMinorVersion`. Security or correctness defects may be repaired
incompatibly when preserving behavior would be unsafe. Such an exception must
be recorded in the release notes and compatibility documentation, and should
produce a comprehensible diagnostic when a safe failure path exists.

Header and library artifacts are versioned as a pair. Source compatibility does
not make mixed commits or mixed build modes valid, and it does not imply a C++
ABI guarantee across compilers, standard libraries, flags, or platforms.

## Error handling

The flat ABI uses a mixture of null handles, booleans, numeric counts, sentinel
values, and caller-provided error buffers. Integrators must follow each function
contract rather than assuming one global convention. A future stable surface
should normalize status codes, diagnostics, and ownership without silently
changing existing meanings.

See [Architecture](ARCHITECTURE.md) for component ownership and
[Build, test, and release](BUILD_TEST_RELEASE.md) for the current ABI oracle.
