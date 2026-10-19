# Current release gate: go / no-go decision

**Pre-release decision: GO for `v0.1.0-rc.19`.**

**Stable-release decision: NO-GO for `v0.1.0`.**

**Decision date:** 2026-10-19

**Evidence cutoff:** 2026-10-19T21:00:00-03:00

**Candidate identity:** the commit targeted by the annotated tag
`v0.1.0-rc.19`. The tag object records the exact candidate SHA and is the
authoritative binding between this decision and the immutable source revision.

**Evidence baseline before the decision record:**
`dd6df45` (`main` after merging pull requests #43 and #44, on top of
`79c47fe`, the published candidate `v0.1.0-rc.18`).

**Decider:** the sole maintainer, operating under
[GOV-2026-001](GOVERNANCE_EXCEPTIONS.md#gov-2026-001-single-maintainer-code-owner-review)

This record supersedes the operational GO for `v0.1.0-rc.18`, which was
published successfully as a pre-release. The earlier decisions remain
immutable historical snapshots in this file's history and in
[RELEASE_GO_NO_GO.md](RELEASE_GO_NO_GO.md).

## Why a new candidate

The HD music of Golden Axe drops to the original sound when the game restarts
the same music: on entering the selection screen the game cuts every voice and
starts "The Battle" intro again, but the Engine kept the title's HD intro open,
let it run out two seconds later and left the original playing until the loop
anchored (15.2 to 22.9 s of the spec 006 power-on timeline). Since `rc.18`,
`main` gained:

- pull request #44: Sequence anchoring Rule 6 also re-anchors the open
  exclusive Sequence when none of its voices is sounding and its own opening
  arrives, so the HD restarts from zero with the game;
- pull request #43: a cheaper CI (vcpkg binary cache kept warm on `main`, one
  sanitizer job, one fuzz job, Windows CI builds only the VPX variant, a push
  to `main` no longer rebuilds). No product change.

## Decision scope

The GO authorizes publishing `v0.1.0-rc.19` as a **pre-release candidate** so
that AYTHER Runtime and AYTHER Play CE can lock the Engine with the fix. It
does not authorize publishing the stable `v0.1.0` release.

Stable remains NO-GO because supported-release blockers and the required
rollback rehearsal are not yet closed.

## Evidence supporting the RC GO

| Criterion | Result | Evidence |
|---|---|---|
| Version contract accepts the candidate | **Pass** | `tools/check_release_version.ps1 -Tag v0.1.0-rc.19` passes for prerelease `rc.19` of `0.1.0`; every surface keeps the core `0.1.0`, as in `rc.11` to `rc.18` |
| Required CI on the changes | **Pass** | [Pull request #44](https://github.com/Ayther-Dev/AYTHER-Engine/pull/44): 18 checks green on head `a6a4a3f` ([CI](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/39312930533)). [Pull request #43](https://github.com/Ayther-Dev/AYTHER-Engine/pull/43): 18 checks green on head `7624feb`. Both include the C++ coverage gate, ASan + UBSan, fuzz smoke, clang-format 18, clang-tidy, `cargo audit` and CodeQL; the opt-in GPU job was skipped |
| Required CI on the merged baseline | **Pass by construction** | Since #43, a push to `main` only configures (it keeps the vcpkg cache) and does not rebuild or retest. `Protect main` requires an up-to-date branch, so `dd6df45` is the tree pull request #44 validated; the release workflow builds and tests the four artifacts again from the tag |
| Open code-scanning findings | **Pass** | GitHub returned no open code-scanning alerts at the evidence cutoff |
| Dependency advisories | **Pass** | `cargo audit --deny warnings` passed on `Cargo.lock` and `fuzz/Cargo.lock` in the CI of #44; no dependency changed since `rc.18` |
| The changes have tests | **Pass** | `tests/unit/audio_seq_anchor_test.cpp`: the open Intro re-anchors at its own opening without live voices, and a sustained voice keeps its recurring hi-hat internal. Locally on the candidate sources: the 92 audio CTests of the `windows-release-engine-vpx` build |
| Consumer integration | **Pass** | AYTHER Runtime built against these sources (`out/build/windows-qa-local`) replays the spec 006 TL-010 timeline of Golden Axe: the Intro re-anchors at f799 with `restart same_key_same_asset`, ends at f1374 exactly where the Loop anchors, no other re-anchor; the maintainer confirmed by ear that the replay sounds right |
| ABI and package surface | **Pass, unchanged** | No public header, flat C ABI (revision `8`), recording format (`.arp` v10) or package change since `rc.18` |
| `rc.18` release outcome | **Pass** | Published 2026-10-18 as [pre-release `v0.1.0-rc.18`](https://github.com/Ayther-Dev/AYTHER-Engine/releases/tag/v0.1.0-rc.18) |
| Candidate tag is unused | **Pass** | `refs/tags/v0.1.0-rc.19` did not exist at the evidence cutoff |
| Release controls | **Pass with temporary governance exception** | Rulesets `Immutable release tags` and `Protect main` are active; the `release` environment requires the maintainer's approval |

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

After publication, AYTHER Runtime moves its Engine and QA Engine locks to this
candidate and AYTHER Play CE picks up that Runtime, so the music of Golden Axe
plays the restarted intro in HD; AYTHER Lab moves its lock as well, before the
stable `v0.1.0` gate is re-evaluated.

## Reproducing the pre-tag checks

```text
git rev-parse main
pwsh ./tools/check_release_version.ps1 -Tag v0.1.0-rc.19
gh run list --branch main --limit 12
gh api 'repos/Ayther-Dev/AYTHER-Engine/code-scanning/alerts?state=open'
gh api repos/Ayther-Dev/AYTHER-Engine/environments/release
gh api repos/Ayther-Dev/AYTHER-Engine/rulesets
gh api repos/Ayther-Dev/AYTHER-Engine/git/ref/tags/v0.1.0-rc.19
```

Ruleset identifiers are not treated as stable evidence. Enumerate the active
rulesets whenever this decision is re-evaluated.
