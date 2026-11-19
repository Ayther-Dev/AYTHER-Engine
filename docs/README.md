# AYTHER Engine documentation

This directory documents AYTHER Engine: the Rust core, the C++ engine and its
emulation providers, and the contracts Engine owns. It does not duplicate the
guides or contracts owned by AYTHER Runtime, Lab, Play CE, Web, or Tessera.

> [!IMPORTANT]
> Each document states whether what it describes is **implemented**,
> **specified but not present**, or **planned**. [Project status](PROJECT_STATUS.md)
> is the single authoritative statement of current capability; history lives in
> the [changelog](../CHANGELOG.md).

## Start here

| Document | Audience | Purpose |
|---|---|---|
| [Project status](PROJECT_STATUS.md) | Everyone | Current capabilities, versions, validation, and release blockers |
| [Architecture](ARCHITECTURE.md) | Maintainers and integrators | Components, emulation providers, data flow, module map, and trust boundaries |
| [Support matrix](SUPPORT_MATRIX.md) | Integrators and release engineers | Tested platforms, CI configurations, packages, and toolchains |
| [Glossary](GLOSSARY.md) | Everyone | Product and technical vocabulary |

## Integrating Engine

| Document | Audience | Purpose |
|---|---|---|
| [API and compatibility](API_COMPATIBILITY.md) | Rust and C++ integrators | API surfaces, FFI safety, version axes, and compatibility policy |
| [Engine public API contract](ENGINE_PUBLIC_API_CONTRACT.md) | Runtime and Engine integrators | Runtime-facing headers, ownership, lifetime, threading, errors, and probing |
| [Generated public API index](PUBLIC_API_INDEX.md) | C++ consumers | Installed headers and their top-level declarations |
| [C++ API and implementation contracts](CPP_API_REFERENCE.md) | C++ maintainers and integrators | Ownership, lifetime, threading, failure, and subsystem contracts |
| [Tessera native provider](TESSERA_NATIVE.md) | Engine and Runtime integrators | Native provider configuration, Tessera lock, selection API, capabilities, packaging, and parity testing |
| [Emulator extension ABI](EMULATOR_EXTENSION_ABI.md) | Emulator-core and Engine integrators | Libretro extension negotiation, capabilities, regions, snapshots, and subscriptions |
| [Core probe contract](CORE_PROBE_CONTRACT.md) | Engine, Runtime, Lab, and Play CE integrators | Deciding whether a libretro core on disk can be used with AYTHER |
| [Installed package consumer investigation](PACKAGE_CONSUMER_INVESTIGATION.md) | Build and Runtime maintainers | Link-only termination evidence and Debug/Release distinction |

## Content, rendering, and audio

| Document | Audience | Purpose |
|---|---|---|
| [Component model](COMPONENT_MODEL.md) | Engine and tooling developers | Grouping, contextual resolution, animation, audio events, backgrounds, and RAM anchoring |
| [Pack identity specification](IDENTITY_SPECIFICATION.md) | Pack tooling authors and integrators | Bit-exact identity algorithms, known-answer tests, and reimplementation hazards |
| [Audio continuity pack format](AUDIO_CONTINUITY_FORMAT.md) | Pack authors and Engine consumers | `audio_events.toml` schema, units, validation, and rejection policy |
| [Audio observation](AUDIO_OBSERVATION.md) | Runtime and QA integrators | Audio QA observer contract |
| [Render observation](RENDER_OBSERVATION.md) | Runtime and QA integrators | Contract C3: per-frame occurrences, replacements, reasons, and draw outcome |
| [Visual state](VISUAL_STATE.md) | Runtime and replay integrators | Contract C4: export, restore order, sections, and restore codes |
| [Replacement composition rules](REPLACEMENT_COMPOSITION.md) | Renderer maintainers and pack developers | Rules R1-R9: membership, depth, layer order, residency, and pack retirement |
| [Cinematic plane composition](CINEMATIC_PLANE_COMPOSITION.md) | Renderer maintainers | Plane masks, VDP priority, opaque-video policy, and GPU oracle |
| [Widescreen composition](WIDESCREEN.md) | Renderer and pack developers | Widening and gating contract |
| [Pack and security model](PACK_SECURITY_MODEL.md) | Security reviewers and pack tooling authors | Container format, signature and integrity flow, trust registries, and Lua sandbox |

