# Current release gate: go / no-go decision

**Pre-release decision: GO for `v0.1.0-rc.23`.**

**Stable-release decision: NO-GO for `v0.1.0`.**

**Decision date:** 2026-11-13

**Evidence cutoff:** 2026-11-13T12:00:00Z

**Candidate identity:** the commit targeted by the annotated tag
`v0.1.0-rc.23`. The tag object records the exact candidate SHA and is the
authoritative binding between this decision and the immutable source revision.

**Evidence baseline before the decision record:**
`3168546` (`main` after merging pull request #55, on top of `1dff21f`, the
base of the published candidate `v0.1.0-rc.22`).

**Decider:** the sole maintainer, operating under
[GOV-2026-001](GOVERNANCE_EXCEPTIONS.md#gov-2026-001-single-maintainer-code-owner-review)

This record supersedes the operational GO for `v0.1.0-rc.22`, which was
published successfully as a pre-release. The earlier decisions remain
immutable historical snapshots in this file's history and in
[RELEASE_GO_NO_GO.md](RELEASE_GO_NO_GO.md).

## Why a new candidate

Since `rc.22`, `main` gained one unified pull request, #55 (spec 008, WP-7,
the Engine side of the Tessera native provider):

- `ObservationSource`, extracted from `RetroRunner` and implemented by the
  Tessera backend over the Tessera API 1.1 observation surface, so `FrameView`
  is built the same way from either provider;
- native Mega Drive sessions through `AytherSession`: the frame pipeline, pack
  substitutions, work RAM, pokes and cheats, rewind, `.arp` takes with
  provenance `tessera`, replay seeks and device audio;
- `tools/parity_bench` filled in: native `dump`, `check` with strict exception
  accounting, identity files and the cross-pack line;
- `dependencies/tessera.lock.json` pins Tessera `v0.2.0-rc.9` by tag and
  package SHA-256.

The pull request also fixed an infinite recursion in the session's `emu_fps`.

AYTHER Lab and Runtime can lock a single Engine that carries the native
provider surface (spec 008, TP-302.4).

## Decision scope

The GO authorizes publishing `v0.1.0-rc.23` as a **pre-release candidate**. It
does not authorize publishing the stable `v0.1.0` release.

Stable remains NO-GO because supported-release blockers and the required
rollback rehearsal are not yet closed.

## Evidence supporting the RC GO

| Criterion | Result | Evidence |
|---|---|---|
| Version contract accepts the candidate | **Pass** | `tools/check_release_version.ps1 -Tag v0.1.0-rc.23` passes for prerelease `rc.23` of `0.1.0`; every surface keeps the core `0.1.0`, as in `rc.11` to `rc.22` |
| Required CI on the change | **Pass** | [Pull request #55](https://github.com/Ayther-Dev/AYTHER-Engine/pull/55): 19 checks green and the opt-in GPU job skipped on head `65727be` (run `43408958113`), after adding tests to lift the C++ changed-line coverage above its 70 % floor |
| Required CI on the merged baseline | **Pass** | The `CI` workflow on `main` (run `43417571066`) at `3168546`. `Protect main` requires an up-to-date branch; the release workflow builds and tests the four artifacts again from the tag |
| Open code-scanning findings | **Pass** | GitHub returned no open code-scanning alerts at the evidence cutoff |
| Dependency advisories | **Pass** | `vcpkg.json` is identical to `rc.22`; `Cargo.lock` and `fuzz/Cargo.lock` only move `rustix` to 1.1.6 (with its `windows-sys` requirement); `cargo audit --deny warnings` ran in the CI of the pull request |
| The change has tests | **Pass, with a stated limit** | New tests cover `observation_source_parity`, the native session (`tessera_native_state`: rewind 590/590, memory 1.18 × gpgx, take provenance and keyframes, 47/47 seeks, PCM continuity), the `AUDIO_MUTE` channel masks on both providers, the native subsystems, `RewindBuffer` and the parity bench (`check`, identities, cross-pack). The release artifacts are built without `AYTHER_ENABLE_TESSERA`, so the native provider is not in the published packages; the tests that need the Tessera package or real ROMs ran in the CI of the pull request and in its local build |
| Consumer integration | **Not yet exercised against this candidate** | Locking Lab and Runtime onto `rc.23` is the consumers' own gate after publication (spec 008, TP-302.4). The `Runtime integration` workflow failed against `rc.22` on Runtime's `vcpkg_manifest` test (`No vcpkg port mapping is defined for Engine dependency 'Tessera'`) because it pinned Runtime `7580a88`, before Runtime learned that Tessera is not a vcpkg port (`a1fc482`). This candidate pins Runtime `500cf88` (its `main`, which contains the fix) |
| ABI and package surface | **Pass, unchanged** | Flat C ABI revision stays `9`. The take format stays `.arp` v11 and `AytherRecording::abi_size()` is unchanged. The installed header manifest (`cmake/AytherPublicHeaders.txt`) is unchanged: `ObservationSource` and `RetroRunner` are internal. `ayther_session.h` and `engine/native_emulation.hpp` change only comments and a private friend declaration |
| `rc.22` release outcome | **Pass** | Published 2026-11-06 as [pre-release `v0.1.0-rc.22`](https://github.com/Ayther-Dev/AYTHER-Engine/releases/tag/v0.1.0-rc.22) |
| Candidate tag is unused | **Pass** | `refs/tags/v0.1.0-rc.23` did not exist at the evidence cutoff |
| Release controls | **Pass with temporary governance exception** | Rulesets `Immutable release tags` and `Protect main` are active; the `release` environment has a branch policy and required reviewers |

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

After publication, AYTHER Lab and Runtime can move their Engine lock to this
candidate. The flat C ABI revision stays at `9` and the take format at `.arp`
v11, so a consumer built against `rc.22` headers needs no source change; it
still rebuilds against the locked package as usual.

## Reproducing the pre-tag checks

```text
git rev-parse main
pwsh ./tools/check_release_version.ps1 -Tag v0.1.0-rc.23
gh run list --branch main --limit 12
gh api 'repos/Ayther-Dev/AYTHER-Engine/code-scanning/alerts?state=open'
gh api repos/Ayther-Dev/AYTHER-Engine/environments/release
gh api repos/Ayther-Dev/AYTHER-Engine/rulesets
gh api repos/Ayther-Dev/AYTHER-Engine/git/ref/tags/v0.1.0-rc.23
```

Ruleset identifiers are not treated as stable evidence. Enumerate the active
rulesets whenever this decision is re-evaluated.
