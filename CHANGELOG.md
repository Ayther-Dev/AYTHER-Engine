# Changelog

All notable changes to AYTHER Engine will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project will adhere to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Security

- Added explicit production pack trust registries with Ed25519 signer identity,
  validity windows, revocation, game scope, and release-mode rejection of the
  public development key.
- Added reproducible tag releases with SPDX SBOMs, SHA-256 checksums, keyless
  Sigstore bundles, SLSA provenance, and signed SBOM attestations.
- Pack builders, readers, and validators now share canonical logical-path
  enforcement, duplicate-name rejection, bounded metadata reads, archive and
  entry size ceilings, total expansion limits, and ZIP compression-ratio
  defenses.
- Required pull-request jobs now run AddressSanitizer and
  UndefinedBehaviorSanitizer over the CPU test suite, a pinned `cargo-audit`
  against the locked dependency graph, and a short fuzz smoke over the pack,
  decoder, and FFI targets, keeping any crash as an artifact.

### Added

- Initial repository documentation structure.
- Contribution and security policies, attributes, and ignore rules.
- Reproducible Windows and Linux development environment setup guide.
- Root Rust workspace manifest and minimal `ayther_core` crate scaffold.
- Architecture, project-status, API/compatibility, pack-security, build/release,
  legal, roadmap, glossary, and decision records.
- Root CMake build for `ayther_core`, the partial `Ayther::core` install package,
  a public C11/C++20 header, and a headless cross-language ABI test.
- Direct third-party dependency inventory and release notice requirements.
- Native release presets for the complete engine (`*-release-engine`) and for
  the engine with VP9 (`*-release-engine-vpx`), each with build and test
  presets.
- A core-only out-of-tree package consumer, so the core artifact is proved
  consumable without the engine's native dependency surface.
- `tools/check_release_payload.ps1`, which verifies an unpacked artifact holds
  exactly the payload its name advertises, and proves the core package does not
  contain the engine.
- `tools/gen_release_notes.ps1` and `tools/verify_release_artifact.ps1`, which
  state the artifact scope on the release page and re-verify a published
  archive from a clean checkout.
- Separate informational Rust and C++ coverage reports on every pull request,
  and `clang-tidy` over the translation units a change touches.

### Changed

- Cargo, CMake, vcpkg, SDK, engine validation, and Lua now share the `0.1.0`
  release version; ABI and pack-schema values are explicitly independent
  protocol revisions.
- A tag now publishes three clearly named artifact families per platform --
  `ayther-core`, `ayther-engine`, and `ayther-engine-vpx` -- instead of one
  core-only archive named after the engine. Each is built, tested, unpacked,
  payload-checked, and consumed from its own archive before publication.
- The release version contract accepts pre-release tags such as `v0.1.0-rc.1`.
- First-party C++ compiles with warnings as errors; vendored ymfm and the
  `tools/` probes remain explicitly exempt.

### Deprecated

- None.

### Removed

- None.

### Fixed

- None.

<!-- Comparison links will be added with the first published tag. -->
