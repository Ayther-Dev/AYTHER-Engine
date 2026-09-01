# Current stability gate: go / no-go decision

**Decision: NO-GO for stable release `v0.1.0`.**

**Assessment date:** 2026-08-31

**Evidence cutoff:** 2026-08-31T23:52:17-03:00

**Remote commit evaluated:** `d68cfad0cc9619063d407d930a78140ee2d61b0b`
(`main`)

**Decider:** engineering, on the evidence below

This is the current release decision until a newer dated record supersedes it.
It evaluates the remote `main` commit and GitHub repository settings observed at
the evidence cutoff. The rollback runbook and this decision are introduced by
the current documentation change; neither counts as exercised release evidence.
The [2026-08-30 decision](RELEASE_GO_NO_GO.md) remains an immutable historical
snapshot.

## Summary

| # | Criterion | Result | Decisive evidence |
|---|---|---|---|
| 1 | All supported-release blockers closed | **Fail** | Three blockers remain open and two are deferred outside this repository |
| 2 | No open critical or high security findings | **Fail** | CodeQL has one open critical and one open high alert on the evaluated commit |
| 3 | Required CI green on the exact commit | **Pass** | CI run `33461455942` and CodeQL run `33461455802` completed successfully on `d68cfad0` |
| 4 | Protected release control plane | **Temporary exception** | Hosting controls are active; independent approving and Code Owner review are absent under `GOV-2026-001` |
| 5 | Reproducible candidate published and externally consumed | **Fail** | GitHub has no tags and no releases; the release workflow has never published its six-artifact matrix |
| 6 | Support and compatibility scope published | **Pass** | `SUPPORT_MATRIX.md` and `API_COMPATIBILITY.md` state the supported and unsupported scope |
| 7 | Private vulnerability channel operational | **Pass** | Private vulnerability reporting is enabled; no repository advisory is waiting in triage |
| 8 | Rollback operational | **Fail** | A runbook now exists, but it has not been rehearsed against a published release candidate |

Criteria 1, 2, 5, and 8 independently prevent a stable release. Criterion 4 is
an accepted, time-bounded operating exception, not a passing independent-review
control. A successful workflow run is not sufficient to override any criterion.

## 1. Supported-release blockers — FAIL

