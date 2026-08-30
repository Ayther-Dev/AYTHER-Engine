# Pack and security model

**Status:** container hardening and explicit production trust policy implemented

**Last reviewed:** 2026-08-30

An `.ay` pack is an untrusted ZIP container that supplies metadata and optional
replacement assets. Passing validation means the bytes satisfy the implemented
rules under a selected trust policy; it does not prove that the content is safe,
lawful, high quality, or compatible with every runtime.

> [!CAUTION]
> The embedded RFC 8032 key is authoring-only. Debug builds may use it, but
> release builds reject it. Production hosts must call the trusted-open API with
> an explicitly provisioned registry; this repository does not publish or
> operate AYTHER Hub's production keys.

## Container contract

The archive uses these security-relevant entries:

- `manifest.toml` — required pack metadata and schema declaration;
- `integrity.toml` — digests and, where applicable, chunk metadata;
- `signature.bin` — versioned signer identity plus an Ed25519 signature over the
  raw `integrity.toml` bytes;
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
4. Decode the signature envelope, resolve its key identifier in the explicit
   trust registry, enforce validity and revocation, then verify Ed25519 over the
   exact integrity-index bytes.
5. Verify entry SHA-256 digests before treating content as trusted.
6. Authorize the authenticated manifest's `game_id` against the key scope.
7. Apply manifest, system, profile, region, and tier compatibility rules.
8. Activate the pack only if the caller's policy accepts the result.

Debug builds may accept unsigned packs with a warning and signatures from the
development key. Release builds reject both. This difference is intentional for
authoring, but callers must surface it clearly and must not label a debug
acceptance as release-equivalent.

## Production trust registry

The runtime accepts a version-1 TOML registry with no private material:

```toml
version = 1

[[keys]]
id = "hub-2026-01"
algorithm = "ed25519"
public_key = "<64 lowercase hexadecimal characters>"
not_before_unix = 1767225600
not_after_unix = 1830297600
revoked = false
games = ["sonic2", "streets_of_rage"]
```

Key identifiers contain only ASCII letters, digits, `.`, `_`, or `-` and are
unique. Validity bounds are inclusive Unix seconds. `games = ["*"]` is an
explicit all-games delegation; an empty scope is invalid. Unknown fields,
unsupported algorithms, duplicate identifiers, inverted validity intervals,
invalid public keys, and the known development public key make the entire
registry fail closed.

Rust callers load [`TrustStore`](../core/src/pack_trust.rs) and use
`AyArchive::open_with_trust_store`. C and C++ hosts use
`ayther_pack_open_trusted(pack_path, registry_path)`. The ordinary open API is
retained for authoring compatibility, but cannot accept the development key in
an optimized release build.

Rotation is an overlap operation: distribute a registry containing both active
keys, start signing with the new identifier, then retire the old entry after its
last supported pack window. A compromise update sets `revoked = true`; it must
be shipped through the host's protected update channel before affected packs
are accepted again under any replacement key. Reusing an identifier with new
key bytes is forbidden operationally even after expiry.

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

## Remaining operational controls

- protected signing service with no private key in source or developer builds;
- decoded image, audio, and script-memory/time limits beyond container bytes;
- fuzzing and adversarial fixtures for ZIP, TOML, patches, media, scripts, and
  all FFI entry points;
- audit logging that distinguishes integrity, compatibility, and policy failure.

Security reports follow [SECURITY.md](../SECURITY.md). Content and distribution
rights are defined separately in
[Legal and distribution boundaries](LEGAL_AND_DISTRIBUTION.md).
