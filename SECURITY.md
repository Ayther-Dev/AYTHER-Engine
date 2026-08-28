# Security Policy

## Supported versions

AYTHER Engine has not yet published a stable version. During initial
development, reports will be evaluated against the `main` branch. This section
will be replaced with a version matrix after the first stable release.

No security-support lifetime or production pack trust guarantee is currently
offered. The implemented development model and its known gaps are documented in
[Pack and security model](docs/PACK_SECURITY_MODEL.md).

## Reporting a vulnerability

Do not open a public issue for an unpatched vulnerability.

Use the private **Report a vulnerability** option under GitHub Security
Advisories for this repository. Include the following whenever possible:

- affected version, tag, or commit;
- platform and toolchain;
- description and impact;
- minimal reproduction steps;
- proof of concept without sensitive data;
- known mitigation;
- availability to validate a fix.

If GitHub Security Advisories is not enabled, contact the maintainers privately
before disclosing technical details.

## Priority scope

The following areas are considered especially sensitive:

- signature and integrity validation for `.ay` packs;
- path traversal or writes outside the expected directory when reading files;
- memory corruption at the Rust/C++ boundary;
- incorrect ownership or lifetime handling for FFI handles;
- unauthorized execution through scripts or pack content;
- unsafe loading of cores or dynamic libraries;
- exposure of keys, certificates, or private data;
- denial of service through malformed packs or files.
- archive path ambiguity, decompression bombs, and resource-limit bypasses;
- acceptance of unknown, expired, revoked, or development signing keys in a
  production policy.

## Response process

The maintainers will acknowledge receipt when possible, assess the scope and
severity, prepare a fix, and coordinate disclosure. Response times depend on
the issue's complexity and project availability. Information that could enable
exploitation will not be published before a mitigation is available.

## Disclosure

Once a fix is available, the project will document affected versions,
mitigations, and the recommended update through a GitHub Security Advisory and
the `Security` section of `CHANGELOG.md`.

## Out of scope

- ROMs, cores, and third-party tools not distributed by AYTHER Engine.
- Vulnerabilities that exist only in an already-patched dependency when the
  project neither pins nor distributes the vulnerable version.
- Issues that require deliberately modifying a local binary after it has
  passed integrity checks.
