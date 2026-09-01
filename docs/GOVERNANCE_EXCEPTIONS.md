# Governance exceptions

**Status:** active exception register

**Last reviewed:** 2026-08-31

This register records temporary deviations from repository governance controls.
An exception permits explicitly scoped operation while a constraint exists; it
does not make the missing control pass, provide independent review, or waive
unrelated release and security gates.

## GOV-2026-001: single-maintainer Code Owner review

| Field | Value |
|---|---|
| Status | Active, temporary |
| Effective date | 2026-08-31 |
| Review deadline | 2026-11-30 |
| Owner | `@Ayther-Dev/maintainers` |
| Scope | Required approving review and required Code Owner review on the `main` ruleset |
| Does not waive | Pull requests, strict required CI, CodeQL findings, immutable release tags, release-environment approval, rollback rehearsal, or any release blocker |

### Constraint and rationale

At the review date, `@Ayther-Dev/maintainers` contains one member,
`@davidlazarte`. GitHub does not permit a pull-request author to approve their
own pull request. Enabling required Code Owner review while that sole member is
both author and only eligible Code Owner would block maintenance without adding
an independent reviewer.

The active `Protect main` ruleset therefore requires changes to enter through a
pull request but temporarily retains:

```text
required_approving_review_count: 0
require_code_owner_review: false
```

This is a capacity constraint, not evidence that self-review is equivalent to
peer review. The exception permits repository maintenance while the team has a
single qualified member. It must never be reported as independent review,
four-eyes approval, separation of duties, or two-person release control.

### Compensating controls

The following controls reduce risk while the exception is active:

- `main` changes must use a pull request and pass the strict `Required CI gate`;
- the `main` ruleset blocks deletion and non-fast-forward updates and has no
  bypass actors;
- `.github/CODEOWNERS` continues to route release-workflow and release-tool
  changes to `@Ayther-Dev/maintainers`, so a future eligible member is visible
  without changing ownership patterns;
- CodeQL runs independently of the author, and open high or critical findings
  remain release blockers even when the workflow itself succeeds;
- the `release` environment creates an explicit, auditable publication
  checkpoint, although approval by the sole maintainer is not independent;
- the `refs/tags/v*` ruleset blocks update and deletion with no bypass actors;
- every stable-release decision must state that independent Code Owner review
  was absent while this exception was active.

For changes to `.github/workflows/release.yml`, `.github/CODEOWNERS`, or the
release tooling owned by CODEOWNERS, the pull-request description must record
the reason, affected release boundary, test evidence, and whether an external
review was obtained. An external comment without repository review authority
is useful feedback but is not an approving Code Owner review.

### Residual risk

The sole maintainer can author and merge a pull request after required status
checks pass. The same person can currently initiate and approve a `release`
environment deployment because `prevent_self_review` is disabled. The audit
trail proves what was changed, tested, and published; it does not prove that a
second person challenged the decision.

This exception is acceptable for ongoing development and release-candidate
preparation. By itself it does not satisfy a stable-release claim of independent
review. The current [go/no-go decision](RELEASE_GO_NO_GO_CURRENT.md) remains
authoritative and must identify this limitation separately from its other
release blockers.

### Expiry and closure

The exception expires at the earliest of:

1. a second qualified maintainer or eligible external Code Owner receives write
   access;
2. opening the final go/no-go for the first supported stable release;
3. 2026-11-30.

Before expiry, either close the exception or record a new time-bounded decision
with current evidence and an explicit residual-risk owner. Silence does not
renew it.

To close the exception:

1. keep the eligible reviewer in the visible CODEOWNERS team or name them in
   `.github/CODEOWNERS`;
2. set `required_approving_review_count` to at least `1`;
3. set `require_code_owner_review` to `true`;
4. dismiss stale approvals after new commits and require approval of the last
   push by someone other than its author;
5. enable `prevent_self_review` on the `release` environment and retain at
   least one independent required reviewer;
6. open a test pull request touching `.github/workflows/release.yml` and retain
   evidence that GitHub blocks merge and publication until the other reviewer
   approves;
7. mark this entry closed with the ruleset, test pull request, reviewer, and
   closure date.

## Review procedure

Every active exception is reviewed at each release go/no-go and at its stated
deadline. The review must confirm that its constraint still exists, its scope
has not expanded, compensating controls remain active, and the residual risk is
still accepted. Closed entries remain in this file as audit history.
