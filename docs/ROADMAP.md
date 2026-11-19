# Engineering roadmap

**Status:** directional, not a release promise

**Last reviewed:** 2026-11-19

This roadmap orders the remaining work toward a stable `0.1.0` by dependency and
risk. It has no dates. What already exists is described in
[Project status](PROJECT_STATUS.md); what changed is in the
[changelog](../CHANGELOG.md).

## 1. Close the engineering release blockers

- Add an automated ABI symbol and layout baseline that compares each build with
  the previous release (blocker 4).
- Run the real-emulator oracles in CI against the locked Genesis Plus GX fork
  core, as the cross-pack test already does, so they stop skipping (blocker 2).
- Complete the security review of decoder limits, scripting lifetimes, FFI
  ownership, dynamic core loading, and fuzz coverage (blocker 7).

Exit criterion: blockers 2, 4, and 7 in [Project status](PROJECT_STATUS.md#release-blockers)
are closed with linked evidence.

## 2. Complete the native provider surface

- Decide, per option, whether `patch_path`, `core_options`, `rom_path`, and the
  `audio_observer` and `render_observer` callbacks get native equivalents or
  stay libretro-only, and document the result in
  [Tessera native provider](TESSERA_NATIVE.md).
- Keep observation equivalence with the reference core enforced through
  `tools/parity_bench check` as Tessera evolves, bumping the lock only with the
  code that needs it.
- Decide whether the regular release packages carry the provider and whether a
  Linux provider is in scope.

Exit criterion: every `Config` option has a documented native behavior, and the
native package follows the same release controls as the regular packages.

## 3. Close the external blockers

- Exercise the public-header allowlist and pack trust through the shipping
  first-party hosts (blockers 1 and 5).
- Provision operational signing keys and rehearse rotation, revocation, and the
  [rollback procedure](RELEASE_ROLLBACK.md).
- Retire the [single-maintainer exception](GOVERNANCE_EXCEPTIONS.md) when a
  second eligible reviewer exists.

## 4. Declare supported scope

- Turn the [support matrix](SUPPORT_MATRIX.md) into a support commitment:
  platforms, compilers, GPU drivers, emulator cores, and pack schema.
- Publish deprecation and security-support windows.
- Verify documentation examples against released artifacts.

Only after these steps should the project consider a stable release. New
systems or presentation features must not bypass the security, legal,
compatibility, and reproducibility gates above.
