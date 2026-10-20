# Current release gate: go / no-go decision

**Pre-release decision: GO for `v0.1.0-rc.20`.**

**Stable-release decision: NO-GO for `v0.1.0`.**

**Decision date:** 2026-10-20

**Evidence cutoff:** 2026-10-20T12:00:00-03:00

**Candidate identity:** the commit targeted by the annotated tag
`v0.1.0-rc.20`. The tag object records the exact candidate SHA and is the
authoritative binding between this decision and the immutable source revision.

**Evidence baseline before the decision record:**
`5db91f1` (`main` after merging pull request #46, on top of `6f272c8`, the
published candidate `v0.1.0-rc.19`).

**Decider:** the sole maintainer, operating under
[GOV-2026-001](GOVERNANCE_EXCEPTIONS.md#gov-2026-001-single-maintainer-code-owner-review)

This record supersedes the operational GO for `v0.1.0-rc.19`, which was
published successfully as a pre-release. The earlier decisions remain
immutable historical snapshots in this file's history and in
[RELEASE_GO_NO_GO.md](RELEASE_GO_NO_GO.md).

## Why a new candidate

The HD Panorama of "Go to Turtle Town" (Golden Axe) stops short of the screen
edge: at the end of the pan a strip of the backdrop shows through. The camera
rests on a fine offset there, and the background stitcher read whole cells
only, so the last column on screen was never stitched (191 columns where the
screen shows 192) while the content index still had cells in it; those cells
were claimed, hiding the original tiles, and the HD quad, which covers
exactly the stored rectangle, drew nothing over them. Since `rc.19`, `main`
gained:

- pull request #46: a resting camera has its partial column and row
  stitched (a new capture can be one column or row larger than an older one
  of the same stretch, and a camera left of the origin starts at column -1);
  `define_panorama` and pack loading ignore the cells outside the Panorama's
  rectangle, which also protects packs baked before the fix; the per-seek
  `replay_seek` diagnostic moves from Warning to Debug.

## Decision scope

The GO authorizes publishing `v0.1.0-rc.20` as a **pre-release candidate** so
that AYTHER Lab can lock the Engine that stitches the partial edge and
recapture its Panoramas. It does not authorize publishing the stable `v0.1.0`
release.

Stable remains NO-GO because supported-release blockers and the required
rollback rehearsal are not yet closed.

## Evidence supporting the RC GO

| Criterion | Result | Evidence |
|---|---|---|
| Version contract accepts the candidate | **Pass** | `tools/check_release_version.ps1 -Tag v0.1.0-rc.20` passes for prerelease `rc.20` of `0.1.0`; every surface keeps the core `0.1.0`, as in `rc.11` to `rc.19` |
| Required CI on the changes | **Pass** | [Pull request #46](https://github.com/Ayther-Dev/AYTHER-Engine/pull/46): 18 checks green on head `f2243cf` ([CI](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/39359417249)), including the C++ coverage gate, ASan + UBSan, fuzz smoke, clang-format 18, clang-tidy, `cargo audit` and CodeQL; the opt-in GPU job was skipped |
| Required CI on the merged baseline | **Pass by construction** | A push to `main` only configures (it keeps the vcpkg cache) and does not rebuild or retest. `Protect main` requires an up-to-date branch, so `5db91f1` is the tree pull request #46 validated; the release workflow builds and tests the four artifacts again from the tag |
| Open code-scanning findings | **Pass** | GitHub returned no open code-scanning alerts at the evidence cutoff |
| Dependency advisories | **Pass** | `cargo audit --deny warnings` passed on `Cargo.lock` and `fuzz/Cargo.lock` in the CI of #46; no dependency changed since `rc.19` |
| The changes have tests | **Pass** | `tests/unit/bg_capture_window_test.cpp` (the edge rule) and `tests/integration/bg_partial_edge_session_test.cpp` on the new `AYTHER-PAN-SCENARIO` of `tools/test_core`: a resting fine offset stitches the full width, aligned and moving captures are unchanged, a pan left of the origin starts at -1, and cells outside the rectangle are not claimed; without the fix exactly four of its checks fail, among them a strip one column short as in Golden Axe. Locally on the candidate sources: `ctest --preset windows-native` 214/214 (4 skipped by environment), `tools/gen_api_reference.ps1 -Check` and `tools/check_doc_references.ps1` clean |
| Consumer integration | **Pass** | AYTHER Lab `feat/pano-export-progress` drives "Recapturar en su sitio" and the Panorama export end to end over MCP on a copy of the Golden Axe QA project (17/17 checks against `rc.19`); its re-run against these sources, where "Go to Turtle Town" grows to 192 columns, is the Lab's lock gate |
| ABI and package surface | **Pass, unchanged** | No public header declaration, flat C ABI (revision `8`), header inventory, recording format (`.arp` v10) or package change since `rc.19`; `ayther_session.h` changed in comments only |
| `rc.19` release outcome | **Pass** | Published 2026-10-19 as [pre-release `v0.1.0-rc.19`](https://github.com/Ayther-Dev/AYTHER-Engine/releases/tag/v0.1.0-rc.19) |
| Candidate tag is unused | **Pass** | `refs/tags/v0.1.0-rc.20` did not exist at the evidence cutoff |
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

After publication, AYTHER Lab moves its Engine lock to this candidate so that
"Recapturar en su sitio" stitches the full Panorama. A pack baked from the
recaptured strip draws without the strip in the current Runtime; the Runtime
picks up the rectangle guard with its next lock, before the stable `v0.1.0`
gate is re-evaluated.

## Reproducing the pre-tag checks

```text
git rev-parse main
pwsh ./tools/check_release_version.ps1 -Tag v0.1.0-rc.20
gh run list --branch main --limit 12
gh api 'repos/Ayther-Dev/AYTHER-Engine/code-scanning/alerts?state=open'
gh api repos/Ayther-Dev/AYTHER-Engine/environments/release
gh api repos/Ayther-Dev/AYTHER-Engine/rulesets
gh api repos/Ayther-Dev/AYTHER-Engine/git/ref/tags/v0.1.0-rc.20
```

Ruleset identifiers are not treated as stable evidence. Enumerate the active
rulesets whenever this decision is re-evaluated.
