# Current release gate: go / no-go decision

**Pre-release decision: GO for `v0.1.0-rc.22`.**

**Stable-release decision: NO-GO for `v0.1.0`.**

**Decision date:** 2026-11-06

**Evidence cutoff:** 2026-11-06T13:28:00Z

**Candidate identity:** the commit targeted by the annotated tag
`v0.1.0-rc.22`. The tag object records the exact candidate SHA and is the
authoritative binding between this decision and the immutable source revision.

**Evidence baseline before the decision record:**
`7adda8e` (`main` after merging pull request #53, on top of `e14f2ac`, the
base of the published candidate `v0.1.0-rc.21`).

**Decider:** the sole maintainer, operating under
[GOV-2026-001](GOVERNANCE_EXCEPTIONS.md#gov-2026-001-single-maintainer-code-owner-review)

This record supersedes the operational GO for `v0.1.0-rc.21`, which was
published successfully as a pre-release. The earlier decisions remain
immutable historical snapshots in this file's history and in
[RELEASE_GO_NO_GO.md](RELEASE_GO_NO_GO.md).

## Why a new candidate

Since `rc.21`, `main` gained one unified pull request, #53 (spec 008, the
Engine side of the Tessera parity work):

- emulator provenance: the take recording (`.arp`) is version 11 and carries
  `emulator_id`, `emulator_version` and `determinism_contract`; pack manifests
  can carry `[emulator]` and `[identity]` tables (schema stays at 2);
  `AytherSession::emulation_generation()` publishes the generation of a
  session and takes are stamped with it;
- the core probe contract v2: `probe_core_v2` with verdicts, the PE/ELF
  architecture and the capability mask, the installed `ayther_core_probe`
  command, the shared vectors in `share/ayther/probe-vectors/`, a 10 s limit
  and the `crashed` verdict (exit 6);
- `tools/parity_bench`, the skeleton of the Tessera parity bench (`dump`,
  `compare`, `trace-from-arp`);
- `dependencies/tessera.lock.json`: the Tessera commit the Engine builds
  against, the API 1.1 requirement and a CI job that builds the locked commit.

The pull request also fixed a Windows hang in the probe: `LoadLibraryW` on a
file that is not a valid image raised a "Bad Image" system box and waited for
it, which blocked `ayther.contract.core_probe_vectors` on a runner without a
desktop.

The authoring applications (AYTHER Lab, Runtime) can lock a single Engine that
carries the new take format and the probe contract.

## Decision scope

The GO authorizes publishing `v0.1.0-rc.22` as a **pre-release candidate**. It
does not authorize publishing the stable `v0.1.0` release.

Stable remains NO-GO because supported-release blockers and the required
rollback rehearsal are not yet closed.

## Evidence supporting the RC GO

| Criterion | Result | Evidence |
|---|---|---|
| Version contract accepts the candidate | **Pass** | `tools/check_release_version.ps1 -Tag v0.1.0-rc.22` passes for prerelease `rc.22` of `0.1.0`; every surface keeps the core `0.1.0`, as in `rc.11` to `rc.21` |
| Required CI on the change | **Pass** | [Pull request #53](https://github.com/Ayther-Dev/AYTHER-Engine/pull/53): 19 checks green and the opt-in GPU job skipped on head `2a7e4c0` (run `42215373028`), after the Windows probe hang and the core results checker were fixed |
| Required CI on the merged baseline | **Pass** | The `CI` workflow on `main` (run `42223744723`, which on `main` warms the caches) and `CodeQL` succeeded at `7adda8e`. `Protect main` requires an up-to-date branch; the release workflow builds and tests the four artifacts again from the tag |
| Open code-scanning findings | **Pass** | GitHub returned no open code-scanning alerts at the evidence cutoff |
| Dependency advisories | **Pass** | `Cargo.lock`, `fuzz/Cargo.lock` and `vcpkg.json` are identical to `rc.21`; `cargo audit --deny warnings` ran in the CI of the pull request |
| The change has tests | **Pass, with a stated limit** | New tests cover the recording provenance round trip, the emulation generation, the identity contract vectors, the probe vectors (`ayther.contract.core_probe_vectors`, the package consumer check `check_core_probe.cmake`), the parity bench subcommands and the Tessera lock rejections. The three real-ROM Tessera tests (`tessera_checkpoint`, `tessera_pack_test`, `tessera_presentation_test`) report SKIPPED in CI because the Tessera package does not install the `clownmdemu` backend; they ran in the local build of the pull request. The Linux paths of the probe were exercised by the Linux jobs only |
| Consumer integration | **Not yet exercised against this candidate** | The local pre-flight `check-engine-compat.ps1 -Variant plain` against the package built from the branch passed (54 checks, 0 failures, full symbols). Locking Lab and Runtime onto `rc.22` is the consumers' own gate after publication |
| ABI and package surface | **Pass, changed and documented** | Flat C ABI revision stays `9`. `AytherRecording::abi_size()` changes with the `.arp` v11 take (risk R-5): consumers rebuild against these headers. New installed header `ayther/emulation_generation.h`, and `ayther/engine/core_probe.hpp` grows the v2 report; new installed command `ayther_core_probe` and the vectors under `share/ayther/probe-vectors/`, checked by `tools/check_release_payload.ps1`. Older readers cannot open `.arp` v11; files up to v10 still load |
| `rc.21` release outcome | **Pass** | Published 2026-11-02 as [pre-release `v0.1.0-rc.21`](https://github.com/Ayther-Dev/AYTHER-Engine/releases/tag/v0.1.0-rc.21) |
| Candidate tag is unused | **Pass** | `refs/tags/v0.1.0-rc.22` did not exist at the evidence cutoff |
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
candidate. `AytherRecording::abi_size()` changes with the take format
(version 11), so the Lab must rebuild against the headers of this Engine: a Lab
built with the previous `ayther_recording.h` refuses to start instead of
misreading takes. The flat C ABI revision stays at `9`; a consumer that calls
`ayther_pack_verify_all()` still requires it.

## Reproducing the pre-tag checks

```text
git rev-parse main
pwsh ./tools/check_release_version.ps1 -Tag v0.1.0-rc.22
gh run list --branch main --limit 12
gh api 'repos/Ayther-Dev/AYTHER-Engine/code-scanning/alerts?state=open'
gh api repos/Ayther-Dev/AYTHER-Engine/environments/release
gh api repos/Ayther-Dev/AYTHER-Engine/rulesets
gh api repos/Ayther-Dev/AYTHER-Engine/git/ref/tags/v0.1.0-rc.22
```

Ruleset identifiers are not treated as stable evidence. Enumerate the active
rulesets whenever this decision is re-evaluated.
