# ADR 0003: Runtime-to-Engine public API ownership

**Status:** accepted

**Date:** 2026-08-31

## Context

AYTHER Runtime needs a narrow way to identify the linked Engine artifact and
the features compiled into it. Reading Engine's private headers or repeating
build macros in Runtime would let the consumer describe itself instead of the
library it actually loaded. Commit `56b39d9` introduced an installed C++20
surface under `include/ayther/engine/`, but its ownership, compatibility window,
and authorized consumers had to be explicit before that surface could grow.

## Decision

The API is retained. AYTHER Engine owns and maintains the Runtime-to-Engine
public API, its installed headers, its out-of-line implementation, and its
contract tests. Changes require review by Engine maintainers and must update
the implementation, package inventory, compatibility documentation, and
installed-consumer tests atomically.

The authorized consumers during the `0.1.x` pre-release line are:

1. AYTHER Runtime, as the production game-session host;
2. AYTHER Play, when it hosts Engine through the same reviewed modules;
3. this repository's header, package, and conformance consumers.

AYTHER Lab, SDK tooling, and third-party applications are not authorized to
expand this boundary implicitly. A new consumer or public module requires an
Engine review and a contract covering ownership, lifetime, threading, errors,
installation, and compatibility.

The `0.1.x` line guarantees source compatibility for published declarations
and documented behavior. A symbol is documented as deprecated with its
replacement before removal and remains present until `0.2.0`. A security or
correctness defect may require an incompatible repair sooner, but the exception
must be documented and must fail with a diagnostic where continued execution
would be unsafe. No C++ binary compatibility is promised: consumers rebuild
against the headers and library from the same package.

`Version::prerelease` is removed before the API is published. The build has no
artifact-owned prerelease source, so an always-empty field would promise a
release-candidate distinction Engine cannot make reliably. It can return only
through a later additive contract backed by stable artifact metadata and tests.

## Consequences

- `version()` and `probe_capabilities()` remain out-of-line, total, concurrent,
  allocation-free, side-effect-free `noexcept` queries.
- Build capability is kept separate from device availability and successful
  initialization.
- `cmake/AytherPublicHeaders.txt` is the single installed-header inventory for
  CMake checks and the generated public index.
- `find_package(Ayther 0.1)` uses `SameMinorVersion`, matching the `0.1.x`
  source-compatibility window without claiming C++ ABI compatibility.
- Runtime does not gain access to Engine internals merely because they exist in
  the source tree.
