# ADR 0002: Release and protocol version contract

**Status:** accepted

**Date:** 2026-08-28

## Context

AYTHER inherited several values that were all described as versions: Cargo and
CMake reported `0.1.0`, pack validation reported `0.9.0`, Lua reported `0.5.0`,
the flat C ABI returned `5`, and the emulator extension negotiated `1.10`.
Mixing release identity with protocol revisions made compatibility decisions
ambiguous and allowed documentation, headers, and runtime reports to drift.

## Decision

AYTHER has one SemVer release version. Cargo workspace metadata is the Rust
source of that value; CMake, vcpkg, the installed native headers, the linked SDK,
pack compatibility checks, and `ayther.version()` must report the same value.
During the `0.x` series, a minor release may change source or binary contracts;
patch releases remain compatible within the same minor release.

Protocol and format revisions remain independent:

- `AYTHER_CORE_C_ABI_REVISION` / `ayther_core_version()` identifies the legacy
  flat C ABI and is not a SemVer number;
- `AYTHER_PACK_MANIFEST_SCHEMA` identifies the `.ay` manifest grammar;
- `AYTHER_PACK_FORMAT` / `ayther_pack_format_supported()` identifies the
  physical `.ay` container and root layout;
- the AYTHER-aware libretro extension negotiates its own major/minor ABI;
- `AYTHER_SDK_C_API_VERSION` identifies the C facade contract.

A release-version bump must update Cargo, CMake, vcpkg, and
`include/ayther/ayther_version.h` in one change. Cross-language tests compare
the runtime Rust value with the C/C++ header and CMake project version. CMake
rejects a vcpkg manifest whose version differs from `PROJECT_VERSION`.

Protocol revisions change only when their respective contracts change. Pack
schema and container format are separate axes: schema versions metadata fields
and tables, while format versions the physical envelope. Both default to 1 when
absent and reject declarations newer than the reader, with container format
validated first. A bump to either must define its readable/writable
compatibility range and migration behavior. An ABI bump must include
symbol/layout tests and a deprecation or migration note.

Flat C ABI revision 6 adds the
`ayther_pack_format_supported()` capability probe. The change is additive and
does not alter any existing function signature or C-compatible layout; consumers
built against revision 5 continue to use the unchanged subset, while consumers
that call the new symbol must relink against revision 6 or later.

## Consequences

Consumers can display one AYTHER release version without guessing. Compatibility
logic still uses the narrower protocol revision relevant to a boundary. A
release cannot configure or pass the ABI suite when its declared versions drift.
