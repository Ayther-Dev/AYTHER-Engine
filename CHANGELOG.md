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


- `AytherSession::set_audio_events` restores the events of an earlier
  `analyze_audio_events` of the same take without replaying it, so a frontend
  can cache them next to the take; `AytherSession::kAudioEventAlgo` versions
  the detector output for such caches.
- Sequence substitutions in a pack can now carry their segmentation step
  separately from their window, through an optional `span` key on each
  `[[event]]` of `audio_events.toml` and a matching `span_frames` field on
  `AytherEventSub`. The window says how long the replacement claims and sounds;
  the step says how soon a new occurrence of the trigger means a NEW pass. An
  absent or zero `span` keeps segmenting by the window, so every pack baked
  before this key reads and sounds exactly as it did, and an older engine
  ignores the key. The field occupies the alignment hole that already sat
  before `match_instrument`, so no offset moves, the struct size is unchanged
  and the C ABI revision stays at 7; `event_sub_layout_tests` pins that.
- A public move-only `ayther::engine::CoreProbe` facade that owns temporary
  Libretro library loading, copies core metadata, returns platform diagnostics,
  and serializes the result without exposing loader or Libretro headers.
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
- Separate Rust and C++ coverage reports on every pull request, and
  `clang-tidy` over the translation units a change touches.
- `tools/check_coverage.py`: a coverage gate enforcing a total floor and a
  changed-line floor per language, reporting the exact files and line ranges
  left uncovered. Thresholds live in `.github/coverage-thresholds.json` and the
  measured baselines are recorded in `docs/COVERAGE.md`.
- `windows-native-coverage`: LLVM source coverage on Windows, so the C++
  baseline is reproducible off the CI runner.
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
- `docs/SUPPORT_MATRIX.md`: the published support matrix, separating what was
  verified on a developer machine from what only CI covers and what nothing has
  measured, with the operating systems, architectures, compilers, GPU backend,
  and VPX configurations behind each claim.
- A compatibility window per axis in `docs/API_COMPATIBILITY.md`, stating what
  each build accepts and produces for the release, flat C ABI, pack manifest
  schema, pack container format, extension ABI, and SDK C API.

### Changed


- Cargo, CMake, vcpkg, SDK, engine validation, and Lua now share the `0.1.0`
  release version; ABI and pack-schema values are explicitly independent
  protocol revisions.
- A tag now publishes three clearly named artifact families per platform --
  `ayther-core`, `ayther-engine`, and `ayther-engine-vpx` -- instead of one
  core-only archive named after the engine. Each is built, tested, unpacked,
  payload-checked, and consumed from its own archive before publication.
- The release version contract accepts pre-release tags such as `v0.1.0-rc.1`.
- First-party C++ compiles with warnings as errors, with no target-level
  opt-out for our own code. The only two exempt targets are `ayther_ymfm`
  (vendored) and `ayther_cxx` (generated by cxxbridge), both documented at
  their definition.
- Deprecated platform APIs are wrapped rather than suppressed: `tools/` now
  goes through `ayther::file_open` and `ayther::env_get` like the engine does.
  `_CRT_SECURE_NO_WARNINGS` is defined nowhere in the repository.
- `ayther_diagnostic.h`: portable three-line windows for the one suppression
  that is legitimate -- the ABI parity oracles, which must call the deprecated
  accessors they exist to compare against.
- Engine code no longer writes to `stderr` or `stdout` directly. All 173 call
  sites now emit structured log records; the only console writer left is the
  central fallback used when no sink is installed.
- A malformed `AYTHER_*` option is reported and ignored instead of being parsed
  by `atoi`, which could not distinguish a typo from a deliberate zero.
- All five ABI oracles now execute instead of reporting CTest's skip code: the
  core they needed is built from source rather than fetched as an ignored
  binary.
- Coverage is a gate rather than an informational artifact: a total below the
  floor, or new code below the changed-line floor, fails the build.
- The end-to-end determinism oracle pins only the frames and audio hashes.
  Events and work RAM are compared run-to-run but not pinned: both were
  measured to change with the optimisation level, so pinning them left the
  `-O0` coverage job permanently red.
