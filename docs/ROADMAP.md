# Engineering roadmap

**Status:** directional, not a release promise

**Last reviewed:** 2026-08-27

This roadmap orders work by dependency and risk. Dates and product commitments
are intentionally omitted; priorities may change as implementation evidence
changes.

## 1. Stabilize the core contract

- keep the accepted release/protocol version contract enforced across Cargo,
  CMake, vcpkg, SDK headers, engine validation, Lua, ABI, and pack schema;
- define the supported Rust, CXX, and C surfaces and remove accidental exports;
- make ABI layout and symbol checks exhaustive;
- normalize diagnostics and document every ownership/lifetime contract;
- add fuzzing and bounded-resource tests for parsers, patches, scripting, and
  foreign inputs.

Exit criterion: deterministic core behavior and one version policy are enforced
in CI, with no undocumented public symbols.

## 2. Harden pack trust

- implement canonical logical paths and archive resource ceilings;
- separate development signing from production trust configuration;
- add key registry, identity scope, rotation, revocation, and compromise
  procedures;
- validate signature, integrity, compatibility, and authorization as distinct
  decisions;
- publish adversarial fixtures and conformance tests.

Exit criterion: a security review can reproduce every acceptance and rejection
decision without development keys.

## 3. Integrate the native engine layer

- complete the existing `AytherSession` facade and explicit component ownership;
- integrate a policy-controlled libretro host;
- integrate SDL audio and Vulkan rendering behind testable interfaces;
- complete rewind, recording, and replacement scheduling with deterministic clocks;
- maintain the installed `Ayther::engine` package and its deliberately small,
  enforced public-header allowlist.

Exit criterion: a headless reference session runs legally distributable fixtures
end to end, and native resources have leak and lifetime oracles.

## 4. Complete verification and packaging

- exercise Windows and Linux clean builds in CI;
- separate CPU, GPU, long-running, and fixture-dependent test groups;
- test installation and consumption outside the source tree;
- generate checksums, software bills of materials, notices, and provenance;
- establish signed release candidates, rollback, and support procedures.

Exit criterion: every supported artifact is reproducible, auditable, and
consumable without local-path assumptions.

## 5. Declare supported scope

- publish platform, compiler, GPU, emulator-core, and pack-schema matrices;
- publish deprecation and security-support windows;
- verify documentation examples against released artifacts;
- run at least one release candidate through external integration review.

Only after these criteria should the project consider a stable release. Work on
additional systems or advanced presentation features must not bypass the
security, legal, compatibility, and reproducibility gates above.
