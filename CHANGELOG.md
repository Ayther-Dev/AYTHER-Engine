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
- Decoded-resource ceilings: an image's declared size is checked from its header
  before any pixel buffer is allocated, video dimensions are checked before
  libvpx is configured, decoded audio is capped per asset, and the Lua VM now
  has a 64 MiB memory ceiling alongside its existing instruction budget. A
  41-byte PNG declaring 12000x12000 passes every container check and is refused
  here.
- Key rotation, revocation, and per-game scope are pinned by fixtures: a two-key
  registry driven through an explicit clock, the same transitions end to end
  with real signed packs, and the whole set repeated through
  `ayther_pack_open_trusted` so the policy is proved at the FFI boundary too.

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
- `ayther::RuntimeOptions`: every `AYTHER_*` environment option is read once,
  validated, and injected into subsystems as an immutable value.
- `ayther::log`: structured records carrying severity, component, a stable event
  id, and typed fields, dispatched to a sink the frontend installs. `log.h`
  joins the installed public header surface so a host can install one.
- `session::PackRuntime`: pack activation, the trust registry, profiles,
  declared systems, asset lookup, and validation, unit tested without an
  emulator core, an audio device, or a Vulkan context.
- `tools/test_core`: a deterministic libretro core, owned here and built from
  source on both platforms, that speaks the AYTHER ABI. Built twice, with and
  without the extension entry point, so both halves of the negotiation run.
- `tools/e2e_determinism`: the whole pipeline -- synthetic ROM, test core,
  signed pack, scripted inputs -- hashed run-to-run and against pinned
  cross-platform constants.
- `tools/check_gpu_matrix.ps1`: records which device and driver answered and
  fails when the GPU suite was skipped rather than executed.
- `tools/check_rc_consumer.ps1` and `tools/make_test_pack`: install a release
  candidate outside the source tree, drive it as a frontend does with a trusted
  pack, and refuse a report containing repository paths.
- `AytherSession::Config::trust_registry`, without which a release build could
  open no pack at all.

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
- Engine code no longer writes to `stderr` or `stdout` directly. All 173 call
  sites now emit structured log records; the only console writer left is the
  central fallback used when no sink is installed.
- A malformed `AYTHER_*` option is reported and ignored instead of being parsed
  by `atoi`, which could not distinguish a typo from a deliberate zero.
- All five ABI oracles now execute instead of reporting CTest's skip code: the
  core they needed is built from source rather than fetched as an ignored
  binary.

### Deprecated

- None.

### Removed

- None.

### Fixed

- `ayther_pack_profile_field(pack, i, "name")` returned nothing. The Rust side
  still matched the pre-rebrand spelling `"nombre"` while the C header and every
  caller used `"name"`, so a pack's profile display name never reached a C++
  consumer. Both spellings are accepted now.
- A trust-registry key could not be scoped to a real game. `valid_game_scope`
  rejected `:`, but the canonical game id is `crc32:XXXXXXXX`, so any registry
  naming an actual title was malformed and the only usable scope was `"*"` --
  per-game delegation that refused every game it was pointed at.
- A release build could open no pack whatsoever through `AytherSession`: an
  unsigned pack is refused, the development key is refused in an optimized
  build, and `Config` had no way to name a production trust registry.

<!-- Comparison links will be added with the first published tag. -->
