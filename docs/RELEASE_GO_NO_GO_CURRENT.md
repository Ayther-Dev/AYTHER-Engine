# Current release gate: go / no-go decision

**Pre-release decision: GO for `v0.1.0-rc.7`.**

**Stable-release decision: NO-GO for `v0.1.0`.**

**Decision date:** 2026-09-11

**Evidence cutoff:** 2026-09-11T01:30:00-03:00

**Candidate identity:** the commit targeted by the annotated tag
`v0.1.0-rc.7`. The tag object records the exact candidate SHA and is the
authoritative binding between this decision and the immutable source revision.

**Evidence baseline before the decision record:**
`65a5e4e6d56b` (`main` at the last published candidate, `v0.1.0-rc.6`) plus
the fix-forward commits `41f8212`, `8bc74e9` and `fd37fcc` on
`feat/runtime-integration-gate-fix`, which this record accompanies.

**Decider:** the sole maintainer, operating under
[GOV-2026-001](GOVERNANCE_EXCEPTIONS.md#gov-2026-001-single-maintainer-code-owner-review)

This record supersedes the operational GO for `v0.1.0-rc.6`, which was
published successfully as a pre-release. The `rc.6` decision, the earlier
`v0.1.0-rc.4` publication and the [2026-08-30 decision](RELEASE_GO_NO_GO.md)
remain immutable historical snapshots.

## Why a new candidate

`v0.1.0-rc.6` ships a scene-inventory defect visible to every consumer that
substitutes 1×1 plane tiles: the cell→sub join looked a sub up by screen
position only, so a plane-B cell under a plane-A glyph received the glyph's
sub, was marked claimed, and the compose skipped the original beneath. In
Golden Axe, Stage 1, the HD "MAGIC" letters erased the tree trunk under them
and showed the backdrop (reported by the Lab on 2026-09-11). `rc.7` is the
fix-forward candidate: `fd37fcc` moves the join to
`src/session/plane_sub_join.h`, requires the same plane, and fixes it with the
unit oracle `ayther.unit.plane_sub_join_test`. The candidate also carries the
resolution-tier, plane-set, layer-focus and native-CI coverage changes listed
under *Fixed* in the changelog.

## Decision scope

The GO authorizes publishing `v0.1.0-rc.7` as a **pre-release candidate** so
that consumers (AYTHER Lab and AYTHER Runtime) can pin a package that contains
the inventory fix, and so that the release pipeline, artifact verification and
external consumption are exercised again on a fix-forward candidate. It does
not authorize publishing the stable `v0.1.0` release.

Stable remains NO-GO because supported-release blockers and the required
rollback rehearsal are not yet closed.

## Evidence supporting the RC GO

| Criterion | Result | Evidence |
|---|---|---|
| Version contract accepts the candidate | **Pass** | `tools/check_release_version.ps1 -Tag v0.1.0-rc.7` passes for prerelease `rc.7` of `0.1.0` (Cargo, vcpkg, CMake and `ayther_version.h` all at `0.1.0`) |
| Required CI on the last published baseline | **Pass** | [CI run 33711947727](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/33711947727) passed on `65a5e4e6d56b`; the scheduled run [34370139361](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/34370139361) of 2026-09-09 passed on the same SHA |
| Required CI on the candidate | **Pending the pull request** | The fix-forward pull request from `feat/runtime-integration-gate-fix` must pass the required CI gate before the merge; the tag is created only on the merged `main` commit |
| Open code-scanning findings | **Pass** | GitHub returned no open code-scanning alerts at the evidence cutoff |
| Inventory fix has a pure oracle | **Pass** | `tests/unit/plane_sub_join_test.cpp`: a B cell under an A glyph is not the glyph's, the A cell is, a sub emitted on B is found only by B, multi-cell sets do not enter this path, two subs at one position on two planes each join their own cell |
| Local native suites on the candidate | **Pass** | `windows-release-engine-vpx`: full build, unit 23/23, non-GPU CTest selection green (2026-09-11, maintainer's machine) |
| Consumer verification of the fix | **Pass** | AYTHER Lab rebuilt against the candidate's installed prefix and re-ran the reported case on a copy of the real project: at Stage 1 frame 60 the six plane-B cells under the glyphs go from claimed to free and the trunk is drawn again; the eleven glyphs stay replaced |
| `rc.6` release outcome | **Pass** | Published 2026-09-03 as [pre-release `v0.1.0-rc.6`](https://github.com/Ayther-Dev/AYTHER-Engine/releases/tag/v0.1.0-rc.6) with all four archives, checksums, SBOMs and Sigstore bundles; consumed by AYTHER Lab `0.1.0-beta.1` |
| Candidate tag is unused | **Pass** | `refs/tags/v0.1.0-rc.7` did not exist at the evidence cutoff |
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
lock and re-validates its own candidate against it; record that result before
re-evaluating the stable `v0.1.0` gate.

## Reproducing the pre-tag checks

```text
git rev-parse main
pwsh ./tools/check_release_version.ps1 -Tag v0.1.0-rc.7
gh run list --branch main --limit 12
gh api 'repos/Ayther-Dev/AYTHER-Engine/code-scanning/alerts?state=open'
gh api repos/Ayther-Dev/AYTHER-Engine/environments/release
gh api repos/Ayther-Dev/AYTHER-Engine/rulesets
gh api repos/Ayther-Dev/AYTHER-Engine/git/ref/tags/v0.1.0-rc.7
```

Ruleset identifiers are not treated as stable evidence. Enumerate the active
rulesets whenever this decision is re-evaluated.
