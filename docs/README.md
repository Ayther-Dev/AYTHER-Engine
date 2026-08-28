# AYTHER Engine documentation

This directory documents AYTHER Engine. It covers
the Rust core, the C++ engine boundary, and the cross-cutting contracts
owned by the motor. It does not duplicate the user guides or product contracts
owned by AYTHER Runtime, SDK, Play, Hub, or Lab.

> [!IMPORTANT]
> Documents describe one of three states: **implemented here**, **specified but
> not present**, or **planned**. The status marker near
> each document title wins over older design material.

## Start here

| Document | Audience | Purpose |
|---|---|---|
| [Project status](PROJECT_STATUS.md) | Everyone | Current capabilities, missing repository areas, validation evidence, and release blockers |
| [Architecture](ARCHITECTURE.md) | Maintainers and integrators | System context, internal layers, data flow, module ownership, and failure boundaries |
| [API and compatibility](API_COMPATIBILITY.md) | Rust/C++ integrators | API surfaces, FFI safety, version axes, and compatibility policy |
| [Generated public API index](PUBLIC_API_INDEX.md) | C++ consumers | Installed headers and their top-level declarations |
| [C++ API and implementation contracts](CPP_API_REFERENCE.md) | C++ maintainers and integrators | Ownership, lifetime, threading, failure, and subsystem contracts |
| [C++ engineering review](CPP_ENGINE_REVIEW.md) | Maintainers | Critical findings, hardcoding, design patterns, corrections, and performance opportunities |
| [Pack and security model](PACK_SECURITY_MODEL.md) | Security reviewers and pack tooling authors | Container format, signature/integrity flow, Lua sandbox, and hardening gaps |
| [Build, test, and release](BUILD_TEST_RELEASE.md) | Contributors and release engineers | Commands that work now, intended CMake flow, CI expectations, and release gates |
| [Development environment](DEVELOPMENT_ENVIRONMENT.md) | Contributors | Windows/Linux toolchain installation and local setup |
| [Legal and distribution boundaries](LEGAL_AND_DISTRIBUTION.md) | Distributors, pack authors, and maintainers | BYOR/BYOC rules, license scope, content rights, trademarks, and third-party notices |
| [Repository roadmap](ROADMAP.md) | Maintainers | Evidence-based development sequence for this repository |
| [Glossary](GLOSSARY.md) | Everyone | Canonical product and technical vocabulary |

## Decisions

Accepted architecture decisions live in `docs/adr/`. They explain why a
constraint exists; they are not substitutes for current status or user-facing
instructions.

## Repository policies

- [Contributing](../CONTRIBUTING.md)
- [Security policy](../SECURITY.md)
- [Changelog](../CHANGELOG.md)
- [MPL-2.0 license](../LICENSE)
- [Third-party notices](../NOTICE.md)

## Documentation maintenance rules

1. The current checkout is the primary source of truth for implementation.
2. Design material must not be presented as implemented until the corresponding
   files and tests exist here.
3. Every command shown as runnable must be exercised from a clean standalone
   checkout before release.
4. Public API, ABI, manifest, signing-policy, dependency, and license changes
   update their owning document in the same change.
5. Relative links must stay within this repository. Cross-repository references
   use stable product or artifact names, never local checkout paths.
