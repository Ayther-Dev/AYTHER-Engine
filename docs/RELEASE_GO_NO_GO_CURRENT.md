# Current release gate: go / no-go decision

**Pre-release decision: GO for `v0.1.0-rc.10`.**

**Stable-release decision: NO-GO for `v0.1.0`.**

**Decision date:** 2026-10-01

**Evidence cutoff:** 2026-10-01T00:55:00-03:00

**Candidate identity:** the commit targeted by the annotated tag
`v0.1.0-rc.10`. The tag object records the exact candidate SHA and is the
authoritative binding between this decision and the immutable source revision.

**Evidence baseline before the decision record:**
`f3066ecf650c` (`main` after merging pull request #20), which is the last
published candidate `v0.1.0-rc.9` (`af2623d536c9`) plus the fix commit
`5e3e966` on `fix/pose-relative-flips`.

**Decider:** the sole maintainer, operating under
[GOV-2026-001](GOVERNANCE_EXCEPTIONS.md#gov-2026-001-single-maintainer-code-owner-review)

This record supersedes the operational GO for `v0.1.0-rc.8`. `v0.1.0-rc.9`
was published on 2026-09-15 as a pre-release without its own decision record;
this record covers the step from `rc.9` to `rc.10`. The `rc.8`, `rc.7` and
`rc.6` decisions, the earlier `v0.1.0-rc.4` publication and the
[2026-08-30 decision](RELEASE_GO_NO_GO.md) remain immutable historical
snapshots.

## Why a new candidate

`v0.1.0-rc.9` cannot tell apart two poses whose only difference is the flip of
one member sprite relative to the others, a case reported by the Lab on
2026-09-30 with Golden Axe's dwarf standing with its head facing its body and
facing away. Per-member flips already travel in the pack, but the pose
matcher used them only as a tiebreak: in a left-right symmetric layout it
dropped the mirror arrangement by comparing positions alone, so a mirrored
instance could resolve to the wrong variant, and `pose_key_of` ignored flips,
so a tween between the two variants never fired. `rc.10` is the fix-forward
candidate: `5e3e966` keeps every mirror for multi-member poses with flips,
ranks candidates by hits, relative flip agreement (neutral for poses without
flips) and absolute agreement, gives mixed relative flips their own in-between
key (`pose_key_with_flips`), and masks live-override flips like the pack
parser. A whole-pose mirror is still a state of the same pose.

## Decision scope

The GO authorizes publishing `v0.1.0-rc.10` as a **pre-release candidate** so
that consumers (AYTHER Lab and AYTHER Runtime) can pin a package whose pose
matcher prefers the flip variant that agrees and tweens between variants, and
so that the release pipeline, artifact verification and external consumption
are exercised again on a fix-forward candidate. It does not authorize
publishing the stable `v0.1.0` release.

Stable remains NO-GO because supported-release blockers and the required
rollback rehearsal are not yet closed.

## Evidence supporting the RC GO

| Criterion | Result | Evidence |
|---|---|---|
| Version contract accepts the candidate | **Pass** | `tools/check_release_version.ps1 -Tag v0.1.0-rc.10` passes for prerelease `rc.10` of `0.1.0` on `f3066ec` |
| Required CI on the change | **Pass** | [Pull request #20](https://github.com/Ayther-Dev/AYTHER-Engine/pull/20): all required checks green, including the [Required CI gate](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/36801603174), Rust and C++ coverage gates, ASan/UBSan, fuzz smokes and CodeQL; the opt-in GPU job was skipped |
| Required CI on the merged baseline | **Pass** | [CI push 36810971724](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/36810971724) and CodeQL 36810971003 passed on `f3066ecf650c`; required CI must also be green on the merge commit that adds this record before the tag |
| Open code-scanning findings | **Pass** | GitHub returned no open code-scanning alerts at the evidence cutoff |
| Matching fix has unit oracles | **Pass** | New tests in `core/src/vram_sprite.rs`: the dwarf flip variants (each form, the mirrored form, a lone variant, flips stored shifted), the mirrored symmetric stack, an unchanged single-member mirror, `pose_key_with_flips` with pinned vectors, a tween between variants, pack parsing of two variants, and stable order against poses without flips; the ones covering defects fail against the `rc.9` logic |
| Local suites on the candidate | **Pass** | `cargo fmt --check`, `cargo clippy -D warnings`, `cargo test --workspace` (415 passed), `gen_notice -Check`, `check_doc_references`, `gen_api_reference -Check`; `windows-native` full build and non-GPU CTest 60/60 with 4 core-dependent tests skipped for lack of the test core (2026-09-30, maintainer's machine) |
| ABI and package surface | **Pass** | No public header or C ABI change (core C ABI revision 7, package `0.1.0`); `git diff --stat v0.1.0-rc.9 f3066ec` touches only `core/src/vram_sprite.rs`, `CHANGELOG.md`, `docs/IDENTITY_SPECIFICATION.md` and `tools/pose_replay_scan/main.cpp` |
| Consumer verification of the fix | **Pending the Lab pin** | AYTHER Lab ships the C++ twin of the resolver with the same rules (Lab pull requests #6 and #7); the parity oracle against this package and the Golden Axe dwarf case run when the Lab pins `rc.10` |
| `rc.9` release outcome | **Pass** | Published 2026-09-15 as [pre-release `v0.1.0-rc.9`](https://github.com/Ayther-Dev/AYTHER-Engine/releases/tag/v0.1.0-rc.9) with 18 assets; pinned by AYTHER Runtime `v0.1.0-beta.6` and by AYTHER Lab |
| Candidate tag is unused | **Pass** | `refs/tags/v0.1.0-rc.10` did not exist at the evidence cutoff |
| Release controls | **Pass with temporary governance exception** | Rulesets `Immutable release tags` and `Protect main` are active; the `release` environment requires the maintainer's approval (`required_reviewers`, `branch_policy`) |

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

After publication, AYTHER Runtime pins the candidate and publishes a matching
beta, and AYTHER Lab pins both in its dependency lock with a parity oracle
between its C++ twin of the pose resolver and this release; the Golden Axe
dwarf case (two standing poses that differ only in the head's flip) is
re-run there and recorded before re-evaluating the stable `v0.1.0` gate.

## Reproducing the pre-tag checks

```text
git rev-parse main
pwsh ./tools/check_release_version.ps1 -Tag v0.1.0-rc.10
gh run list --branch main --limit 12
gh api 'repos/Ayther-Dev/AYTHER-Engine/code-scanning/alerts?state=open'
gh api repos/Ayther-Dev/AYTHER-Engine/environments/release
gh api repos/Ayther-Dev/AYTHER-Engine/rulesets
gh api repos/Ayther-Dev/AYTHER-Engine/git/ref/tags/v0.1.0-rc.10
```

Ruleset identifiers are not treated as stable evidence. Enumerate the active
rulesets whenever this decision is re-evaluated.
