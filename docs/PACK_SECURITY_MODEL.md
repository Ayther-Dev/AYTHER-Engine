# Pack and security model

**Status:** container hardening and development trust implemented; production trust incomplete

**Last reviewed:** 2026-08-28

An `.ay` pack is an untrusted ZIP container that supplies metadata and optional
replacement assets. Passing validation means the bytes satisfy the implemented
rules under a selected trust policy; it does not prove that the content is safe,
lawful, high quality, or compatible with every runtime.

> [!CAUTION]
> Signed packs currently verify against an embedded public development test key.
> The builder uses corresponding development key material. There is no
> production key registry, rotation, revocation, delegation, or compromise
> response. Do not use this mechanism as a production chain of trust.

## Container contract

The archive uses these security-relevant entries:

- `manifest.toml` — required pack metadata and schema declaration;
- `integrity.toml` — digests and, where applicable, chunk metadata;
- `signature.bin` — Ed25519 signature over the raw `integrity.toml` bytes;
- asset, locale, profile, script, and credit data referenced by the manifest.

The highest implemented manifest schema is `2`. Compatibility results are
graded as exact, warning-bearing, experimental, or incompatible; only an
incompatible result blocks activation. A missing compatibility context can
therefore yield an experimental result and must not be presented as verified.

## Verification flow

1. Reject containers whose central directory violates the shared path,
   duplicate-name, entry-count, size, or compression-ratio policy.
2. Open the ZIP without extracting its contents to the filesystem.
3. Parse the manifest and integrity index with bounded reads and format checks.
4. Verify the Ed25519 signature over the exact integrity-index bytes when a
   signature is required or present.
5. Verify entry SHA-256 digests before treating content as trusted.
6. Apply manifest, system, profile, region, and tier compatibility rules.
7. Activate the pack only if the caller's policy accepts the result.

Debug builds may accept unsigned packs with a warning. Release builds reject
unsigned packs. This difference is intentional for authoring, but callers must
surface it clearly and must not label a debug acceptance as release-equivalent.

## Reading strategy

With an integrity index, entries can be opened lazily. Stored entries may use
verified ranged reads backed by chunk hashes. The implementation uses chunks of
at least 64 KiB and caps an entry at 2,048 chunks. Older packs without an
integrity index require resident full-entry hashing.

The shared container policy currently allows at most 4 GiB on disk, 8,192 file
entries, 1 GiB per uncompressed entry, 8 GiB total declared uncompressed data,
8 MiB per metadata/script entry, and a 200:1 expansion ratio. Reads are bounded
again at the point of decompression and must match the central-directory size.
Media decoders still require their own decoded-image, decoded-audio, nesting,
and processing-time limits.

## Path handling

The VFS reads archive entries directly and does not extract them to disk. The
builder, reader, and diagnostic validator share one canonical path policy.
Builder paths normalize backslashes to `/`; readers require that normalized form
already be present because signatures authenticate exact name bytes. The policy
rejects absolute and drive/stream-qualified paths, empty, `.` and `..` segments,
duplicates, control characters, trailing-dot/space aliases, and Windows device
names. Entry paths are restricted to printable ASCII during the pre-release
format phase, eliminating Unicode-normalization ambiguity; localized labels stay
in UTF-8 metadata.

## Lua sandbox

Scripts run with a reduced Lua 5.4 environment. The exposed standard libraries
are string, table, and math; I/O, operating-system, package loading, debug,
coroutine, and UTF-8 libraries are excluded. A hook limits execution to
1,000,000 instructions per frame. Script state is single-threaded.

The instruction budget limits CPU work but does not by itself bound all memory,
allocation, asset-size, or host-callback costs. Host APIs must remain narrow,
validate all indices and lengths, avoid ambient filesystem/network authority,
and define deterministic failure behavior.

## ROM patches and emulator cores

IPS and BPS patches are applied to an in-memory copy; the source ROM is not
rewritten. Patch validity does not establish permission to use or distribute the
underlying game. Emulator cores are native code and sit outside the pack
sandbox: loading a core is equivalent to loading an untrusted dynamic library
and requires a separate allowlist, provenance, and isolation policy.

## Required production controls

- trusted-key registry with scoped identities, expiry, rotation, and revocation;
- protected signing service with no private key in source or developer builds;
- decoded image, audio, and script-memory/time limits beyond container bytes;
- signature-policy tests covering missing, malformed, unknown, expired, and
  revoked keys;
- fuzzing and adversarial fixtures for ZIP, TOML, patches, media, scripts, and
  all FFI entry points;
- audit logging that distinguishes integrity, compatibility, and policy failure.

Security reports follow [SECURITY.md](../SECURITY.md). Content and distribution
rights are defined separately in
[Legal and distribution boundaries](LEGAL_AND_DISTRIBUTION.md).
