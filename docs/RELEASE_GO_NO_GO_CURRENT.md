# Current release gate: go / no-go decision

**Pre-release decision: GO for `v0.1.0-rc.13`.**

**Stable-release decision: NO-GO for `v0.1.0`.**

**Decision date:** 2026-10-02

**Evidence cutoff:** 2026-10-02T15:15:00-03:00

**Candidate identity:** the commit targeted by the annotated tag
`v0.1.0-rc.13`. The tag object records the exact candidate SHA and is the
authoritative binding between this decision and the immutable source revision.

**Evidence baseline before the decision record:**
`08643857199dfbb05a6bc213cbc145e5f2b9e6ea` (`main` after merging pull
request #26), which is the failed candidate `v0.1.0-rc.12` plus the
release-only QA-280 fixture-path correction.

**Decider:** the sole maintainer, operating under
[GOV-2026-001](GOVERNANCE_EXCEPTIONS.md#gov-2026-001-single-maintainer-code-owner-review)

This record supersedes the operational GO for `v0.1.0-rc.12`, whose immutable
release workflow failed before producing artifacts. The `rc.11`, `rc.10`,
`rc.8`, `rc.7` and
`rc.6` decisions, the earlier `v0.1.0-rc.4` publication and the
[2026-08-30 decision](RELEASE_GO_NO_GO.md) remain immutable historical
snapshots.

## Why a new candidate

All four `rc.12` artifact builds failed in the same test before packaging:
QA-280 derived the shared RF-18 fixture path from `__FILE__`, while release
builds intentionally remap source paths to `/usr/src/ayther` for
reproducibility. Pull request #26 injects the real checkout fixture path from
CMake and adds a release-path contract that rejects future runtime fixture
lookups based on `__FILE__`. No product behavior, ABI or package surface
changes; this candidate is a release-test fix forward.

## Decision scope

The GO authorizes publishing `v0.1.0-rc.13` as a **pre-release candidate** to
exercise the corrected release-only QA-280 path and complete the four-artifact
pipeline. It does not authorize
publishing the stable `v0.1.0` release.

Stable remains NO-GO because supported-release blockers and the required
rollback rehearsal are not yet closed.

## Evidence supporting the RC GO

| Criterion | Result | Evidence |
|---|---|---|
| Version contract accepts the candidate | **Pass** | `tools/check_release_version.ps1 -Tag v0.1.0-rc.13` passes for prerelease `rc.13` of `0.1.0` |
| Required CI on the correction | **Pass** | [Pull request #26](https://github.com/Ayther-Dev/AYTHER-Engine/pull/26) merged only after its required checks passed |
| Required CI on the merged baseline | **Pending at the cutoff** | CI and CodeQL on `08643857199dfbb05a6bc213cbc145e5f2b9e6ea` were in progress; required CI must be green on the merge commit that adds this record before the tag |
| Open code-scanning findings | **Pass** | GitHub returned no open code-scanning alerts at the evidence cutoff |
| The correction has a regression contract | **Pass** | `ayther.quality.release_fixture_paths` fails if a release test derives fixture paths from `__FILE__` or omits the injected RF-18 path |
| Local correction tests | **Pass** | The regression was observed red before the fix; after recompilation `ayther.quality.release_fixture_paths` and `ayther.integration.elements_toml_test` passed 2/2 on Windows |
| ABI and package surface | **Pass** | Public Engine audio observation and music-continuity contracts are installable and exercised by the external package consumer; core C ABI revision 7 and package `0.1.0` remain unchanged |
| Human audio verification | **Pass** | QA-309 was accepted by the maintainer: the Golden Axe title/menu/selection intro no longer restarts and the remainder of the replay sounds correct |
| `rc.12` release outcome | **Fail, preserved** | [Release run 37033888520](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/37033888520) failed QA-280 in all four artifact builds; attest, consume and publish were correctly skipped; the tag is not moved or rerun |
| Candidate tag is unused | **Pass** | `refs/tags/v0.1.0-rc.13` did not exist at the evidence cutoff |
| Release controls | **Pass with temporary governance exception** | Rulesets `Immutable release tags` and `Protect main` are active; the `release` environment requires the maintainer's approval |

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

After publication, AYTHER Lab pins the candidate with a declared skew against
AYTHER Runtime `v0.1.0-beta.7` (built with `rc.10`; the Runtime does not use
the new API) and records the Golden Axe take reopening without re-analysis
before re-evaluating the stable `v0.1.0` gate.

## Reproducing the pre-tag checks

```text
git rev-parse main
pwsh ./tools/check_release_version.ps1 -Tag v0.1.0-rc.13
gh run list --branch main --limit 12
gh api 'repos/Ayther-Dev/AYTHER-Engine/code-scanning/alerts?state=open'
gh api repos/Ayther-Dev/AYTHER-Engine/environments/release
gh api repos/Ayther-Dev/AYTHER-Engine/rulesets
gh api repos/Ayther-Dev/AYTHER-Engine/git/ref/tags/v0.1.0-rc.13
```

Ruleset identifiers are not treated as stable evidence. Enumerate the active
rulesets whenever this decision is re-evaluated.
