# Current release gate: go / no-go

**Latest published pre-release:** `v0.1.0-rc.28` (`6d1c2e8`), 2026-11-19, with
the native candidate `native-v0.1.0-rc.28` from the same commit.

**Stable-release decision: NO-GO for `v0.1.0`.**

**State recorded:** 2026-11-19

**Decider:** the sole maintainer, operating under
[GOV-2026-001](GOVERNANCE_EXCEPTIONS.md#gov-2026-001-single-maintainer-code-owner-review)

This file is the current release decision. Earlier decisions are kept below as
superseded records and in this file's Git history;
[RELEASE_GO_NO_GO.md](RELEASE_GO_NO_GO.md) is the first stable-gate assessment
(2026-08-30).

## Pre-releases since the last written GO

The last written GO record authorized `v0.1.0-rc.23` (below). The later
candidates were small fix-forward releases. Each was authorized through the
protected `release` environment, approved by the sole maintainer, without a
separate written record:

| Tag | Commit | Change | Release workflow | Outcome |
|---|---|---|---|---|
| `v0.1.0-rc.24` | `67629e4` | Tessera native `reset()` reapplies observer subscriptions; Runtime integration uses Runtime's toolchains | run `43629889182`, approved | Published 2026-11-14 |
| `v0.1.0-rc.25` | `2f04a02` | Packless Tessera sessions initialize the pack consumers | run `43729999447`, approved | Published 2026-11-15 |
| `v0.1.0-rc.26` | `10100fe` | Plane-set matcher indexes cells by hash | run `44510504646` failed: both Windows packages failed to compile on runner image 20261116 (MSVC 14.52 with LLVM 20) | No release; tag kept |
| `v0.1.0-rc.27` | `5d00853` | LLVM 22.1.8 pinned for Windows jobs | run `44526333464` failed in `Finalize and attest release inputs` (Rekor 409 on a retried signature) | No release; tag kept |
| `v0.1.0-rc.28` | `6d1c2e8` | Cosign signing retried with fresh keys | run `44543445802`, approved | Published 2026-11-19 |

The native drafts `native-v0.1.0-rc.26` and `native-v0.1.0-rc.27` were built
successfully but never published. The changes are listed in
[CHANGELOG.md](../CHANGELOG.md).

## Evidence at `v0.1.0-rc.28`

| Criterion | Result | Evidence |
|---|---|---|
| Version contract | **Pass** | `tools/check_release_version.ps1 -Tag v0.1.0-rc.28`: pre-release `rc.28` of `0.1.0` |
| Required CI | **Pass** | Pull request #62 green (run `44529951482`); `CI` on `main` at `6d1c2e8` green (run `44543326669`, configure-only by design) |
| Release workflow | **Pass** | Run `44543445802`: four builds, attestation, four package consumers, approved `release` deployment, pre-release created |
| Native candidate | **Pass** | `Native Tessera software candidate` run `44543445839` against Tessera `v0.2.0-rc.9` (`d29385a`); the draft was published as a pre-release on 2026-11-19 |
| Runtime integration | **Pass** | Run `44553987318` against the published archives, pinned Runtime `500cf88` |
| Consumer locks | **Pass** | AYTHER Runtime `main` locks `v0.1.0-rc.28` (regular and QA locks) and `native-v0.1.0-rc.28` with Tessera `v0.2.0-rc.9` |
| Coverage | **Pass** | CI run `44506963494` (`10100fe`): Rust 78.71%, C++ 81.01% combined, above the floors in [Coverage policy](COVERAGE.md) |
| Open code-scanning findings | **Pass** | `gh api .../code-scanning/alerts?state=open` returned no alerts on 2026-11-19 |
| ABI and package surface | **Pass, unchanged since rc.23** | No change under `include/`, `cmake/AytherPublicHeaders.txt`, `Cargo.lock`, `fuzz/Cargo.lock`, or `vcpkg.json` between `v0.1.0-rc.23` and `v0.1.0-rc.28`. Flat C ABI revision `9`; take format `.arp` v11 |
| Private vulnerability reporting | **Pass** | Enabled for the repository |
| Release controls | **Pass with temporary governance exception** | Rulesets `Immutable release tags` (`refs/tags/v*`: no update, no deletion, no bypass) and `Protect main` (pull request, strict `Required CI gate`, no deletion or force push) are active; the `release` environment has a branch policy and one required reviewer with `prevent_self_review` disabled |

## Why stable remains NO-GO

- The open and deferred blockers in
  [Project status](PROJECT_STATUS.md#release-blockers) are not closed.
- The [rollback procedure](RELEASE_ROLLBACK.md) has not been rehearsed against
  a published candidate.
- GOV-2026-001 is active: no independent Code Owner review or two-person
  release control exists. It expires on 2026-11-30 or when the first stable
  go/no-go is opened, whichever comes first.
- The immutable-tag ruleset does not cover `native-v*` tags.

## Publication acceptance criteria for the next candidate

A candidate publication is successful only if the tag-triggered release
workflow:

1. validates the version contract on the exact annotated-tag target;
2. builds and tests all four advertised product/platform artifacts;
3. reproduces each archive byte-for-byte within its build job;
4. verifies every packaged payload with the repository-owned minimal consumer;
5. reaches the protected `release` environment and records its approval;
6. publishes checksums, SBOMs, Sigstore bundles, and provenance alongside the
   archives; and
7. creates a GitHub **pre-release**, not a stable release.

Any failed mandatory job, unexpected asset set, missing attestation, or version
mismatch is a NO-GO for that tag. The tag is not moved or deleted; the fix
ships under a new commit and a new tag. If a published candidate turns out to
be defective, follow [RELEASE_ROLLBACK.md](RELEASE_ROLLBACK.md).

## Reproducing the checks

```text
pwsh ./tools/check_release_version.ps1 -Tag v0.1.0-rc.28
gh run list --branch main --limit 12
gh run list --workflow release.yml --limit 8
gh api 'repos/Ayther-Dev/AYTHER-Engine/code-scanning/alerts?state=open'
gh api repos/Ayther-Dev/AYTHER-Engine/private-vulnerability-reporting
gh api repos/Ayther-Dev/AYTHER-Engine/environments/release
gh api repos/Ayther-Dev/AYTHER-Engine/rulesets
gh api repos/Ayther-Dev/AYTHER-Engine/actions/runs/<run-id>/approvals
```

Ruleset identifiers are not stable evidence. Enumerate the active rulesets
whenever this decision is re-evaluated.

## Superseded: GO for `v0.1.0-rc.23` (2026-11-13)

> Superseded by the candidates above. Kept as the dated record of the last
> written pre-release GO.

**Decision:** GO for publishing `v0.1.0-rc.23` as a pre-release; NO-GO for
stable `v0.1.0`. **Evidence cutoff:** 2026-11-13T12:00:00Z. **Evidence
baseline:** `3168546` (`main` after pull request #55, on top of `1dff21f`, the
base of `v0.1.0-rc.22`). The annotated tag records the exact candidate SHA.

Since `rc.22`, `main` had gained pull request #55 (spec 008, WP-7, the Engine
side of the Tessera native provider): `ObservationSource`, native Mega Drive
sessions through `AytherSession`, the filled-in `tools/parity_bench`, and the
Tessera `v0.2.0-rc.9` lock. It also fixed an infinite recursion in the
session's `emu_fps`.

| Criterion | Result | Evidence |
|---|---|---|
| Version contract accepts the candidate | **Pass** | `tools/check_release_version.ps1 -Tag v0.1.0-rc.23` passes for prerelease `rc.23` of `0.1.0` |
| Required CI on the change | **Pass** | [Pull request #55](https://github.com/Ayther-Dev/AYTHER-Engine/pull/55): 19 checks green and the opt-in GPU job skipped on head `65727be` (run `43408958113`) |
| Required CI on the merged baseline | **Pass** | `CI` on `main` (run `43417571066`) at `3168546` |
| Open code-scanning findings | **Pass** | No open alerts at the evidence cutoff |
| Dependency advisories | **Pass** | `vcpkg.json` identical to `rc.22`; the lockfiles only move `rustix` to 1.1.6; `cargo audit --deny warnings` ran in the pull request CI |
| The change has tests | **Pass, with a stated limit** | `observation_source_parity`, `tessera_native_state`, `AUDIO_MUTE` masks on both providers, native subsystems, `RewindBuffer`, and the parity bench. The regular release artifacts do not include the native provider |
| Consumer integration | **Not yet exercised** | Runtime integration had failed against `rc.22` because it pinned Runtime `7580a88`; `rc.23` pinned Runtime `500cf88` |
| ABI and package surface | **Pass, unchanged** | Flat C ABI revision `9`; `.arp` v11; installed header manifest unchanged |
| Candidate tag unused; release controls | **Pass** | `refs/tags/v0.1.0-rc.23` did not exist; rulesets and the `release` environment active |

Outcome: published 2026-11-13 as a pre-release; the post-release Runtime
integration first failed on Runtime's toolchain checks and passed after
`b44ba49`, which shipped in `v0.1.0-rc.24`.