[Project status](PROJECT_STATUS.md#release-blockers) records eight blockers:
three closed, three open, and two deferred.

- Blocker 2 remains open because the real-emulator oracles require a
  developer-supplied fork core and have not been exercised in protected CI.
- Blocker 4 remains open because no automated symbol or ABI-layout baseline
  compares a candidate with its predecessor.
- Blocker 7 remains open because the wider security review is incomplete and
  CodeQL currently reports critical and high findings.
- Blockers 1 and 5 depend on first-party frontends and Hub operational keys
  owned outside this repository. Deferred does not mean passed.

A stable release requires all supported-release blockers to be closed or an
explicitly approved change to the advertised stable scope. Neither condition is
met.

## 2. Critical and high security findings — FAIL

The CodeQL workflow completed successfully, which proves that analysis ran; it
does not mean that analysis found no defects. Two alerts are open on the exact
evaluated `main` commit:

| Severity | Alert | Location |
|---|---|---|
| Critical | `cpp/unsigned-difference-expression-compared-zero` ([alert 2](https://github.com/Ayther-Dev/AYTHER-Engine/security/code-scanning/2)) | `tools/mute_replacement_probe/main.cpp:348` |
| High | `cpp/wrong-type-format-argument` ([alert 1](https://github.com/Ayther-Dev/AYTHER-Engine/security/code-scanning/1)) | `tools/sound_mailbox_probe/main.cpp:262` |

At the cutoff there were no open GitHub issues, Dependabot alerts, secret
scanning alerts, or private advisories in triage. That does not neutralize the
two open CodeQL findings. They must be fixed or explicitly dismissed with a
reviewed false-positive rationale before the gate can pass.

## 3. Required CI on the evaluated commit — PASS

Remote `main` was
`d68cfad0cc9619063d407d930a78140ee2d61b0b`. The complete
[CI run 33461455942](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/33461455942)
passed on that SHA, including:

- repository policy and the aggregate `Required CI gate`;
- Rust quality on Windows and Linux;
- Windows and Linux headless builds;
- Windows and Linux native package consumers, with and without VPX;
- ASan, UBSan, Rust and C++ coverage;
- the packs, decoders, and FFI fuzz-smoke jobs.

The ordinary GPU matrix was skipped by design and remains an explicit opt-in
hardware gate; the skip was not represented as a successful GPU run. The
[CodeQL run 33461455802](https://github.com/Ayther-Dev/AYTHER-Engine/actions/runs/33461455802)
also completed successfully on the SHA, subject to the open findings above.

## 4. Protected release control plane — TEMPORARY EXCEPTION

The following hosting controls were present and active at the cutoff:

- the `release` environment has a required reviewer and a custom tag policy
  matching `v*`;
- the `Immutable release tags` ruleset targets `refs/tags/v*`, blocks updates
  and deletions, has no bypass actors, and reports that the current user can
  never bypass it;
- the `Protect main` ruleset blocks deletion and non-fast-forward updates and
  requires the strict `Required CI gate` status;
- `.github/CODEOWNERS` exists on remote `main`; the visible
  `@Ayther-Dev/maintainers` team has the `maintain` repository role.

The active `Protect main` pull-request rule has:

```text
required_approving_review_count: 0
require_code_owner_review: false
```

`@Ayther-Dev/maintainers` currently has one member. Requiring that sole Code
Owner to approve their own pull request would not create independent review and
would block maintenance because GitHub does not allow an author to approve
their own pull request. [GOV-2026-001](GOVERNANCE_EXCEPTIONS.md#gov-2026-001-single-maintainer-code-owner-review)
therefore accepts the missing approval requirements temporarily.

The exception does not convert CODEOWNERS routing, CI, or self-approval of the
`release` environment into independent review. A release workflow change can
merge after status checks without another maintainer's approval, and the
deployment initiator can approve their own deployment
(`prevent_self_review: false`). This must be recorded as a single-maintainer
decision, not as four-eyes approval or separation of duties. The exception
expires when an eligible second reviewer exists, before the first supported
stable release, or on 2026-11-30, whichever happens first.

## 5. Published and consumed release candidate — FAIL

GitHub returned an empty tag list and an empty release list. Consequently:

- `.github/workflows/release.yml` has never run from a `v*` tag;
- its six platform/product artifacts have never been published together;
- reproducibility, checksums, Sigstore bundles, provenance, and signed SBOM
  attestations have never been observed on a real GitHub release;
- no published archive has been consumed externally on Windows and Linux.

Local package-consumer tests are strong build evidence, but they are not a
substitute for downloading and verifying the assets that users receive.

## 6. Published support and compatibility scope — PASS

[Support matrix](SUPPORT_MATRIX.md) distinguishes verified configurations from
configured or pending ones. [API and compatibility](API_COMPATIBILITY.md)
defines the release, ABI, protocol, schema, and deprecation windows. This
criterion passes on documentation accuracy, not on readiness of every listed
configuration.

## 7. Private vulnerability reporting — PASS

GitHub private vulnerability reporting is enabled. Security researchers can
use the private reporting channel described by `SECURITY.md`. At the cutoff no
repository security advisory was in triage.

## 8. Rollback — FAIL

The [release rollback runbook](RELEASE_ROLLBACK.md) now defines evidence
preservation, release-asset withdrawal without tag deletion, consumer
notification, fix-forward publication, and closure criteria. It is explicitly
marked as not rehearsed. No release exists against which to prove that assets
can be withdrawn while the immutable tag remains, that consumers receive the
notice, or that a replacement is issued only under a new version.

Documentation closes the design gap; only a recorded rehearsal closes the
operational gate.

## What changes this decision

In dependency order:

1. Fix or review-dismiss CodeQL alerts 1 and 2, then obtain green CI and CodeQL
   on the resulting `main` SHA.
2. Close or explicitly re-evaluate `GOV-2026-001`. Once an eligible second
   reviewer exists, require at least one approval and Code Owner review in the
   `main` ruleset and prevent self-review in the `release` environment. No
   decision may describe the temporary single-maintainer path as independent
   review.
3. Close blockers 2, 4, and 7. Resolve blockers 1 and 5 through their owning
   products or formally narrow the proposed stable scope with explicit review.
4. Publish a new release candidate from an immutable `v*` tag and retain the
   complete six-artifact build, signature, SBOM, provenance, and approval
   evidence.
5. Download and consume that candidate independently on Windows and Linux.
6. Rehearse `RELEASE_ROLLBACK.md`, preserving the tag while withdrawing the
   release and proving consumer notification and fix-forward behavior.
7. Run this gate again against the exact commit proposed for stable release.

## Reproducing the hosting checks

Run these read-only queries with GitHub CLI access to the repository:

```text
gh api repos/Ayther-Dev/AYTHER-Engine/commits/main --jq '.sha'
gh run list --repo Ayther-Dev/AYTHER-Engine --limit 12
gh api repos/Ayther-Dev/AYTHER-Engine/private-vulnerability-reporting
gh api repos/Ayther-Dev/AYTHER-Engine/environments/release
gh api repos/Ayther-Dev/AYTHER-Engine/environments/release/deployment-branch-policies
gh api repos/Ayther-Dev/AYTHER-Engine/rulesets
gh api repos/Ayther-Dev/AYTHER-Engine/rulesets/21977273
gh api repos/Ayther-Dev/AYTHER-Engine/rulesets/21928058
gh api 'repos/Ayther-Dev/AYTHER-Engine/code-scanning/alerts?state=open'
gh api repos/Ayther-Dev/AYTHER-Engine/releases
gh api repos/Ayther-Dev/AYTHER-Engine/tags
```

Ruleset IDs are evidence from this assessment, not stable API identifiers. If a
ruleset is recreated, enumerate the rulesets first and query the replacement
ID.