- A pose's `pose_key` now includes each member's flip relative to member 0
  when the members carry mixed flips. Two poses with the same sprites and
  layout that differ only in a member's relative flip get distinct keys, so the
  in-between between them fires. Poses with uniform, absent, or single-member
  flips keep their previous key byte for byte; only mixed-flip poses change.
  Packs store in-betweens by asset rather than by `pose_key`, so no baked pack
  breaks.
- A mirrored instance of a pose with authored flips and a symmetric layout,
  where a mirror arrangement coincides with the captured positions, is now
  emitted with its mirror bits instead of in the captured face. An HD asset
  drawn facing the opposite way in such a pose now renders inverted and is
  corrected with the asset's own horizontal flip. The same applies when such a
  pose's stored flips are shifted by a global mirror from the captured face.
  Single-sprite poses keep the previous behavior.

### Deprecated

- None.

### Removed

- None.

### Fixed


- The caches derived from the audio events (sequence anchors, one-shot
  timbres) were keyed by the event count alone, so analysing another take
  with the same count kept the previous ones; any change of the events now
  rebuilds them.
- A Sequence played from a baked pack could never re-anchor on its own period.
  The pack had no way to express the segmentation step, so `audio_event_seq_view`
  left it at zero and the policy fell back to the window: a Sequence whose HD is
  longer than its phrase re-anchored at most once per HD length, and a phrase
  whose last note rings past the loop point swallowed the pass starting there.
  Only the live and replay paths ever carried a real step. Reading the new
  `span` key closes that gap; the authoring tool has to re-export the pack for
  the correction to reach an already-baked one.
- Two poses with the same sprites and layout that differ only in the flip of a
  member resolved by absolute flip agreement alone. Flips stored shifted by a
  global mirror, as older authoring backfills saved them from a mirrored face,
  lost against the other variant, and in symmetric layouts the mirror
  arrangement was discarded by comparing positions only, so a mirrored
  instance could pick the wrong variant. Phase 2 now prefers the pose whose
  relative member flips agree with the observed ones, then the arrangement
  whose absolute flips agree. A single pose is still recognized in the other
  variant. A pose without flips counts as full relative agreement, and a single
  hit carries no relative evidence, so against a pose without flips only
  absolute agreement decides, as before, and the phase-1 order breaks the
  remaining ties. The one change in such a contest: a pose with flips whose
  relative flips disagree with the observed ones now yields to a pose without
  flips at equal hits, where before it could win on partial absolute
  agreement.
- Plane sets were tried in the container's bucket order, so a one-member set
  could claim a cell before the larger set containing it was tried, and the
  larger set never matched (Golden Axe: assigning HD to the one-tile
  "Magic bar - Empty" and "Magic bar - Border" Objects cancelled the
  seven-tile "Ax Battler - Magic bar"). Sets are now tried by complexity —
  more members, then larger bbox, then id — so the most specific element
  claims first and the order is total (`src/session/plane_set_order.h`, unit
  oracle `plane_set_order_test`, overlap case in `paint_set_smoke`).
- The scene inventory joined 1x1 plane-tile subs to cells by screen position
  alone, so a plane-B cell under a plane-A glyph took the glyph's sub, was
  marked claimed, and the compose skipped the original beneath it (Golden Axe,
  Stage 1: the HD "MAGIC" letters erased the tree trunk and showed the
  backdrop). The join now requires the sub's plane
  (`src/session/plane_sub_join.h`, unit oracle `plane_sub_join_test`).
- Native CI now requires all ten core-related test results and rejects failures,
  duplicate results, and unexplained skips across the complete CTest log.
  Missing optional external cores no longer excuse bundled-core ABI or
  determinism tests that did not run.
- Resolution selection now includes the 1440p tier and agrees with the public
  C++ tier values for HD, Full HD, 2K, 4K, and 8K.
- Plane sets match complete occurrences in any VDP plane, independently of the
  plane recorded at capture. Emitted overlays follow the occurrence's plane.
- Layer focus retains 50% brightness outside a focused plane and 25% outside
  focused sprites, with consistent tint and opacity across rendering paths.
- External-core test executables resolve their binary from `AYTHER_ABI_CORE`
  or `core.lock`. Native CI rejects unexplained skips and missing test results.

