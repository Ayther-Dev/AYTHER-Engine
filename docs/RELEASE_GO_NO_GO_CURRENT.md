# Current release gate: go / no-go decision

**Pre-release decision: GO for `v0.1.0-rc.18`.**

**Stable-release decision: NO-GO for `v0.1.0`.**

**Decision date:** 2026-10-18

**Evidence cutoff:** 2026-10-18T12:30:00-03:00

**Candidate identity:** the commit targeted by the annotated tag
`v0.1.0-rc.18`. The tag object records the exact candidate SHA and is the
authoritative binding between this decision and the immutable source revision.

**Evidence baseline before the decision record:**
`279262e5852f9c9f2b654f63f13bb3cdd47f300e` (`main` after merging pull requests #40 and #41, on top of
`4ed2310f`, the published candidate `v0.1.0-rc.17`).

**Decider:** the sole maintainer, operating under
[GOV-2026-001](GOVERNANCE_EXCEPTIONS.md#gov-2026-001-single-maintainer-code-owner-review)

This record supersedes the operational GO for `v0.1.0-rc.17`, which was
published successfully as a pre-release. The `rc.16`, `rc.15`, `rc.14`,
`rc.13`, `rc.12`, `rc.11`, `rc.10`, `rc.8`, `rc.7` and `rc.6` decisions, the
earlier `v0.1.0-rc.4` publication and the
[2026-08-30 decision](RELEASE_GO_NO_GO.md) remain immutable historical
snapshots.

## Why a new candidate

AYTHER Lab's element index (spec 007) needs an Engine it can lock, and the
takes it writes need a Runtime that loads them. Since `rc.17`, `main` gained:

- pull request #40, the Golden Axe music campaign of 2026-10-13 to 10-15:
  Sequence anchoring rules 5 and 6, the FM signature without carrier total
  level (detector revision 3), `.arp` v9 with the detector's YM2612 shadow,
  HD audio that stops and resumes with its gate, `ayther_recording.h`
  installed with `AytherRecording::abi_size()`, and sprites clipped to the
  rows the VDP draws them on (Sonic 1 title);
- pull request #41, spec 007: the element record stream
  (`ayther_element_record.h`), reconstruction of an element without its take
  on the CPU and through Vulkan (`ayther_element_scene.h`,
  `AytherRenderer::export_elements`), and `.arp` v10 with a stable `take_id`
  (`patch_take_id`, `peek_take_id`);
- `zip` 9.0.5 for RUSTSEC-2026-0367, -0368 and -0369, which made the
  `cargo audit` gate fail on every branch.

## Decision scope

The GO authorizes publishing `v0.1.0-rc.18` as a **pre-release candidate** so
that AYTHER Lab and AYTHER Runtime can lock the Engine of the element index and
of `.arp` v10. It does not authorize publishing the stable `v0.1.0` release.

Stable remains NO-GO because supported-release blockers and the required
rollback rehearsal are not yet closed.

## Evidence supporting the RC GO

| Criterion | Result | Evidence |
|---|---|---|
| Version contract accepts the candidate | **Pass** | `tools/check_release_version.ps1 -Tag v0.1.0-rc.18` passes for prerelease `rc.18` of `0.1.0`; every surface keeps the core `0.1.0`, as in `rc.11` to `rc.17` |
| Required CI on the changes | **Pass** | [Pull request #40](https://github.com/Ayther-Dev/AYTHER-Engine/pull/40): 22 checks green on head `af2ba95` ([CI](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/39131533513); the Windows native job was rerun once after `audio_qa_frame_instrumentation_cost` measured one pair 5.3 % over its 5 % p95 budget on a shared runner). [Pull request #41](https://github.com/Ayther-Dev/AYTHER-Engine/pull/41): 22 checks green on head `658a835` ([CI](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/39131549584)), including the C++ coverage gate, ASan/UBSan, fuzz smokes, clang-format 18, clang-tidy, `cargo audit` and CodeQL; the opt-in GPU job was skipped |
| Required CI on the merged baseline | **Pass** | [CI](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/39139738731) on `279262e`; required CI must also be green on the merge commit that adds this record before the tag |
| Open code-scanning findings | **Pass** | GitHub returned no open code-scanning alerts at the evidence cutoff |
| Dependency advisories | **Pass** | `cargo audit --deny warnings` passes on `Cargo.lock` and `fuzz/Cargo.lock` with an empty exception list after `zip` 9.0.5 |
| The changes have tests | **Pass** | Locally on the candidate sources: `cargo test --workspace` 454 passed (1 ignored), `cargo clippy -D warnings` clean, `ctest --preset windows-native` 212/212 on `658a835`, and the GPU suite green on the spec 007 sources including the GPU suite (`element_scene_gpu_test`: the Vulkan reconstruction equals the CPU one pixel for pixel) |
| Consumer integration | **Pass** | AYTHER Lab `feat/element-index` built against the spec 007 Engine sources (before the formatting and `zip` commits): non-GPU suite 81/81, element index of a whole Sonic 1 take (2985 frames, 1268 elements, 0 hash mismatches) and previews identical with the take loaded, renamed or deleted. AYTHER Runtime `feat/take-format-v10`: `ayther_audio_qa check` replays a v10 take whole (2985 of 2985 inputs) |
| ABI and package surface | **Pass with a recorded change** | Flat C ABI revision `7` → `8` (additive `ayther_sprite_pattern_hash`), which also closes the decision open since `rc.14`. `AytherRecording` gains `take_id`, so its `abi_size()` changes: a consumer compiled against an older `ayther_recording.h` is rejected by its own guard and must rebuild. Takes of format 10 do not load in an Engine before `rc.18`. Package `0.1.0` unchanged |
| `rc.17` release outcome | **Pass** | Published 2026-10-08 as [pre-release `v0.1.0-rc.17`](https://github.com/Ayther-Dev/AYTHER-Engine/releases/tag/v0.1.0-rc.17) |
| Candidate tag is unused | **Pass** | `refs/tags/v0.1.0-rc.18` did not exist at the evidence cutoff |
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

After publication, AYTHER Lab and AYTHER Runtime move their Engine locks (and
the Runtime its QA Engine lock) to this candidate, so that takes of format 10
written by the Lab load in the Runtime that replays and plays them, before the
stable `v0.1.0` gate is re-evaluated.

## Reproducing the pre-tag checks

```text
git rev-parse main
pwsh ./tools/check_release_version.ps1 -Tag v0.1.0-rc.18
gh run list --branch main --limit 12
gh api 'repos/Ayther-Dev/AYTHER-Engine/code-scanning/alerts?state=open'
gh api repos/Ayther-Dev/AYTHER-Engine/environments/release
gh api repos/Ayther-Dev/AYTHER-Engine/rulesets
gh api repos/Ayther-Dev/AYTHER-Engine/git/ref/tags/v0.1.0-rc.18
```

Ruleset identifiers are not treated as stable evidence. Enumerate the active
rulesets whenever this decision is re-evaluated.
