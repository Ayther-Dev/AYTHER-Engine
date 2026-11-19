# Security Policy

## Supported versions

AYTHER Engine has no stable release. Every published artifact is a
`0.1.0-rc.N` pre-release, and fixes land on `main` and ship in the next
candidate; earlier candidates are not patched. Reports are evaluated against
`main`. A version matrix will replace this section after the first stable
release.

No security-support lifetime and no operated AYTHER Hub trust registry are
offered. The implemented production trust policy and the remaining operational
gaps are documented in [Pack and security model](docs/PACK_SECURITY_MODEL.md).

## Reporting a vulnerability

Do not open a public issue for an unpatched vulnerability.

Use **Report a vulnerability** under the repository's **Security** tab (GitHub
private vulnerability reporting, enabled for this repository). Include whenever
possible:

- affected version, tag, or commit;
- platform and toolchain;
- description and impact;
- minimal reproduction steps;
- a proof of concept without sensitive data;
- known mitigations;
- whether you can validate a fix.

## Priority scope

These areas are especially sensitive:

- signature and integrity validation of `.ay` packs, including acceptance of
  unknown, expired, revoked, out-of-scope, or development signing keys in a
  production policy;
- archive path ambiguity, path traversal, or writes outside the expected
  directory;
- decompression bombs and bypasses of container or decoded-resource limits;
- memory corruption at the Rust/C++ boundary, and incorrect ownership or
  lifetime of FFI handles;
- unauthorized execution through scripts or pack content;
- unsafe loading of emulator cores or other dynamic libraries;
- denial of service through malformed packs, takes (`.arp`), or media files;
- exposure of keys, certificates, or private data;
- the release pipeline: checksums, Sigstore bundles, provenance, and SBOMs that
  do not match the published bytes.

The optional Tessera native provider is a separate repository and package.
Report a defect in Tessera itself to its maintainers privately; report here
when the defect is in how the Engine loads, configures, or trusts it.

## Response process

The maintainers acknowledge receipt when possible, assess scope and severity,
prepare a fix, and coordinate disclosure. Response times depend on the issue's
complexity and the maintainers' availability. Information that could enable
exploitation is not published before a mitigation is available.

## Disclosure

Once a fix is available, the project documents affected versions, mitigations,
and the recommended update in a GitHub Security Advisory and in the `Security`
section of [CHANGELOG.md](CHANGELOG.md). A published artifact found unsafe is
withdrawn following the [release rollback runbook](docs/RELEASE_ROLLBACK.md).

## Out of scope

- ROMs, cores, and third-party tools not distributed by AYTHER Engine.
- Vulnerabilities that exist only in an already-patched dependency when the
  project neither pins nor distributes the vulnerable version.
- Issues that require deliberately modifying a local binary after it has
  passed integrity checks.