## Building, testing, and releasing

| Document | Audience | Purpose |
|---|---|---|
| [Development environment](DEVELOPMENT_ENVIRONMENT.md) | Contributors | Windows and Linux toolchain installation and local setup |
| [Build, test, and release](BUILD_TEST_RELEASE.md) | Contributors and release engineers | Presets, CTest, packages, CI, and release gates |
| [Coverage policy](COVERAGE.md) | Contributors and release engineers | Per-language baselines, thresholds, exclusions, and reports |
| [Generated dependency graph](DEPENDENCY_GRAPH.md) | Maintainers | Cargo and CMake dependency edges generated from the checkout |
| [Current release go/no-go](RELEASE_GO_NO_GO_CURRENT.md) | Maintainers and release approvers | Current pre-release and stable-release decisions and their evidence |
| [Stability gate snapshot](RELEASE_GO_NO_GO.md) | Maintainers and auditors | Historical go/no-go assessment |
| [Release rollback](RELEASE_ROLLBACK.md) | Release engineers and downstream owners | Withdrawal, consumer notification, immutable tags, and fix-forward |
| [Governance exceptions](GOVERNANCE_EXCEPTIONS.md) | Maintainers, auditors, and release approvers | Time-bounded control deviations and their closure evidence |
| [Audio QA tests](../tests/audio_qa/README.md) | Contributors | Shared-capture audio QA tests (QA-015 to QA-019) |

## Planning and review

| Document | Audience | Purpose |
|---|---|---|
| [Repository roadmap](ROADMAP.md) | Maintainers | Remaining work toward a stable release |
| [C++ engineering review](CPP_ENGINE_REVIEW.md) | Maintainers | Findings, design patterns, corrections, and performance opportunities |
| [Legal and distribution boundaries](LEGAL_AND_DISTRIBUTION.md) | Distributors, pack authors, and maintainers | BYOR/BYOC rules, license scope, content rights, trademarks, and notices |

## Decisions

Architecture decision records explain why a constraint exists; they do not
describe current status.

- [ADR 0001: Keep core policy separate from native orchestration](adr/0001-core-and-engine-component-boundary.md)
- [ADR 0002: Release and protocol version contract](adr/0002-release-and-protocol-version-contract.md)
- [ADR 0003: Runtime-to-Engine public API ownership](adr/0003-runtime-engine-public-api-ownership.md)

## Repository policies

- [Contributing](../CONTRIBUTING.md)
- [Security policy](../SECURITY.md)
- [Changelog](../CHANGELOG.md)
- [MPL-2.0 license](../LICENSE)
- [Third-party notices](../NOTICE.md)

## Documentation maintenance rules

1. The current checkout is the source of truth for implementation; a document
   never presents design material as implemented until the code and tests exist.
2. Each topic has one owning document. Other documents link to it instead of
   repeating it.
3. Reference documents describe the present; history belongs in the changelog,
   and dated evidence belongs in status and release records.
4. Public API, ABI, manifest, signing-policy, dependency, and license changes
   update their owning document in the same change.
5. Relative links stay within this repository. Cross-repository references use
   product names or `https` URLs, never local checkout paths.
6. Generated documents (`DEPENDENCY_GRAPH.md`, `PUBLIC_API_INDEX.md`,
   `NOTICE.md`) are updated through their generators, never by hand.
7. `pwsh tools/check_doc_references.ps1` must pass before a change merges.
