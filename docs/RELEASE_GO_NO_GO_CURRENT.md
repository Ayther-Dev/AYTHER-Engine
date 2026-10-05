# Current release gate: go / no-go decision

**Pre-release decision: GO for `v0.1.0-rc.16`.**

**Stable-release decision: NO-GO for `v0.1.0`.**

**Decision date:** 2026-10-05

**Evidence cutoff:** 2026-10-05T17:00:00-03:00

**Candidate identity:** the commit targeted by the annotated tag
`v0.1.0-rc.16`. The tag object records the exact candidate SHA and is the
authoritative binding between this decision and the immutable source revision.

**Evidence baseline before the decision record:**
`ff4b233c583a6a091052ee969b9bdddba4435757` (`main` after merging pull
request #35, on top of `ba640528f36e147ff8b476ce6fa1fcf94cf2852b`, the
merge of pull request #33), which is the published candidate `v0.1.0-rc.15`
plus the render findings of the spec 002 visual review (R8 by bands,
recognition-only Cuadros in the pack format and overlay reporting) and of the
whole-take review (the display-enable bit, one-shot register writes and the
O2 oracle).

**Decider:** the sole maintainer, operating under
[GOV-2026-001](GOVERNANCE_EXCEPTIONS.md#gov-2026-001-single-maintainer-code-owner-review)

This record supersedes the operational GO for `v0.1.0-rc.15`, which was
published successfully as a pre-release. The `rc.14`, `rc.13`, `rc.12`, `rc.11`, `rc.10`,
`rc.8`, `rc.7` and
`rc.6` decisions, the earlier `v0.1.0-rc.4` publication and the
[2026-08-30 decision](RELEASE_GO_NO_GO.md) remain immutable historical
snapshots.

## Why a new candidate

The visual review of Runtime beta.10 with Engine `rc.15` found two render
defects on Golden Axe Toma 3:

- the HD background flickered while a sign was written and before enemy
  groups (frames 1568-1690, 4062-4066 and 4742-4757): R8 dropped every HD
  layer of a frame with raster writes in mid-screen. Under decision DI-17 a
  frame now loses HD only in the line bands those writes touch, found from the
  core's raster journal and, for pattern writes, from the core's
  recomposition; a sprite replacement that touches a band is not applied, so
  an element never shows as original and as replacement at once (RF-10.1);
- the title overlays never appeared from the baked pack: the pack format
  dropped Cuadros without an asset, which only gate overlays (DI-18). The
  format now keeps them, the pack exposes each overlay's stack index, and the
  render observation (contract 1.1) reports the stack's layers and each
  overlay's gate and draw. The Lab bake and the Runtime stack are specified
  separately; the Golden Axe pack must be re-baked for the overlays to show.

The whole-take review of that change found one more defect, fixed by pull
request #35:

- at the start of Toma 3 (frame 2) the game turns the display off in
  mid-frame while VRAM still holds the Stage 1 maps, and the frame composed
  whole: the Stage 1 HD Panorama over a black screen with the pack, the
  original background without it (O1). A frame that ends with the display off
  is now the core's image with no HD lane, and the lines before a register's
  first write in mid-screen take the value the previous frame ended with. The
  O2 oracle accepts pose hand-offs (the 116 `transition` frames were all
  hand-offs) and its invariant 6a reads the session's R5 verdict.

## Decision scope

The GO authorizes publishing `v0.1.0-rc.16` as a **pre-release candidate** so
that AYTHER Runtime and Lab can consume the render corrections and the pack
format from a tagged distribution, and the spec 002 acceptance campaign can
run from it. It does not authorize
publishing the stable `v0.1.0` release.

Stable remains NO-GO because supported-release blockers and the required
rollback rehearsal are not yet closed.

## Evidence supporting the RC GO

| Criterion | Result | Evidence |
|---|---|---|
| Version contract accepts the candidate | **Pass** | `tools/check_release_version.ps1 -Tag v0.1.0-rc.16` passes for prerelease `rc.16` of `0.1.0`; every surface keeps the core `0.1.0`, as in `rc.11` to `rc.15` |
| Required CI on the changes | **Pass** | [Pull request #33](https://github.com/Ayther-Dev/AYTHER-Engine/pull/33) ([Required CI gate](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/37340500594)): 22 checks green on its final head, including the C++ coverage gate with the software Vulkan run (199/199 CPU and 23/23 GPU tests; total 80.49%, changed lines 84.75%), ASan/UBSan, fuzz smokes and CodeQL; the opt-in GPU job was skipped. [Pull request #35](https://github.com/Ayther-Dev/AYTHER-Engine/pull/35): 22 checks green on its head `718b02f` ([Required CI gate](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/37356767069)), including the C++ coverage gate, ASan/UBSan, fuzz smokes and CodeQL; the opt-in GPU job was skipped. A first ASan run failed only the timing budget of `audio_qa_frame_instrumentation_cost` (p99 of one pair of three) and passed on re-run |
| Required CI on the merged baseline | **Pending at the cutoff** | [CI](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/37364851252) on `ff4b233c583a6a091052ee969b9bdddba4435757` was in progress (its push workflow passed); required CI must be green on the merge commit that adds this record before the tag |
| Open code-scanning findings | **Pass** | GitHub returned no open code-scanning alerts at the evidence cutoff |
| The changes have tests | **Pass** | Each change was observed red before it and green after: `raster_bands_test`, `raster_band_test` (GPU, including a replacement across a band's edge and a frame with the display off), `render_probe_o3_test`, `elements_toml_test` (recognition-only Cuadro), `pack_overlay_gate_test` (synthetic pack: the gate opens from the baked pack), `overlay_layer_report_test` (GPU), the contract 1.1 cases of the builder and record tests, and for pull request #35 `frame_composability_test`, `display_enable_session_test` (the test core's display scenario), `render_probe_o2_test` (hand-offs) and `framebuffer_judge_test` (the published R5 verdict). Locally: 200/200 CPU tests (4 skipped without the optional core) and 23/23 GPU tests |
| Visual evidence (Toma 3, current pack) | **Pass with recorded residue** | The full-frame flicker is gone: mean frame-to-frame change on entering and leaving the windows drops from 62-81 to 0-19 (the core's own change is 0.6-21). Bands remain for one frame in 643, 647, 1572, 1621, 1645 and 1690, where the core drew with patterns the frame no longer has. O2: no violation in CV-5 and the three windows |
| Whole take, oracles O1-O3 (Toma 3, 7892 frames) | **Pass with recorded residue** | After pull request #35: O2 has no violation (it had 125 `transition` violations in 116 frames, all pose hand-offs); O3 has 294 frames with an unexplained block (296 before: frames 2-3 are gone, every other row is unchanged), namely the residual bands above and 282 frames with 1-9 blocks from pose changes; O1 without a pack fails in 2 frames (3 before: frame 2 is fixed), 647 (282 pixels, two lines below a band) and 4757 (10 696 pixels, lines 0-88 drawn with an hscroll value written once in mid-screen), both residuals of R8 by bands present since `ba64052`. Frames 288-289, 1364-1415, CV-5 and the windows 1568-1690, 4062-4066 and 4742-4757 compose pixel-identically before and after the fix. The residue is left to the spec 002 coordinator and does not block a pre-release |
| ABI and package surface | **Pass with a recorded gap** | Render observation contract 1.1 is additive (a 1.0 consumer is served); `FrameView` (including `sprite_occ_core_drawn` and `scene_dirty` bit 3, display off) and `PackOverlay` gain fields; the pack format accepts Cuadros without an asset, which earlier readers drop as before; package `0.1.0` unchanged. The open decision from `rc.14` stands: spec 002 added flat C exports while `AYTHER_CORE_C_ABI_REVISION` stays at `7` |
| `rc.15` release outcome | **Pass** | Published 2026-10-04 as [pre-release `v0.1.0-rc.15`](https://github.com/Ayther-Dev/AYTHER-Engine/releases/tag/v0.1.0-rc.15) |
| Candidate tag is unused | **Pass** | `refs/tags/v0.1.0-rc.16` did not exist at the evidence cutoff |
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
and stacks overlays at their index, the Lab bakes recognition-only Cuadros and
overlay indices, the Golden Axe pack is re-baked, and the spec 002 acceptance
campaign installs from the tagged distribution before the stable `v0.1.0`
gate is re-evaluated.

## Reproducing the pre-tag checks

```text
git rev-parse main
pwsh ./tools/check_release_version.ps1 -Tag v0.1.0-rc.16
gh run list --branch main --limit 12
gh api 'repos/Ayther-Dev/AYTHER-Engine/code-scanning/alerts?state=open'
gh api repos/Ayther-Dev/AYTHER-Engine/environments/release
gh api repos/Ayther-Dev/AYTHER-Engine/rulesets
gh api repos/Ayther-Dev/AYTHER-Engine/git/ref/tags/v0.1.0-rc.16
```

Ruleset identifiers are not treated as stable evidence. Enumerate the active
rulesets whenever this decision is re-evaluated.
