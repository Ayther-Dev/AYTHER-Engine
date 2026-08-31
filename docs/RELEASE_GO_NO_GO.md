# Stability gate: go / no-go decision

**Decision: NO-GO.**

**Date:** 2026-08-30
**Commit evaluated:** `c3866fe` (local `main`)
**Proposed version:** `0.1.0`
**Decider:** engineering, on the evidence below

This is the formal record of the first stable-release decision. Every line is
tied to something that was queried or executed, and the commands are given so
the decision can be re-run rather than trusted.

One of the six substantive criteria is met. The gate does not turn on a
judgement call: three criteria fail on facts that no interpretation softens —
no release candidate has ever been published, the only CI run in this
repository's history failed, and the private vulnerability channel that
`SECURITY.md` instructs reporters to use is switched off.

## Summary

| # | Criterion | Result |
|---|---|---|
| 1 | A release candidate consumed externally on Windows and Linux | **Fail** |
| 2 | No open critical or high defects | **Fail** |
| 3 | All mandatory controls green on the commit to publish | **Fail** |
| 4 | Production keys and rollback procedure operational | **Fail** |
| 5 | Support matrix and compatibility policy published | **Pass** |
| 6 | A private channel for reporting vulnerabilities | **Fail** |
| 7 | A documented go/no-go | **Pass** (this document) |
| 8 | On "go", promote an immutable commit | Not applicable |

## 1. A release candidate consumed externally — FAIL

No release candidate exists. Not "exists but was not consumed": it was never
created.

```console
$ git tag -l                  # (no output)
$ git ls-remote --tags origin # (no output)
$ gh api repos/Ayther-Dev/AYTHER-Engine/releases --jq 'length'
0
```

`docs/BUILD_TEST_RELEASE.md` documents the procedure for publishing
`v0.1.0-rc.1`, and `tools/check_rc_consumer.ps1` is written and works, but the
tag was never pushed, so the release workflow has never run on any commit. The
external consumption that did happen — the out-of-tree package consumers — ran
against a local install tree, not against a published artifact, and only on
Windows.

Consuming a release candidate on Linux is not merely unproven; it is currently
impossible, for the reason in criterion 3.

## 2. No open critical or high defects — FAIL

One high-severity defect is open and confirmed by tooling.

**High — unbounded allocation reachable from untrusted SoundFont input.**
The `decoders` fuzz target reported:

```
==5000== ERROR: libFuzzer: out-of-memory (malloc(4294967295))
SUMMARY: libFuzzer: out-of-memory
```

The reproducer is preserved as CI artifact `fuzz-crashes-decoders`
(`oom-2daa503e168be05745765288223baa675a72bf3d`, not expired). It is 43 bytes:
a `RIFF`/`sfbk` header holding a `LIST`/`INFO` chunk whose `INAM` size field is
`ffffffff`. A 43-byte input drives a request for 4 GiB — a denial of service
from content a pack can carry.

The defect is open at the evaluated commit. The parser sources and the fuzz
target are byte-identical between the failing run's commit and `c3866fe`:

```console
$ git diff --stat 92b2538..HEAD -- core/src/sf2.rs core/src/sf3.rs \
      core/src/sf2_bake.rs fuzz/
# (no output)
```

The allocation site was not localised. The first-party chunk walkers in
`sf2_bake.rs` and `sf3.rs` all bound the declared size against the input length
before slicing, and the sample-header path clamps `start`/`end` with `.min()`,
so the allocation plausibly originates in a decoding dependency rather than in
our own walker. It was deliberately not reproduced locally: provoking a 4 GiB
allocation on a developer workstation risks thrashing the machine for evidence
CI already provides. Localising it is the first step of the fix, not of this
decision.

Beyond that single defect, three release blockers remain open — 2, 4, and 7 in
[Project status](PROJECT_STATUS.md) — and blocker 7 is *"complete security
review of media-decoder limits … and adversarial fuzz coverage"*. The open OOM
is an instance of exactly the review that has not been completed, which is the
strongest available argument that the blocker is correctly marked open.

## 3. All mandatory controls green on the commit to publish — FAIL

Two independent failures here.

**No control has ever been observed green on a recent commit.** Exactly one CI
run exists in this repository's history, and it failed:

```console
$ gh run list --limit 12
failure  33339406053  main  fix(build): select Git Bash for libvpx
```

Six of its sixteen jobs failed: `Linux native ASan`, `Linux native UBSan`,
`Linux native + package consumer`, `Linux native + VPX + package consumer`,
`Windows native + VPX + package consumer`, and `C++ coverage`.

Every Linux failure had one cause: `src/ayther_renderer.cpp` used `std::fmod`
and `std::lround` without including `<cmath>`. Windows compiles this because its
standard-library headers include `<cmath>` transitively; libstdc++ does not. The
source was portable by accident.

**The commit to publish has never been through CI at all.** That run was on
`92b2538`, which is three commits behind, and local `main` has not been pushed:

```console
$ git status -sb
## main...origin/main [ahead 3]
```

The `<cmath>` fix is in `3acfdab`, one of the three unpushed commits, so the
Linux breakage is repaired in the working tree and unvalidated everywhere else.
Local runs on `c3866fe` are green — Rust reports 400 passed, 0 failed, 1
ignored, and the repository-policy gates pass — but a local Windows run is not
the control set. It cannot speak for Linux, and Linux is half of criterion 1.

Regenerating `docs/PUBLIC_API_INDEX.md` during the previous task is itself
evidence of this gap: `gen_api_reference.ps1 -Check` was failing on `main`,
which means the `Repository policy` job would have failed on the next run, and
nothing caught it because there was no next run.