- Installed packages with the `engine` component now export a relocatable
  `Ayther_SHADER_DIR`, backed by the complete installed SPIR-V set; the
  out-of-tree package consumer verifies the asset contract.
- Six ignored `[[nodiscard]]` results in the GPU smoke tools. `set_visible` and
  `set_content` report whether the id was theirs to change, and dropping that
  answer is how a smoke test ends up compositing an empty layer stack and
  reporting green. They are now checked.
- Two unused parameters in `abi_write_control`, and a descriptor plus its
  capability list and recomposition cache left compiled into the ABI-less test
  core, where nothing could reach them.
- Native coverage excluded nothing on Windows. The exclusion pattern matched
  only `/`, so the vendored `third_party/` tree landed in the denominator.
- Coverage totals double-counted lines. `llvm-cov` emits one `DA` record per
  region, and summing the `LF`/`LH` summary fields counted a line once per
  region instead of once.
- `AYTHER_ENABLE_COVERAGE` emitted `-O0 -g`, which `clang-cl` rejects as unused
  arguments and, under warnings-as-errors, fails the build.
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

## [0.1.0-rc.14] - 2026-10-04

Replay QA inspection and visual representation fixes (spec 002). The earlier
pre-releases are not split out: their changes stay under [Unreleased] until the
stable `0.1.0` entry. The release-version surfaces keep the core `0.1.0`; the
tag carries the pre-release suffix.

### Added

- Render observation (spec 002, contract C3): the installed header
  `ayther/engine/render_observer.hpp`, `AytherSession::Config::render_observer`,
  `AytherSession::publish_render_observation` and
  `AytherRenderer::last_draw_report`. Per frame, a host learns which sprite
  occurrences were detected, which replacement claimed each one, why an
  assigned pose was not applied, how it was drawn and whether the frame could
  be composed. See `docs/RENDER_OBSERVATION.md`.
- Visual state (spec 002, contract C4): the installed header
  `ayther/engine/visual_state.hpp`, `AytherSession::game_state_identity`,
  `export_visual_state` and `restore_visual_state`. A checkpoint restored
  with the core state and the visual state presents the same frame as the
  linear run. See `docs/VISUAL_STATE.md`.
- `tools/render_probe` replays a take with or without a pack and writes the
  composed and core images and a JSON report per frame, with the O1 (no pack
  against the core) and O2 (pack invariants) oracles and the `--settle`,
  `--prewarm` and `--timing` options.
- Texture residency controls: `AytherRenderer::set_synchronous_textures`,
  `prewarm_textures` with its `PrewarmReport`, `sprite_texture_state`, and
  `AytherSession::catalog_texture_assets` and `pack_derived_state`.
- The pack validator reports `pose.asset_missing` and `pose.asset_unreadable`.

### Changed

- Replacement composition follows the rules R1-R9 documented in
  `docs/REPLACEMENT_COMPOSITION.md`: membership-only pose anchors, per-member
  depth partitions, the VDP sprite layer with per-pixel priority through a
  depth buffer, low-priority HD lanes in their plane pass, the framebuffer
  judge and per-line sprite limits, synchronous texture residency, poses
  without an asset left unclaimed, scroll composed by bands, and pack
  retirement. The SPIR-V of `indexed_plane.vert`, `sprite.vert`, `sprite.frag`
  and `sprite_mask.frag` changes accordingly.
- On a pack change the session now clears its own pack-derived state; the
  renderer's texture caches stay with the host, which still calls
  `AytherRenderer::evict_pack_textures`.

### Fixed

- A replacement no longer hides sprites it does not own, and is no longer
  drawn over or under the wrong sprites: the depth came from the pose's area
  and its anchor could be an unrelated sprite.
- Replacements no longer flicker or vanish while their texture decodes, and a
  cold run now produces the same image as a prewarmed one.
- Frames with raster writes in mid-screen are presented with the originals
  only, never an original and its replacement together.
- Retiring a pack no longer leaves its plane sets, screens, panoramas,
  kinematics or palette luminance peak behind.
- The animation grouper gives a hash shared by several slots the same group
  in every instance.
- The HD audio state exported while a take plays is complete even when the
  device still holds queued bytes, so the checkpoint restores.

<!-- Comparison links will be added with the first published tag. -->
