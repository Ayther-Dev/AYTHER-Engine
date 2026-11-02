# Current release gate: go / no-go decision

**Pre-release decision: GO for `v0.1.0-rc.21`.**

**Stable-release decision: NO-GO for `v0.1.0`.**

**Decision date:** 2026-11-02

**Evidence cutoff:** 2026-11-02T21:39:00Z

**Candidate identity:** the commit targeted by the annotated tag
`v0.1.0-rc.21`. The tag object records the exact candidate SHA and is the
authoritative binding between this decision and the immutable source revision.

**Evidence baseline before the decision record:**
`c53794e` (`main` after merging pull request #49, on top of `14e82c5`, pull
request #48, and of `5db91f1`, the base of the published candidate
`v0.1.0-rc.20`).

**Decider:** the sole maintainer, operating under
[GOV-2026-001](GOVERNANCE_EXCEPTIONS.md#gov-2026-001-single-maintainer-code-owner-review)

This record supersedes the operational GO for `v0.1.0-rc.20`, which was
published successfully as a pre-release. The earlier decisions remain
immutable historical snapshots in this file's history and in
[RELEASE_GO_NO_GO.md](RELEASE_GO_NO_GO.md).

## Why a new candidate

Since `rc.20`, `main` gained two pull requests:

- pull request #48: a native Tessera session. `AytherSession` can drive a
  native emulation provider (Mega Drive detection and dispatch, observation,
  deterministic replay, graphics composition and audio control) next to
  Libretro, through the new public header `ayther/engine/native_emulation.hpp`.
  The provider is the CMake option `AYTHER_ENABLE_TESSERA`, OFF by default, so
  the `v*` archives of this release do not link it; the Windows software
  candidates that do are published separately under `native-v*` tags. The
  pull request also added the flat C function `ayther_pack_verify_all()`.
- pull request #49: the authoring identity Object is renamed Drawing.
  `MatchRank::Drawing` replaces `MatchRank::Prop` (kept as a deprecated alias
  for one release), `rank_name` returns `"drawing"` and plane sets written
  without a `type` read as `"drawing"`. The Engine never branches on `type`, so
  packs that carry `type = "utileria"` load exactly as before.

The authoring applications (AYTHER Lab, Runtime) can lock a single Engine that
carries both changes.

## Decision scope

The GO authorizes publishing `v0.1.0-rc.21` as a **pre-release candidate**. It
does not authorize publishing the stable `v0.1.0` release.

Stable remains NO-GO because supported-release blockers and the required
rollback rehearsal are not yet closed.

## Evidence supporting the RC GO

| Criterion | Result | Evidence |
|---|---|---|
| Version contract accepts the candidate | **Pass** | `tools/check_release_version.ps1 -Tag v0.1.0-rc.21` passes for prerelease `rc.21` of `0.1.0`; every surface keeps the core `0.1.0`, as in `rc.11` to `rc.20` |
| Required CI on the changes | **Pass** | [Pull request #48](https://github.com/Ayther-Dev/AYTHER-Engine/pull/48): 20 checks green and the opt-in GPU job skipped on head `4a6ce4d`. [Pull request #49](https://github.com/Ayther-Dev/AYTHER-Engine/pull/49): 19 checks green and the opt-in GPU job skipped on head `3bad09d`, after its clang-format 18 failure was fixed |
| Required CI on the merged baseline | **Pass** | The `CI` workflow on `main` succeeded at `c53794e` and at `14e82c5`. `Protect main` requires an up-to-date branch; the release workflow builds and tests the four artifacts again from the tag |
| Open code-scanning findings | **Pass** | GitHub returned no open code-scanning alerts at the evidence cutoff |
| Dependency advisories | **Pass** | `Cargo.lock`, `fuzz/Cargo.lock` and `vcpkg.json` are identical to `rc.20`; `cargo audit --deny warnings` ran in the CI of both pull requests |
| The changes have tests | **Pass** | #48: provider, controls, pack, presentation and native-session tests, plus a long Release replay reported in its description (18,000 frames, 4,500 continuations, zero divergences). #49: `tests/integration/elements_toml_test.cpp` now checks that a `[[set]]` with `type = "utileria"` (an old pack) keeps its value, that an empty type reads as `"drawing"`, and that both load identically; `pack_removal_test`, `plane_set_*` and `panorama_cover_test` pass locally on the candidate sources. The full local `ctest` of the candidate was not rerun for this record: the tag workflow does it |
| Consumer integration | **Not yet exercised against this candidate** | AYTHER Lab [pull request #24](https://github.com/Ayther-Dev/AYTHER-Lab/pull/24) (the Drawing rename, written against `rc.20`) is green and reads old and new ids. Locking Lab and Runtime onto `rc.21` is the consumers' own gate after publication |
| ABI and package surface | **Pass, changed and documented** | Flat C ABI revision `8` becomes `9` (`ayther_pack_verify_all()` is new; `docs/API_COMPATIBILITY.md` records it). New public header `engine/native_emulation.hpp` is in `cmake/AytherPublicHeaders.txt`, and `ayther_session.h` gains the native-provider API additively. `MatchRank::Prop` is deprecated, not removed. The recording format (`.arp` v10) did not change. The package config finds Tessera only when `AYTHER_ENABLE_TESSERA` is on |
| `rc.20` release outcome | **Pass** | Published 2026-10-20 as [pre-release `v0.1.0-rc.20`](https://github.com/Ayther-Dev/AYTHER-Engine/releases/tag/v0.1.0-rc.20) |
| Candidate tag is unused | **Pass** | `refs/tags/v0.1.0-rc.21` did not exist at the evidence cutoff |
| Release controls | **Pass with temporary governance exception** | Rulesets `Immutable release tags` and `Protect main` are active; the `release` environment has a branch policy and required reviewers |

The final candidate commit is the merge result containing this record. Before
tag creation, the pull request that adds it must be green and up to date with
`main`. The annotated tag message must contain both the explicit GO and the
full target SHA. This avoids claiming that a file inside a Git commit can
contain its own SHA: changing such a file would itself produce a different
commit.

## Approval and single-maintainer exception

`@Ayther-Dev/maintainers` has one eligible member. Under `GOV-2026-001`, the
same maintainer may record this GO and approve the protected `release`
environment because `prevent_self_review` is disabled temporarily.

That approval is an operational gate, not independent review, four-eyes
approval, or separation of duties. The release evidence must describe it as a
single-maintainer decision. The exception remains time-bounded and must be
removed when a second eligible reviewer exists, before the first supported
stable release, or on 2026-11-30, whichever happens first.

## Publication acceptance criteria

The RC publication is successful only if the tag-triggered release workflow:

1. validates the version contract on the exact annotated-tag target;
2. builds and tests all four advertised product/platform artifacts;
3. reproduces each archive byte-for-byte within its build job;
4. verifies every packaged payload with the repository-owned minimal consumer;
5. reaches the protected `release` environment and records its approval;
6. publishes checksums, SBOMs, Sigstore bundles, and provenance alongside the
   archives; and
7. creates a GitHub **pre-release**, not a stable release.

Any failed mandatory job, unexpected asset set, missing attestation, or version
mismatch changes the operational outcome to NO-GO. The immutable tag must not
be moved or deleted; remediation uses a new commit and a new RC tag.

## Rollback and follow-up

If publication or post-publication verification exposes a defect, follow
[RELEASE_ROLLBACK.md](RELEASE_ROLLBACK.md). Preserve the immutable tag and run
evidence, withdraw affected release assets as documented, notify consumers,
and publish a fix-forward candidate under a new version.

After publication, AYTHER Lab and Runtime can move their Engine lock to this
candidate. The Lab writes `type = "drawing"` and still reads the old ids, so it
works against `rc.20` and `rc.21` alike. A consumer that calls
`ayther_pack_verify_all()` must require ABI revision `9`.

## Reproducing the pre-tag checks

```text
git rev-parse main
pwsh ./tools/check_release_version.ps1 -Tag v0.1.0-rc.21
gh run list --branch main --limit 12
gh api 'repos/Ayther-Dev/AYTHER-Engine/code-scanning/alerts?state=open'
gh api repos/Ayther-Dev/AYTHER-Engine/environments/release
gh api repos/Ayther-Dev/AYTHER-Engine/rulesets
gh api repos/Ayther-Dev/AYTHER-Engine/git/ref/tags/v0.1.0-rc.21
```

Ruleset identifiers are not treated as stable evidence. Enumerate the active
rulesets whenever this decision is re-evaluated.
