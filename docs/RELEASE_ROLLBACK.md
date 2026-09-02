# Release rollback

**Status:** operational procedure; not yet rehearsed against a published
release candidate

**Last reviewed:** 2026-08-31

This runbook withdraws an AYTHER Engine release candidate or stable release
after a defect is found. It applies to the GitHub release and the four Engine
archives published by `.github/workflows/release.yml`. AYTHER Engine has no updater or
package registry in this repository, so rollback means stopping new downloads,
directing consumers to a known-good version, and publishing a corrected version.
It cannot remove copies that consumers already downloaded.

## Invariants

- Never move, delete, or recreate a `v*` tag. The tag ruleset is the audit
  boundary and every affected tag remains attached to its original commit.
- Never replace assets under an existing tag. Checksums, Sigstore bundles,
  provenance, and SBOM attestations identify the original bytes.
- Never reuse a version. A correction receives the next release or pre-release
  version, such as `v0.1.0-rc.2` after withdrawing `v0.1.0-rc.1`.
- Preserve evidence before removing public assets. A downloaded signature or
  attestation remains valid evidence that GitHub Actions produced those bytes;
  withdrawal does not cryptographically revoke copies already in circulation.
- Do not publish vulnerability details before the security maintainer decides
  coordinated disclosure is safe. Follow `SECURITY.md` for private handling.

## Roles and authority

- The incident lead decides whether to withdraw and records the reason, impact,
  affected artifacts, and replacement plan.
- A repository administrator or release maintainer withdraws the GitHub
  release. The `v*` tag ruleset must not be bypassed.
- A security maintainer owns disclosure when confidentiality, integrity,
  signature, dependency, or memory-safety issues are involved.
- A required reviewer on the `release` environment approves any replacement
  publication after its evidence has been reviewed.

## When to invoke this runbook

Withdraw a release when an artifact is unsafe, corrupt, legally
redistributable only under different terms, incompatible with its advertised
contract, or materially different from its SBOM, checksums, provenance, or
release notes. A failed workflow before the publish job creates a release is
not a rollback: reject or cancel the environment deployment, retain the tag and
workflow logs, fix forward, and use a new version.

## 1. Freeze and record

1. Stop promotion and downstream rollout. Do not approve another `release`
   environment deployment until the scope is known.
2. Record the affected tag, GitHub Actions run, release URL, discovery time,
   reporter, severity, affected platforms and product families, and the last
   known-good tag in a private incident record.
3. Confirm that the remote tag still names the published commit:

```text
git fetch --tags origin
git rev-parse <affected-tag>^{commit}
git ls-remote --tags origin refs/tags/<affected-tag>
```

For an annotated tag, `ls-remote` reports the tag object while `rev-parse`
peels it to the commit. Record both values; do not change either.

## 2. Preserve release evidence

Use a restricted incident directory outside the repository. Do not commit
downloaded release archives or confidential reports.

```text
gh release view <affected-tag> \
  --repo Ayther-Dev/AYTHER-Engine \
  --json databaseId,url,name,tagName,isDraft,isPrerelease,publishedAt,body
gh release download <affected-tag> \
  --repo Ayther-Dev/AYTHER-Engine \
  --dir <restricted-incident-directory>
gh run list --repo Ayther-Dev/AYTHER-Engine \
  --workflow release.yml --limit 20
```

Record the workflow run ID and retain its logs. Verify the downloaded
`CHECKSUMS.sha256`, Sigstore bundles, GitHub attestations, and SBOMs before
removing public access; this distinguishes a bad published artifact from a
later download or storage failure.

## 3. Withdraw public distribution

If the release is unsafe to download, delete the GitHub **release object and
its assets**, but keep the immutable Git tag:

```text
gh release delete <affected-tag> \
  --repo Ayther-Dev/AYTHER-Engine \
  --yes
```

Do **not** pass `--cleanup-tag`. After deletion, verify both conditions:

```text
gh release view <affected-tag> --repo Ayther-Dev/AYTHER-Engine
git ls-remote --tags origin refs/tags/<affected-tag>
```

The first command must report that no release exists; the second must still
return the tag. If evidence must remain public and the assets are safe to keep
available, an administrator may instead mark the release title as
`WITHDRAWN — <tag>` and prepend a warning to its notes. That is an advisory
notice, not a complete withdrawal, because the assets remain downloadable.

## 4. Notify consumers

Publish a notice through the release communication channels and notify AYTHER
Runtime, SDK, Play, and Hub maintainers directly. The public notice must state:

- the affected tag and artifact families/platforms;
- whether exploitation or data exposure is known, without disclosing unsafe
  detail prematurely;
- that downloads have been withdrawn and the tag remains only as an immutable
  historical identifier;
- the last known-good version and its expected checksums, or that no safe
  fallback exists;
- the required consumer action: stop rollout, remove cached affected assets,
  pin the known-good version, and verify it with
  `tools/verify_release_artifact.ps1`;
- where and when the next update will be published.

The repository cannot revoke already downloaded keyless signatures. Consumers
must treat the withdrawal notice and affected-version list as the revocation
signal. If a pack trust key is involved, execute the rotation and revocation
procedure from `docs/PACK_SECURITY_MODEL.md` in addition to this runbook.

## 5. Fix forward

1. Reproduce the defect and add the narrowest regression test or policy check
   that would have blocked the affected release.
2. Apply the fix through the protected `main` branch and obtain the required CI
   result and the review required by current governance. While
   [GOV-2026-001](GOVERNANCE_EXCEPTIONS.md#gov-2026-001-single-maintainer-code-owner-review)
   is active, record explicitly that no independent Code Owner review occurred.
3. Update versions and release notes. Select a new tag; never reuse the affected
   version.
4. Run the local version-contract check, then create and push a new annotated
   tag:

```text
pwsh tools/check_release_version.ps1 -Tag <replacement-tag>
git tag -a <replacement-tag> -m 'AYTHER <replacement-tag>'
git push origin <replacement-tag>
```

5. Review the complete replacement evidence before approving its protected
   publish job. Download and verify at least one artifact from every advertised
   platform and product family.
6. Update the withdrawal notice with the replacement release URL. Do not restore
   or recreate the deleted release under the affected tag.

## 6. Close and rehearse

Close the incident only after public downloads are unavailable, the immutable
tag is confirmed unchanged, consumers have acknowledged the notice, and the
replacement or supported fallback has been independently verified. Record a
timeline, affected digests, commands executed, approvers, notification links,
and follow-up actions.

Before the first stable release, rehearse this procedure with an explicitly
expendable release candidate. The rehearsal must prove that:

1. environment approval can be withheld or cancelled before publication;
2. a published release and its assets can be removed without deleting its tag;
3. the affected tag cannot be updated or deleted under the active ruleset;
4. the last known-good artifact can be downloaded and independently verified;
5. consumer notification reaches every owning repository or team;
6. a corrected release can be issued only under a new version.

Attach the rehearsal evidence to the release go/no-go record. Documentation
alone does not make the rollback control operational.
