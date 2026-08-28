# Legal and distribution boundaries

**Status:** project policy; not legal advice

**Last reviewed:** 2026-08-27

This document states the distribution boundaries intended by the project. It
does not replace the license texts or professional advice for a particular
jurisdiction, product, or content catalog.

## Engine license

AYTHER Engine source identified as covered software is available under the
Mozilla Public License 2.0 (`MPL-2.0`). The controlling text is
[LICENSE](../LICENSE). MPL-2.0 obligations apply at file level and include source
availability requirements when covered executable code is distributed. A
larger work may use different terms for separate files, subject to the license.

The license provides no trademark rights. Names, logos, service marks, and
product presentation require separate authorization except where use is needed
to satisfy license notices or applicable law.

## Product separation

AYTHER Lab is a separate proprietary product and is not licensed by this
repository. AYTHER Runtime, SDK, Play, and Hub are separate products or services
with their own artifacts, terms, and release responsibilities. An interface or
integration point does not transfer license rights between products.

## Bring your own ROM and core

AYTHER follows two distribution constraints:

- **BYOR — Bring Your Own ROM:** users supply their own game image and remain
  responsible for lawful acquisition and use.
- **BYOC — Bring Your Own Core:** users supply a compatible emulator core unless
  a distributor has separately established the right to ship one.

Project artifacts must not include ROMs, BIOS images, decryption material,
commercial game assets, or emulator cores without documented authorization.
AYTHER does not endorse infringement, circumvention, or unauthorized
distribution. Availability on the internet is not evidence of permission.

## Packs and authored content

An `.ay` pack is content separate from the engine. Pack authors and distributors
must have rights for every image, audio file, font, script, name, likeness,
translation, and derivative element they include. A valid pack signature
attests to bytes under a selected key policy; it does not grant copyright,
trademark, publicity, patent, or other rights.

Credits and license metadata should be complete and machine-readable, but
metadata does not cure missing permission. Packs must not embed the source ROM,
BIOS, private signing keys, or undistributable emulator binaries.

## Patches

The engine can apply IPS/BPS patches in memory without rewriting the user's
source file. Technical non-destructiveness does not decide whether a patch or
its use is lawful. Distributors must assess whether a patch contains protected
expression, circumvention material, or other restricted content.

## Third-party software

Dependencies and bundled code retain their own copyright and license terms.
Before any binary release, generate a complete transitive inventory for the
exact artifact, preserve required license texts and notices, satisfy source or
offer obligations, and review patent or codec terms. [NOTICE.md](../NOTICE.md)
is a development inventory, not a substitute for artifact-specific review.

Static linking deserves particular attention because the final distribution
combines multiple components. Dependency selection should prefer terms
compatible with the intended distribution and avoid enabling optional features
that silently change obligations. The final result must be reviewed from the
actual lockfiles, vcpkg baseline, build flags, and bundled files.

## Distributor checklist

Before publishing an artifact:

1. identify every included source, binary, asset, font, codec, and data file;
2. record its origin, exact version, license, copyright, and applicable notices;
3. verify rights for names, logos, screenshots, recordings, and pack content;
4. provide MPL-covered source in the manner required by MPL-2.0;
5. exclude ROMs, BIOS files, private keys, and unauthorized cores or assets;
6. publish accurate warranty, support, privacy, export, and telemetry terms;
7. have qualified counsel review unresolved jurisdiction-specific questions.

The software is provided without warranty as stated in MPL-2.0. Early-development
status creates additional technical risk and must remain visible in release and
integration materials.
