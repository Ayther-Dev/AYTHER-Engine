# Changelog

All notable changes to AYTHER Engine will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project will adhere to [Semantic Versioning](https://semver.org/).

## [Unreleased]

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

### Changed

- Cargo, CMake, vcpkg, SDK, engine validation, and Lua now share the `0.1.0`
  release version; ABI and pack-schema values are explicitly independent
  protocol revisions.

### Deprecated

- None.

### Removed

- None.

### Fixed

- None.

### Security

- None.

<!-- Comparison links will be added with the first published tag. -->
