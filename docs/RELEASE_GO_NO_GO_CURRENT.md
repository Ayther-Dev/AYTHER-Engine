# Current release gate: go / no-go decision

**Pre-release decision: GO for `v0.1.0-rc.17`.**

**Stable-release decision: NO-GO for `v0.1.0`.**

**Decision date:** 2026-10-08

**Evidence cutoff:** 2026-10-08T12:00:00-03:00

**Candidate identity:** the commit targeted by the annotated tag
`v0.1.0-rc.17`. The tag object records the exact candidate SHA and is the
authoritative binding between this decision and the immutable source revision.

**Evidence baseline before the decision record:**
`cdd850f3faa5701ab51e66590efddc2db49088bf` (`main` after merging pull
request #36, on top of `56740041da24ab457ffe6a09d5e1ea7e480ef8e3`, the
published candidate `v0.1.0-rc.16`), which is `rc.16` plus the render and
audio findings of the spec 002 acceptance campaigns of 2026-10-05 and
2026-10-07.

**Decider:** the sole maintainer, operating under
[GOV-2026-001](GOVERNANCE_EXCEPTIONS.md#gov-2026-001-single-maintainer-code-owner-review)

This record supersedes the operational GO for `v0.1.0-rc.16`, which was
published successfully as a pre-release. The `rc.15`, `rc.14`, `rc.13`,
`rc.12`, `rc.11`, `rc.10`, `rc.8`, `rc.7` and `rc.6` decisions, the earlier
`v0.1.0-rc.4` publication and the [2026-08-30 decision](RELEASE_GO_NO_GO.md)
remain immutable historical snapshots.

## Why a new candidate

The spec 002 acceptance campaigns on Golden Axe Toma 3 with `rc.16` found
render and audio defects that pull request #36 fixes. The final Runtime
candidate of campaign h (2026-10-07) was accepted by the user on this Engine
source:

- the two one-frame O1 residuals that `rc.16` recorded are gone: frame 4757
  (a first hscroll write that already carries its final value now bands the
  lines above it) and frame 647 (a scene sprite whose final SAT entry no
  longer matches its occurrence bands its lines, RF-10.3);
- a raster band no longer shows a strip of the original background across the
  HD one (DI-20), bands are bounded by the core's recomposition from its final
  state (DI-21), and a band caused only by pattern writes is composed in HD
  (DI-22);
- a valid empty or large PARSED_SPRITES snapshot is authoritative and a failed
  read fails the frame closed (RF-3.6, RF-5.2);
- HD audio no longer stalls a frame: a checkpoint shares the voices' source
  PCM and a prewarmed asset is converted to the mixer format up front (D-6b);
- `AytherRecording::patch_name` replaces a take atomically on Windows and the
  writer lock keys destinations by filesystem identity (RNF-5).

## Decision scope

The GO authorizes publishing `v0.1.0-rc.17` as a **pre-release candidate** so
that AYTHER Runtime `v0.1.0-beta.11` can lock the Engine the spec 002
acceptance was run on and close its criterion 11 from a tagged distribution.
It does not authorize publishing the stable `v0.1.0` release.

Stable remains NO-GO because supported-release blockers and the required
rollback rehearsal are not yet closed.

## Evidence supporting the RC GO

| Criterion | Result | Evidence |
|---|---|---|
| Version contract accepts the candidate | **Pass** | `tools/check_release_version.ps1 -Tag v0.1.0-rc.17` passes for prerelease `rc.17` of `0.1.0`; every surface keeps the core `0.1.0`, as in `rc.11` to `rc.16` |
| Required CI on the changes | **Pass** | [Pull request #36](https://github.com/Ayther-Dev/AYTHER-Engine/pull/36): 22 checks green on its final head `fa60b5c` ([CI](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/37723427732)), including the C++ coverage gate, ASan/UBSan, fuzz smokes, clang-tidy and CodeQL; the opt-in GPU job was skipped |
| Required CI on the merged baseline | **Pass** | [CI](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/37725093724) and [Push on main](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/37725093149) green on `cdd850f`; required CI must also be green on the merge commit that adds this record before the tag |
| Open code-scanning findings | **Pass** | GitHub returned no open code-scanning alerts at the evidence cutoff |
| The changes have tests | **Pass** | Each fix of pull request #36 was observed red before it and green after. Locally on the campaign sources: `cargo test` 447 passed (1 ignored), `ctest --preset windows-native` 206/206 and the GPU suite green |
| Whole take, oracles O1-O3 (Toma 3, 7892 frames) | **Pass** | Without a pack O1 is identical on every frame (the `rc.16` residuals 647 and 4757 are gone) and O3 reports no discontinuity; with the final Golden Axe pack O2 reports no violation |
| Acceptance campaign | **Pass with accepted risks** | Spec 002 campaign h (2026-10-07): the four attempts with and without the pack were accredited and audited and the user accepted the specification on this Engine, with the accepted risks DI-24 (frame 647 shown as the core's image) and DI-26 (one P-9 sample of 1.05 periods) |
| ABI and package surface | **Pass with a recorded gap** | `audio_hd_voices_state` gains an additive overload; package `0.1.0` unchanged. The open decision from `rc.14` stands: spec 002 added flat C exports while `AYTHER_CORE_C_ABI_REVISION` stays at `7` |
| `rc.16` release outcome | **Pass** | Published 2026-10-05 as [pre-release `v0.1.0-rc.16`](https://github.com/Ayther-Dev/AYTHER-Engine/releases/tag/v0.1.0-rc.16) |
| Candidate tag is unused | **Pass** | `refs/tags/v0.1.0-rc.17` did not exist at the evidence cutoff |
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

After publication, AYTHER Runtime moves its Engine and QA Engine locks to this
candidate and publishes `v0.1.0-beta.11`, which closes the spec 002
acceptance (criterion 11), before the stable `v0.1.0` gate is re-evaluated.

## Reproducing the pre-tag checks

```text
git rev-parse main
pwsh ./tools/check_release_version.ps1 -Tag v0.1.0-rc.17
gh run list --branch main --limit 12
gh api 'repos/Ayther-Dev/AYTHER-Engine/code-scanning/alerts?state=open'
gh api repos/Ayther-Dev/AYTHER-Engine/environments/release
gh api repos/Ayther-Dev/AYTHER-Engine/rulesets
gh api repos/Ayther-Dev/AYTHER-Engine/git/ref/tags/v0.1.0-rc.17
```

Ruleset identifiers are not treated as stable evidence. Enumerate the active
rulesets whenever this decision is re-evaluated.