## 4. Production keys and rollback procedure operational — FAIL

Neither is operational.

```console
$ gh api repos/Ayther-Dev/AYTHER-Engine/actions/secrets --jq '.secrets[]?.name'
# (no output)
$ gh api repos/Ayther-Dev/AYTHER-Engine/environments --jq '.environments[]?.name'
# (no output)
```

Artifact signing does not need a stored secret — the release workflow signs
keylessly through GitHub OIDC — so the empty secret list is not itself a defect
there. The gap is the pack trust keys: Hub's operational keys are unprovisioned,
which is why blocker 5 stands as *deferred*. The trust primitive is implemented
and tested; nobody has ever operated it.

The reviewer gate is worse than absent, because it looks present. Release
publication is declared as `environment: release` at `.github/workflows/release.yml:338`,
and `docs/BUILD_TEST_RELEASE.md` states that the job *"waits for a reviewer
before any asset is signed or uploaded."* No environment exists. GitHub creates
a referenced environment on first use with no protection rules, so as currently
configured that job would publish immediately, unreviewed. The documentation
describes a control that would not have engaged.

**No rollback procedure exists.** The term appears twice in the repository, both
times as future work: `BUILD_TEST_RELEASE.md:540` lists rollback instructions
among the things a release must carry, and `ROADMAP.md:55` lists establishing
rollback as a task. There is no procedure for withdrawing a bad release, and no
test of one.

Supporting this, `main` carries no protection at all:

```console
$ gh api repos/Ayther-Dev/AYTHER-Engine/branches/main/protection
{"message":"Branch not protected","status":"404"}
$ gh api repos/Ayther-Dev/AYTHER-Engine/rulesets
[]
```

There is no tag protection either, so a published tag could be moved afterwards
— which would defeat criterion 8's immutability requirement even if the decision
had been "go".

## 5. Support matrix and compatibility policy published — PASS

[Support matrix](SUPPORT_MATRIX.md) states the operating systems,
architectures, compilers, GPU backend, and VPX configurations, and separates
what was verified from what is only configured. [API and
compatibility](API_COMPATIBILITY.md) states the compatibility window per axis
for the release, flat C ABI, pack manifest schema, pack container format,
extension ABI, and SDK C API.

This criterion passes on the existence and accuracy of the documents. It should
not be read as a claim that the *contents* are reassuring: the matrix now
records that Linux has never been green, which is precisely why criteria 1 and 3
fail.

## 6. A private channel for reporting vulnerabilities — FAIL

`SECURITY.md` instructs reporters to *"use the private **Report a vulnerability**
option under GitHub Security Advisories for this repository."* That option is
disabled:

```console
$ gh api repos/Ayther-Dev/AYTHER-Engine/private-vulnerability-reporting
{"enabled":false}
```

A reporter following the published instructions finds no such button. The policy
does provide a fallback — *"if GitHub Security Advisories is not enabled, contact
the maintainers privately"* — but it names no address, so the fallback is not
actionable either. For a repository whose stated priority scope includes
signature validation and memory corruption at the Rust/C++ boundary, there is
currently no working way to report either in private.

This is the cheapest failure on the list to fix: it is one repository setting.

## 7. Documented go/no-go — PASS

This document.

## 8. Promotion of an immutable commit — NOT APPLICABLE

The decision is no-go, so nothing was tagged, promoted, or published. No tag was
created, no workflow was dispatched, and `main` was not pushed.

## What would change the decision

In dependency order, smallest first:

1. **Enable private vulnerability reporting**, and give the fallback in
   `SECURITY.md` a real address. One setting; closes criterion 6.
2. **Push the three pending commits and get a green CI run**, including the
   Linux jobs the `<cmath>` fix repairs. This is the load-bearing step: it turns
   Linux from an intention into evidence and unblocks the Linux half of
   criterion 1.
3. **Fix the decoder OOM**, localise the allocation, add the reproducer to the
   fuzz corpus as a regression, and re-run the decoders target. Closes the high
   defect and advances blocker 7.
4. **Create the `release` environment with required reviewers, protect `main`,
   and add tag protection for `v*`**, so the controls the documentation already
   describes actually engage, and so a published tag cannot move.
5. **Write and rehearse a rollback procedure**, including how a signed artifact
   is withdrawn and how consumers are told.
6. **Publish `v0.1.0-rc.1` and consume it externally on both platforms** using
   `tools/check_rc_consumer.ps1` and its Linux equivalent.
7. **Provision Hub's operational keys** and exercise rotation and revocation
   through a real host, or accept that blocker 5 stays deferred and scope the
   stable release to exclude any operated trust claim.

Steps 1 through 6 are all inside this repository. Step 7 is not, which is why it
is listed last: the stable release should either wait for it or explicitly
narrow its promises rather than let a deferred blocker quietly ride along.

## Re-running this gate

```console
git tag -l && git ls-remote --tags origin
gh api repos/Ayther-Dev/AYTHER-Engine/releases --jq 'length'
gh run list --limit 12
gh api repos/Ayther-Dev/AYTHER-Engine/private-vulnerability-reporting
gh api repos/Ayther-Dev/AYTHER-Engine/branches/main/protection
gh api repos/Ayther-Dev/AYTHER-Engine/rulesets
gh api repos/Ayther-Dev/AYTHER-Engine/environments
git status -sb
```

A future gate should record its own evidence rather than citing this one. The
facts above are true of `c3866fe` on 2026-08-30 and of nothing else.
