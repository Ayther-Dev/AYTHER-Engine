//! Resource and logical-path policy for untrusted `.ay` pack containers.
//!
//! Builders, readers, and diagnostic validators use the same policy so a pack
//! cannot be accepted by one boundary and interpreted differently by another.

use std::collections::HashSet;
use std::fmt;

/// Largest accepted `.ay` container on disk: 4 GiB.
pub const MAX_ARCHIVE_BYTES: u64 = 4 * 1024 * 1024 * 1024;

/// Largest number of central-directory records in one pack.
pub const MAX_ENTRY_COUNT: usize = 8_192;

/// Largest uncompressed size of one entry: 1 GiB.
pub const MAX_ENTRY_BYTES: u64 = 1024 * 1024 * 1024;

/// Largest sum of declared uncompressed entry sizes: 8 GiB.
pub const MAX_TOTAL_UNCOMPRESSED_BYTES: u64 = 8 * 1024 * 1024 * 1024;

/// Largest TOML, JSON, Lua, integrity, or signature entry: 8 MiB.
pub const MAX_METADATA_BYTES: u64 = 8 * 1024 * 1024;

/// Largest accepted uncompressed-to-compressed ratio for a non-empty entry.
pub const MAX_COMPRESSION_RATIO: u64 = 200;

/// Largest UTF-8 byte length of one logical pack path.
pub const MAX_PATH_BYTES: usize = 512;

/// Largest byte length of one logical path segment.
pub const MAX_PATH_SEGMENT_BYTES: usize = 255;

/// Security-policy rejection raised before pack content is trusted or decoded.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PackPolicyViolation {
    message: String,
}

impl PackPolicyViolation {
    pub(crate) fn new(message: impl Into<String>) -> Self {
        Self {
            message: message.into(),
        }
    }
}

impl fmt::Display for PackPolicyViolation {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

impl std::error::Error for PackPolicyViolation {}

/// Archive metadata used by the shared container-limit validator.
pub(crate) struct EntryMetadata<'a> {
    pub(crate) path: &'a str,
    pub(crate) uncompressed_size: u64,
    pub(crate) compressed_size: u64,
}

/// Normalizes a producer-supplied path and rejects unsafe logical names.
///
/// Backslashes become `/`. All remaining bytes must be printable ASCII. This
/// deliberately excludes Unicode-normalization ambiguity while packs are
/// pre-release; human-readable labels belong in metadata, not entry names.
/// Absolute paths, drive/stream separators, empty or relative segments,
/// trailing-dot/space aliases, and Windows device names are rejected.
///
/// # Errors
///
/// Returns [`PackPolicyViolation`] when the path has no single portable
/// interpretation.
pub fn normalize_logical_path(path: &str) -> Result<String, PackPolicyViolation> {
    let normalized = path.replace('\\', "/");
    validate_normalized_path(&normalized)?;
    Ok(normalized)
}

/// Requires an archive path to already be in canonical logical form.
///
/// Readers reject rather than silently normalize because signatures and
/// integrity indexes authenticate the exact entry name bytes.
///
/// # Errors
///
/// Returns [`PackPolicyViolation`] when `path` is unsafe or would normalize to
/// a different name.
pub fn validate_canonical_logical_path(path: &str) -> Result<(), PackPolicyViolation> {
    let normalized = normalize_logical_path(path)?;
    if normalized != path {
        return Err(PackPolicyViolation::new(format!(
            "entry path '{path}' is not canonical; use '{normalized}'"
        )));
    }
    Ok(())
}

fn validate_normalized_path(path: &str) -> Result<(), PackPolicyViolation> {
    if path.is_empty() {
        return Err(PackPolicyViolation::new("entry path is empty"));
    }
    if path.len() > MAX_PATH_BYTES {
        return Err(PackPolicyViolation::new(format!(
            "entry path is {} bytes; maximum is {MAX_PATH_BYTES}",
            path.len()
        )));
    }
    if !path.bytes().all(|b| (0x20..=0x7e).contains(&b)) {
        return Err(PackPolicyViolation::new(format!(
            "entry path '{path}' contains non-printable or non-ASCII bytes"
        )));
    }
    if path.starts_with('/') || path.contains(':') {
        return Err(PackPolicyViolation::new(format!(
            "entry path '{path}' is absolute, drive-qualified, or stream-qualified"
        )));
    }

    for segment in path.split('/') {
        if segment.is_empty() || segment == "." || segment == ".." {
            return Err(PackPolicyViolation::new(format!(
                "entry path '{path}' contains an empty or relative segment"
            )));
        }
        if segment.len() > MAX_PATH_SEGMENT_BYTES {
            return Err(PackPolicyViolation::new(format!(
                "entry path '{path}' has a segment longer than {MAX_PATH_SEGMENT_BYTES} bytes"
            )));
        }
        if segment.ends_with(['.', ' ']) || is_windows_device_name(segment) {
            return Err(PackPolicyViolation::new(format!(
                "entry path '{path}' has a platform-ambiguous segment '{segment}'"
            )));
        }
    }
    Ok(())
}

