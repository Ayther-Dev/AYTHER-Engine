# Current release gate: go / no-go decision

**Pre-release decision: GO for `v0.1.0-rc.11`.**

**Stable-release decision: NO-GO for `v0.1.0`.**

**Decision date:** 2026-10-01

**Evidence cutoff:** 2026-10-01T19:40:00-03:00

**Candidate identity:** the commit targeted by the annotated tag
`v0.1.0-rc.11`. The tag object records the exact candidate SHA and is the
authoritative binding between this decision and the immutable source revision.

**Evidence baseline before the decision record:**
`a56dd2f` (`main` after merging pull request #22), which is the last
published candidate `v0.1.0-rc.10` (`47d93d97`) plus the commits `624a0fc` and
`1918fbf` on `feat/audio-events-cache`.

**Decider:** the sole maintainer, operating under
[GOV-2026-001](GOVERNANCE_EXCEPTIONS.md#gov-2026-001-single-maintainer-code-owner-review)

This record supersedes the operational GO for `v0.1.0-rc.10`, which was
published successfully as a pre-release. The `rc.10`, `rc.8`, `rc.7` and
`rc.6` decisions, the earlier `v0.1.0-rc.4` publication and the
[2026-08-30 decision](RELEASE_GO_NO_GO.md) remain immutable historical
snapshots.

## Why a new candidate

Every consumer that substitutes audio by event has to analyse the take first
(`analyze_audio_events`), which replays the whole take: about 44 s for the
13 206-frame Golden Axe take the Lab reported on 2026-10-01, on every open,
because the result lived only in the session. `rc.11` adds
`AytherSession::set_audio_events`, which restores the events of an earlier
analysis of the same take without replaying it, and
`AytherSession::kAudioEventAlgo`, which versions the detector output so a
frontend cache can be invalidated. It also fixes the caches derived from the
events (sequence anchors, one-shot timbres), which were keyed by the event
count alone and could survive the analysis of another take with the same
count. The change is additive: no existing function, struct or C ABI entry
changes.

## Decision scope

The GO authorizes publishing `v0.1.0-rc.11` as a **pre-release candidate** so
that AYTHER Lab can pin a package that restores cached audio events, and so
that the release pipeline, artifact verification and external consumption
are exercised again on a fix-forward candidate. It does not authorize
publishing the stable `v0.1.0` release.

Stable remains NO-GO because supported-release blockers and the required
rollback rehearsal are not yet closed.

## Evidence supporting the RC GO

| Criterion | Result | Evidence |
|---|---|---|
| Version contract accepts the candidate | **Pass** | `tools/check_release_version.ps1 -Tag v0.1.0-rc.11` passes for prerelease `rc.11` of `0.1.0` |
| Required CI on the change | **Pass** | [Pull request #22](https://github.com/Ayther-Dev/AYTHER-Engine/pull/22): 22 checks green, including the [Required CI gate](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/36921305429), coverage gates, ASan/UBSan, fuzz smokes and CodeQL; the opt-in GPU job was skipped. The first run failed on clang-tidy findings in the new test, fixed by `1918fbf` |
| Required CI on the merged baseline | **Pending at the cutoff** | CI and CodeQL on `a56dd2f` were queued; required CI must be green on the merge commit that adds this record before the tag |
| Open code-scanning findings | **Pass** | GitHub returned no open code-scanning alerts at the evidence cutoff |
| The change has tests | **Pass** | New `ayther.integration.audio_events_restore_test` (test core and synthetic ROM): restored events read back unchanged, a same-count restore replaces the previous one, null or zero clears, clearing still works and stepping keeps the events |
| Local suites on the candidate | **Pass** | `windows-native` full build and non-GPU CTest 61/61; `gen_api_reference -Check`, `check_doc_references` (2026-10-01, maintainer's machine) |
| ABI and package surface | **Pass** | Additive C++ API only: one new non-virtual member and one constant on `AytherSession`; core C ABI revision 7 and package `0.1.0` unchanged |
| Consumer verification | **Pending the Lab pin** | AYTHER Lab caches the events next to each take (`.audioevents`) and restores them on open; verified on Golden Axe once it pins `rc.11` |
| `rc.10` release outcome | **Pass** | Published 2026-10-01 as [pre-release `v0.1.0-rc.10`](https://github.com/Ayther-Dev/AYTHER-Engine/releases/tag/v0.1.0-rc.10); pinned by AYTHER Runtime `v0.1.0-beta.7` and by AYTHER Lab |
| Candidate tag is unused | **Pass** | `refs/tags/v0.1.0-rc.11` did not exist at the evidence cutoff |
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
pwsh ./tools/check_release_version.ps1 -Tag v0.1.0-rc.11
gh run list --branch main --limit 12
gh api 'repos/Ayther-Dev/AYTHER-Engine/code-scanning/alerts?state=open'
gh api repos/Ayther-Dev/AYTHER-Engine/environments/release
gh api repos/Ayther-Dev/AYTHER-Engine/rulesets
gh api repos/Ayther-Dev/AYTHER-Engine/git/ref/tags/v0.1.0-rc.11
```

Ruleset identifiers are not treated as stable evidence. Enumerate the active
rulesets whenever this decision is re-evaluated.
