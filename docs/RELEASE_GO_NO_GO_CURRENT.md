# Current release gate: go / no-go decision

**Pre-release decision: GO for `v0.1.0-rc.15`.**

**Stable-release decision: NO-GO for `v0.1.0`.**

**Decision date:** 2026-10-04

**Evidence cutoff:** 2026-10-04T13:45:00-03:00

**Candidate identity:** the commit targeted by the annotated tag
`v0.1.0-rc.15`. The tag object records the exact candidate SHA and is the
authoritative binding between this decision and the immutable source revision.

**Evidence baseline before the decision record:**
`191301994c7a1c8cb0918ebf66e0198359b95e09` (`main` after merging pull
request #31), which is the published candidate `v0.1.0-rc.14` plus the spec
002 P-9 audio alignment: the frame output boundary (pull request #30) and the
first-resume fix (pull request #31).

**Decider:** the sole maintainer, operating under
[GOV-2026-001](GOVERNANCE_EXCEPTIONS.md#gov-2026-001-single-maintainer-code-owner-review)

This record supersedes the operational GO for `v0.1.0-rc.14`, which was
published successfully as a pre-release. The `rc.13`, `rc.12`, `rc.11`, `rc.10`,
`rc.8`, `rc.7` and
`rc.6` decisions, the earlier `v0.1.0-rc.4` publication and the
[2026-08-30 decision](RELEASE_GO_NO_GO.md) remain immutable historical
snapshots.

## Why a new candidate

AYTHER Runtime's replay QA measures P-9: when inspection resumes after a pause,
the audio of frame k+1 must reach the device within one frame period of its
image. `rc.14` gave the Runtime no way to place a frame on the device output
line, and the first resume early in a take arrived about 70 ms late. This
candidate adds:

- the audio observation fact `audio_frame_output_boundary` (pull request #30):
  every frame that reaches the device reports its first sample on
  `engine_main_output` after rate control, with or without HD voices.
  Additive: no existing fact changes;
- the first-resume fix (pull request #31): after `pause_after_drain` the device
  restarts with frame k+1's PCM and the drained backlog is no longer re-primed
  with silence. Tradeoff: playback resumes with an empty backlog, as it already
  did after a short pause, and rate control rebuilds it. Normal playback and
  `cut_transport_audio` are unchanged.

The Runtime measured P-9 with an Engine built from pull request #31 on Toma 3
with the pack: all 30 resumes within one period (−11.5 to +8.9 ms; the first
one went from +75.6 ms to −11.5 ms).

## Decision scope

The GO authorizes publishing `v0.1.0-rc.15` as a **pre-release candidate** so
that AYTHER Runtime can consume the frame output boundary and the first-resume
fix from a tagged distribution, and the spec 002 acceptance campaign can run
from it. It does not authorize
publishing the stable `v0.1.0` release.

Stable remains NO-GO because supported-release blockers and the required
rollback rehearsal are not yet closed.

## Evidence supporting the RC GO

| Criterion | Result | Evidence |
|---|---|---|
| Version contract accepts the candidate | **Pass** | `tools/check_release_version.ps1 -Tag v0.1.0-rc.15` passes for prerelease `rc.15` of `0.1.0`; every surface keeps the core `0.1.0`, as in `rc.11` to `rc.14` |
| Required CI on the changes | **Pass** | [Pull request #30](https://github.com/Ayther-Dev/AYTHER-Engine/pull/30) ([Required CI gate](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/37188486284)) and [pull request #31](https://github.com/Ayther-Dev/AYTHER-Engine/pull/31) ([Required CI gate](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/37214310878)): 22 checks green on each final head, including the C++ coverage gate with the software Vulkan run, ASan/UBSan, fuzz smokes and CodeQL; the opt-in GPU job was skipped |
| Required CI on the merged baseline | **Pending at the cutoff** | CI on `191301994c7a1c8cb0918ebf66e0198359b95e09` was in progress; required CI must be green on the merge commit that adds this record before the tag |
| Open code-scanning findings | **Pass** | GitHub returned no open code-scanning alerts at the evidence cutoff |
| The changes have tests | **Pass** | Each change was observed red before it and green after: `audio_qa_frame_output_boundary` (48 kHz device, rate-control change; each frame's step within one sample of its boundary), `frame_output_boundary_session_test` (every audible frame reported; none in silent production) and `audio_qa_frame_output_resume` (gap k to k+1 after a 400 ms drain pause: 4329 samples before, 985 after, budget 1312) |
| P-9 at resume (Runtime, Toma 3 with the pack) | **Pass** | 30 of 30 resumes within one period (16.7 ms): −11.5 to +8.9 ms, p95 8.9 ms, with the Engine of pull request #31 built locally. To be repeated with the published `rc.15` |
| Other inspection budgets in the same run | **Recorded** | P-2 (pause response) gave 2.216 periods on 1 of 30 pauses (budget 2); pull request #31 changes resume, not pause, and the cause is not established. The decision on P-2 belongs to the spec 002 coordinator and does not block a pre-release |
| Performance | **Pass** | P-8 at rest (2026-10-04), five interleaved rounds on Toma 3 with the pack: total p95 6.28 ms base, 6.10 ms current (−2.9%), under the 16.6 ms budget |
| ABI and package surface | **Pass with a recorded gap** | No public header or package change beyond `AudioPlayer` additions (`FrameOutputBoundary`, `set_frame_output_observer`, `set_rate_ratio_for_test`); package `0.1.0` unchanged. The open decision from `rc.14` stands: spec 002 added flat C exports while `AYTHER_CORE_C_ABI_REVISION` stays at `7` (revisions 6 and 7 were bumped for added symbols). Whether to bump it to `8` is open for the maintainer and does not block a pre-release |
| `rc.14` release outcome | **Pass** | Published 2026-10-04 as [pre-release `v0.1.0-rc.14`](https://github.com/Ayther-Dev/AYTHER-Engine/releases/tag/v0.1.0-rc.14) |
| Candidate tag is unused | **Pass** | `refs/tags/v0.1.0-rc.15` did not exist at the evidence cutoff |
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

After publication, AYTHER Runtime moves its QA Engine lock to this candidate,
repeats P-9 against the published package, and the spec 002 acceptance
campaign installs from the tagged distribution before the stable `v0.1.0`
gate is re-evaluated.

## Reproducing the pre-tag checks

```text
git rev-parse main
pwsh ./tools/check_release_version.ps1 -Tag v0.1.0-rc.15
gh run list --branch main --limit 12
gh api 'repos/Ayther-Dev/AYTHER-Engine/code-scanning/alerts?state=open'
gh api repos/Ayther-Dev/AYTHER-Engine/environments/release
gh api repos/Ayther-Dev/AYTHER-Engine/rulesets
gh api repos/Ayther-Dev/AYTHER-Engine/git/ref/tags/v0.1.0-rc.15
```

Ruleset identifiers are not treated as stable evidence. Enumerate the active
rulesets whenever this decision is re-evaluated.
