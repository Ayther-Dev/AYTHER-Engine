# Current release gate: go / no-go decision

**Pre-release decision: GO for `v0.1.0-rc.8`.**

**Stable-release decision: NO-GO for `v0.1.0`.**

**Decision date:** 2026-09-11

**Evidence cutoff:** 2026-09-11T13:30:00-03:00

**Candidate identity:** the commit targeted by the annotated tag
`v0.1.0-rc.8`. The tag object records the exact candidate SHA and is the
authoritative binding between this decision and the immutable source revision.

**Evidence baseline before the decision record:**
`414b704b0c5d` (`main` at the last published candidate, `v0.1.0-rc.7`) plus
the fix-forward commit `5313662` on `fix/plane-set-order-rc8`, which this
record accompanies.

**Decider:** the sole maintainer, operating under
[GOV-2026-001](GOVERNANCE_EXCEPTIONS.md#gov-2026-001-single-maintainer-code-owner-review)

This record supersedes the operational GO for `v0.1.0-rc.7`, which was
published successfully as a pre-release. The `rc.7` and `rc.6` decisions, the
earlier `v0.1.0-rc.4` publication and the [2026-08-30 decision](RELEASE_GO_NO_GO.md)
remain immutable historical snapshots.

## Why a new candidate

`v0.1.0-rc.7` ships a plane-set matching defect visible to every consumer that
substitutes overlapping multi-tile elements: the matcher walked `plane_sets`
— an `unordered_map` — in bucket order, and a set claims the cells it
matches, so a one-member set with a lucky id could claim a cell before the
larger set containing it was tried, and the larger set never matched. In
Golden Axe, assigning HD to the one-tile "Magic bar - Empty" and "Magic bar -
Border" Objects cancelled the seven-tile "Ax Battler - Magic bar" (reported by
the Lab on 2026-09-11). `rc.8` is the fix-forward candidate: `5313662` tries
the sets by complexity — more members, then larger bbox, then id — through
`src/session/plane_set_order.h`, applying within the plane-set domain the
ladder rule `ayther_rank.h` already states between domains, and fixes it with
the unit oracle `ayther.unit.plane_set_order_test` and an overlap case in
`tools/paint_set_smoke`.

## Decision scope

The GO authorizes publishing `v0.1.0-rc.8` as a **pre-release candidate** so
that consumers (AYTHER Lab and AYTHER Runtime) can pin a package in which a
larger plane set outranks the one-tile sets it contains, and so that the
release pipeline, artifact verification and external consumption are
exercised again on a fix-forward candidate. It does not authorize publishing
the stable `v0.1.0` release.

Stable remains NO-GO because supported-release blockers and the required
rollback rehearsal are not yet closed.

## Evidence supporting the RC GO

| Criterion | Result | Evidence |
|---|---|---|
| Version contract accepts the candidate | **Pass** | `tools/check_release_version.ps1 -Tag v0.1.0-rc.8` passes for prerelease `rc.8` of `0.1.0` (Cargo, vcpkg, CMake and `ayther_version.h` all at `0.1.0`) |
| Required CI on the last published baseline | **Pass** | [CI run 34555815917](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/34555815917) and [Push on main 34555815459](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/34555815459) passed on `414b704b0c5d`; [Runtime integration 34596513727](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/34596513727) passed on the same SHA after the `rc.7` publication |
| Required CI on the candidate | **Pending the pull request** | [Pull request #18](https://github.com/Ayther-Dev/AYTHER-Engine/pull/18) from `fix/plane-set-order-rc8` must pass the required CI gate before the merge; the tag is created only on the merged `main` commit |
| Open code-scanning findings | **Pass** | GitHub returned no open code-scanning alerts at the evidence cutoff |
| Matching fix has a pure oracle | **Pass** | `tests/unit/plane_set_order_test.cpp`: the reported case (a seven-member set and two one-member sets with lower ids) yields the seven-member set first in any insertion order; equal member counts fall to the larger bbox; a full tie falls to ascending id; member count outranks bbox area; no sets gives an empty order |
| Matching fix reproduced on the real core | **Pass** | `tools/paint_set_smoke` overlap case (a one-tile set with a lower id sharing the anchor of a 2×1 set) run on the maintainer's machine against the real core and ROM: the 2×1 keeps its quad and the one-tile set does not claim the anchor. The tool is not wired into CI; its pre-existing pack-open step fails in Release builds because the development key is rejected by trust policy, unrelated to this candidate |
| Local native suites on the candidate | **Pass** | `windows-release-engine-vpx`: full build, unit 24/24, non-GPU CTest selection 62/62 including the repository policy tests (2026-09-11, maintainer's machine); `windows-native`: full build, unit 24/24 |
| Consumer verification of the fix | **Pending the Lab pin** | AYTHER Lab consumes the published package, not the tree; the reported project case (Golden Axe, "Ax Battler - Magic bar" over "Magic bar - Empty/Border") is re-run when the Lab pins `rc.8`, and that result is recorded in the Lab's validation record before the stable gate is re-evaluated |
| `rc.7` release outcome | **Pass** | Published 2026-09-11 as [pre-release `v0.1.0-rc.7`](https://github.com/Ayther-Dev/AYTHER-Engine/releases/tag/v0.1.0-rc.7) with all four archives, checksums, SBOMs and Sigstore bundles; pinned by AYTHER Lab on its `rc.7` bump branch with the engine compatibility oracle green (54 checks) |
| Candidate tag is unused | **Pass** | `refs/tags/v0.1.0-rc.8` did not exist at the evidence cutoff |
| Release controls | **Pass with temporary governance exception** | Rulesets `Immutable release tags` and `Protect main` are active; the `release` environment requires the maintainer's approval and accepts `v*` |

The final candidate commit is the merge result containing this record. Before
tag creation, required CI must be green on that exact `main` commit. The
annotated tag message must contain both the explicit GO and the full target
SHA. This avoids claiming that a file inside a Git commit can contain its own
SHA: changing such a file would itself produce a different commit.

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

After publication, AYTHER Lab pins the `vpx` Windows archive in its dependency
lock and re-validates its own candidate against it, including the reported
Golden Axe case; record that result before re-evaluating the stable `v0.1.0`
gate.

## Reproducing the pre-tag checks

```text
git rev-parse main
pwsh ./tools/check_release_version.ps1 -Tag v0.1.0-rc.8
gh run list --branch main --limit 12
gh api 'repos/Ayther-Dev/AYTHER-Engine/code-scanning/alerts?state=open'
gh api repos/Ayther-Dev/AYTHER-Engine/environments/release
gh api repos/Ayther-Dev/AYTHER-Engine/rulesets
gh api repos/Ayther-Dev/AYTHER-Engine/git/ref/tags/v0.1.0-rc.8
```

Ruleset identifiers are not treated as stable evidence. Enumerate the active
rulesets whenever this decision is re-evaluated.
