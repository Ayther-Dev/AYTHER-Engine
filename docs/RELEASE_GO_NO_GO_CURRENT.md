# Current release gate: go / no-go decision

**Pre-release decision: GO for `v0.1.0-rc.14`.**

**Stable-release decision: NO-GO for `v0.1.0`.**

**Decision date:** 2026-10-04

**Evidence cutoff:** 2026-10-04T03:10:00-03:00

**Candidate identity:** the commit targeted by the annotated tag
`v0.1.0-rc.14`. The tag object records the exact candidate SHA and is the
authoritative binding between this decision and the immutable source revision.

**Evidence baseline before the decision record:**
`c4ff2e4ba317466e525017cb7bfdc58a4c739d31` (`main` after merging pull
request #28), which is the published candidate `v0.1.0-rc.13` plus spec 002:
replay QA inspection and the visual representation fixes.

**Decider:** the sole maintainer, operating under
[GOV-2026-001](GOVERNANCE_EXCEPTIONS.md#gov-2026-001-single-maintainer-code-owner-review)

This record supersedes the operational GO for `v0.1.0-rc.13`, which was
published successfully as a pre-release. The `rc.12`, `rc.11`, `rc.10`,
`rc.8`, `rc.7` and
`rc.6` decisions, the earlier `v0.1.0-rc.4` publication and the
[2026-08-30 decision](RELEASE_GO_NO_GO.md) remain immutable historical
snapshots.

## Why a new candidate

Spec 002 gives AYTHER Runtime what replay QA needs to inspect and reproduce a
take, and corrects the representation defects found in the Golden Axe Toma 3:

- the render observation contract (C3, `engine/render_observer.hpp`) and the
  visual state export and restore (C4, `engine/visual_state.hpp`);
- the HD audio state exported in the middle of a take is complete and restores
  (BR-033b);
- the replacement composition rules R1-R9, documented in
  [Replacement composition rules](REPLACEMENT_COMPOSITION.md): membership-only
  pose anchors, per-member depth, the VDP sprite layer with per-pixel priority,
  HD lanes in their plane pass, original-sprite visibility, synchronous texture
  residency, poses without an asset, non-composable frames, and pack
  retirement;
- the `render_probe` tool with its O1 and O2 oracles.

Over the 124 frames of Toma 3, `render_probe` reports O1 identical in 124 of
124 frames without a pack, and O2 clean in 124 of 124 with the pack, settled
and cold (0 of 124 before the change). The Runtime consumes the new contracts
from this candidate.

## Decision scope

The GO authorizes publishing `v0.1.0-rc.14` as a **pre-release candidate** so
that AYTHER Runtime and Lab can consume the spec 002 contracts and the
corrected composition, and the acceptance campaign can run from a tagged
distribution. It does not authorize
publishing the stable `v0.1.0` release.

Stable remains NO-GO because supported-release blockers and the required
rollback rehearsal are not yet closed.

## Evidence supporting the RC GO

| Criterion | Result | Evidence |
|---|---|---|
| Version contract accepts the candidate | **Pass** | `tools/check_release_version.ps1 -Tag v0.1.0-rc.14` passes for prerelease `rc.14` of `0.1.0`; every surface keeps the core `0.1.0`, as in `rc.11` to `rc.13` |
| Required CI on the change | **Pass** | [Pull request #28](https://github.com/Ayther-Dev/AYTHER-Engine/pull/28): 22 checks green on its final head, including the [Required CI gate](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/37179943001), ASan/UBSan, fuzz smokes and CodeQL; the opt-in GPU job was skipped |
| C++ coverage with the software Vulkan run | **Pass** | Pull request #28 added the Lavapipe GPU tests to the coverage job: 193/193 CPU and 21/21 GPU tests; total 80.14% (minimum 50%), changed lines 85.08% (minimum 70%) |
| Required CI on the merged baseline | **Pending at the cutoff** | CI on `c4ff2e4ba317466e525017cb7bfdc58a4c739d31` was in progress; required CI must be green on the merge commit that adds this record before the tag |
| Open code-scanning findings | **Pass** | GitHub returned no open code-scanning alerts at the evidence cutoff |
| The change has tests | **Pass** | Every R1-R9 fix and BR-033b was observed red before its change and green after; the documentation contract `ayther.quality.spec002_docs` covers C3, C4, R1-R9 and the changelog |
| Visual evidence | **Pass** | `render_probe` over Toma 3: O1 124/124 identical, O2 124/124 clean settled and cold; cold and prewarmed images identical byte for byte |
| Performance | **Pass** | P-8 at rest, five interleaved rounds: total p95 15.64 to 16.16 ms with the pack (+3.3%), under the 16.6 ms budget |
| ABI and package surface | **Pass with a recorded gap** | New installed headers `engine/render_observer.hpp` and `engine/visual_state.hpp`; package `0.1.0` unchanged. Spec 002 adds flat C exports (pose, sprite and tween catalog and state functions, `ayther_sha256_hex`) while the flat C ABI revision stays at `7`; revisions 6 and 7 were bumped for added symbols. Whether to bump it to `8` is open for the maintainer and does not block a pre-release |
| `rc.13` release outcome | **Pass** | Published 2026-10-02 as [pre-release `v0.1.0-rc.13`](https://github.com/Ayther-Dev/AYTHER-Engine/releases/tag/v0.1.0-rc.13) |
| Candidate tag is unused | **Pass** | `refs/tags/v0.1.0-rc.14` did not exist at the evidence cutoff |
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

After publication, AYTHER Runtime moves its QA Engine lock to this candidate
(spec 002 BR-174 and BR-175), and the spec 002 acceptance campaign installs
from the tagged distribution before the stable `v0.1.0` gate is re-evaluated.

## Reproducing the pre-tag checks

```text
git rev-parse main
pwsh ./tools/check_release_version.ps1 -Tag v0.1.0-rc.14
gh run list --branch main --limit 12
gh api 'repos/Ayther-Dev/AYTHER-Engine/code-scanning/alerts?state=open'
gh api repos/Ayther-Dev/AYTHER-Engine/environments/release
gh api repos/Ayther-Dev/AYTHER-Engine/rulesets
gh api repos/Ayther-Dev/AYTHER-Engine/git/ref/tags/v0.1.0-rc.14
```

Ruleset identifiers are not treated as stable evidence. Enumerate the active
rulesets whenever this decision is re-evaluated.
