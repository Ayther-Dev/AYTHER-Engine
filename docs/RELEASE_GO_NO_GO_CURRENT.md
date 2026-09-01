# Current release gate: go / no-go decision

**Pre-release decision: GO for `v0.1.0-rc.3`.**

**Stable-release decision: NO-GO for `v0.1.0`.**

**Decision date:** 2026-09-01

**Evidence cutoff:** 2026-09-01T11:03:46-03:00

**Candidate identity:** the commit targeted by the annotated tag
`v0.1.0-rc.3`. The tag object records the exact candidate SHA and is the
authoritative binding between this decision and the immutable source revision.

**Evidence baseline before the decision record:**
`549294a21881241f9fbd83a25d44c68151e1f2b9` (`main`)

**Decider:** the sole maintainer, operating under
[GOV-2026-001](GOVERNANCE_EXCEPTIONS.md#gov-2026-001-single-maintainer-code-owner-review)

This record supersedes the operational GO for `v0.1.0-rc.2`. That candidate
failed during release configuration before publication and its immutable tag
remains preserved. The earlier assessment of
`d68cfad0cc9619063d407d930a78140ee2d61b0b` and the
[2026-08-30 decision](RELEASE_GO_NO_GO.md) remain immutable historical
snapshots.

## Decision scope

The GO authorizes publishing `v0.1.0-rc.3` as a **pre-release candidate** so
that the release pipeline, artifact verification, external consumption, and
rollback procedure can be exercised. It does not authorize publishing the
stable `v0.1.0` release.

Stable remains NO-GO because supported-release blockers and the required
rollback rehearsal are not yet closed. Evidence produced by this RC may close
part of that gap, but it does not retroactively turn this decision into a
stable-release GO.

## Evidence supporting the RC GO

| Criterion | Result | Evidence |
|---|---|---|
| Version contract accepts the candidate | **Pass** | `tools/check_release_version.ps1 -Tag v0.1.0-rc.3` passes for prerelease `rc.3` of `0.1.0` |
| Required CI on the corrected release baseline | **Pass** | [CI run 33515965518](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/33515965518) passed on `549294a21881241f9fbd83a25d44c68151e1f2b9` |
| CodeQL workflow on the corrected baseline | **Pass** | [CodeQL run 33515965560](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/33515965560) passed on the same SHA |
| Open code-scanning findings | **Pass** | GitHub returned no open code-scanning alerts at the evidence cutoff |
| `rc.2` release outcome | **Failed before publication; remediated by fix-forward** | [Release run 33514264266](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/33514264266) rejected MSVC-style `/pathmap:` under `clang-cl`; no publication job or environment approval was reached, and the immutable `v0.1.0-rc.2` tag was not moved or deleted |
| Windows deterministic-prefix regression | **Pass** | PR [#10](https://github.com/Ayther-Dev/AYTHER-Engine/pull/10) passes Clang's prefix map through `/clang:-ffile-prefix-map=...`; its required CI gate passed both Windows native matrices, including VPX |
| Candidate tag is unused | **Pass** | `refs/tags/v0.1.0-rc.3` did not exist at the evidence cutoff |
| Release controls | **Pass with temporary governance exception** | The `release` environment requires approval and accepts `v*`; immutable tag protection blocks update and deletion |

The final candidate commit is the merge result containing this record. Before
tag creation, required CI and CodeQL must be green on that exact `main` commit.
The annotated tag message must contain both the explicit GO and the full target
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
2. builds and tests all six advertised product/platform artifacts;
3. reproduces each archive byte-for-byte within its build job;
4. verifies the packaged payloads and out-of-tree consumers;
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

After publication, download and verify the released archives on Windows and
Linux, then rehearse the rollback procedure. Record those results before
re-evaluating the stable `v0.1.0` gate.

## Reproducing the pre-tag checks

```text
git rev-parse main
pwsh ./tools/check_release_version.ps1 -Tag v0.1.0-rc.3
gh run list --branch main --limit 12
gh api 'repos/Ayther-Dev/AYTHER-Engine/code-scanning/alerts?state=open'
gh api repos/Ayther-Dev/AYTHER-Engine/environments/release
gh api repos/Ayther-Dev/AYTHER-Engine/rulesets
gh api repos/Ayther-Dev/AYTHER-Engine/git/ref/tags/v0.1.0-rc.3
```

Ruleset identifiers are not treated as stable evidence. Enumerate the active
rulesets whenever this decision is re-evaluated.