fn is_windows_device_name(segment: &str) -> bool {
    let stem = segment
        .split('.')
        .next()
        .unwrap_or(segment)
        .trim_end_matches(['.', ' ']);
    let upper = stem.to_ascii_uppercase();
    matches!(upper.as_str(), "CON" | "PRN" | "AUX" | "NUL")
        || upper
            .strip_prefix("COM")
            .or_else(|| upper.strip_prefix("LPT"))
            .is_some_and(|suffix| {
                matches!(suffix, "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9")
            })
}

/// Validates central-directory metadata without decompressing entry bodies.
pub(crate) fn validate_archive_metadata<'a>(
    archive_size: u64,
    declared_entry_count: usize,
    entries: impl IntoIterator<Item = EntryMetadata<'a>>,
) -> Result<(), PackPolicyViolation> {
    validate_archive_size(archive_size)?;
    if declared_entry_count > MAX_ENTRY_COUNT {
        return Err(PackPolicyViolation::new(format!(
            "archive declares {declared_entry_count} entries; maximum is {MAX_ENTRY_COUNT}"
        )));
    }

    let mut seen = HashSet::with_capacity(declared_entry_count.min(MAX_ENTRY_COUNT));
    let mut total = 0u64;
    for entry in entries {
        validate_canonical_logical_path(entry.path)?;
        if !seen.insert(entry.path.to_ascii_lowercase()) {
            return Err(PackPolicyViolation::new(format!(
                "archive contains a duplicate or case-aliased entry path '{}'",
                entry.path
            )));
        }
        if entry.uncompressed_size > MAX_ENTRY_BYTES {
            return Err(PackPolicyViolation::new(format!(
                "entry '{}' is {} bytes; per-entry maximum is {MAX_ENTRY_BYTES}",
                entry.path, entry.uncompressed_size
            )));
        }
        if is_metadata_path(entry.path) && entry.uncompressed_size > MAX_METADATA_BYTES {
            return Err(PackPolicyViolation::new(format!(
                "metadata entry '{}' is {} bytes; maximum is {MAX_METADATA_BYTES}",
                entry.path, entry.uncompressed_size
            )));
        }
        if entry.uncompressed_size != 0
            && (entry.compressed_size == 0
                || entry.uncompressed_size
                    > entry.compressed_size.saturating_mul(MAX_COMPRESSION_RATIO))
        {
            return Err(PackPolicyViolation::new(format!(
                "entry '{}' expands beyond the {MAX_COMPRESSION_RATIO}:1 compression-ratio limit",
                entry.path
            )));
        }
        total = total
            .checked_add(entry.uncompressed_size)
            .ok_or_else(|| PackPolicyViolation::new("total uncompressed size overflow"))?;
        if total > MAX_TOTAL_UNCOMPRESSED_BYTES {
            return Err(PackPolicyViolation::new(format!(
                "archive expands to more than {MAX_TOTAL_UNCOMPRESSED_BYTES} bytes"
            )));
        }
    }
    Ok(())
}

/// Rejects an oversized container before its ZIP central directory is parsed.
pub(crate) fn validate_archive_size(archive_size: u64) -> Result<(), PackPolicyViolation> {
    if archive_size > MAX_ARCHIVE_BYTES {
        return Err(PackPolicyViolation::new(format!(
            "archive is {archive_size} bytes; maximum is {MAX_ARCHIVE_BYTES}"
        )));
    }
    Ok(())
}

fn is_metadata_path(path: &str) -> bool {
    path.ends_with(".toml")
        || path.ends_with(".json")
        || path.ends_with(".lua")
        || path == "signature.bin"
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn normalizes_portable_builder_paths() {
        assert_eq!(
            normalize_logical_path("assets\\sprites\\hero.png").unwrap(),
            "assets/sprites/hero.png"
        );
    }

    #[test]
    fn rejects_traversal_absolute_unicode_and_platform_aliases() {
        for path in [
            "../secret",
            "assets/../../secret",
            "/absolute",
            "C:/drive",
            "assets//hero.png",
            "assets/./hero.png",
            "assets/hero. ",
            "assets/CON.txt",
            "assets/héroe.png",
        ] {
            assert!(normalize_logical_path(path).is_err(), "accepted {path:?}");
        }
    }

    #[test]
    fn rejects_duplicate_and_high_expansion_entries() {
        let duplicate = [
            EntryMetadata {
                path: "manifest.toml",
                uncompressed_size: 1,
                compressed_size: 1,
            },
            EntryMetadata {
                path: "manifest.toml",
                uncompressed_size: 1,
                compressed_size: 1,
            },
        ];
        assert!(validate_archive_metadata(2, 2, duplicate).is_err());

        let case_alias = [
            EntryMetadata {
                path: "assets/Hero.png",
                uncompressed_size: 1,
                compressed_size: 1,
            },
            EntryMetadata {
                path: "assets/hero.png",
                uncompressed_size: 1,
                compressed_size: 1,
            },
        ];
        assert!(validate_archive_metadata(2, 2, case_alias).is_err());

        let bomb = [EntryMetadata {
            path: "assets/zeros.bin",
            uncompressed_size: MAX_COMPRESSION_RATIO + 1,
            compressed_size: 1,
        }];
        assert!(validate_archive_metadata(1, 1, bomb).is_err());
    }
}
