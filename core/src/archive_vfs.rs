//! Verified virtual filesystem for `.ay` content packs.
//!
//! [`AyArchive`] supports regional and resolution-tier lookups, lazy entry
//! loading, integrity verification, pack metadata, and remastering profiles.

// ---------------------------------------------------------------------------
// AyArchive — virtual filesystem over `.ay` pack files.
//
// ## Format
//
// An `.ay` file is a ZIP archive (Deflate or Store compression) containing:
//
//   manifest.toml    — pack metadata (required)
//   integrity.toml   — per-entry SHA-256 + size for every entry; its
//                      presence selects the LAZY open path (see below)
//   signature.bin    — ED25519 signature (optional; absent in unsigned dev
//                      packs, mandatory in Hub-distributed packs)
//   graphics/        — HD asset tree (PNG / WebP tiles keyed by tile hash)
//   audio/           — OGG Vorbis replacement audio tracks
//   locales/{ID}/    — regional overrides (same tree, transparently applied)
//
// ## Residency
//
// Packs WITH `integrity.toml` open lazily: only the ZIP central directory,
// the manifest, integrity.toml and the signature are read up front. Assets
// are decompressed on demand in `read()` and verified against their
// per-entry hash right there — RAM and verification cost are proportional
// to what is actually used, not to the pack size. La regla del pack («viaja
// lo que se usa») ahora también gobierna lo que se PAGA.
//
// Packs WITHOUT `integrity.toml` are legacy: fully resident, whole-content
// hash verified at open, exactly as before. They keep loading forever.
//
// ## Lectura por RANGO
//
// `read()` verifica la entrada ENTERA, y por eso una entrada grande —un video
// de Cinemática— se materializa entera en RAM aunque sólo se quiera un frame.
// Ése, y no el formato, era el motivo del tope de 32 MB por video del bake.
//
// `read_range()` lo resuelve cambiando la UNIDAD DE VERIFICACIÓN: para las
// entradas `Stored` (hoy, los `.ivf`) el índice firmado trae además un hash
// por TROZO de tamaño fijo, y una lectura por rango verifica sólo los trozos
// que toca. La cadena de confianza no se afloja — signature.bin sigue firmando
// integrity.toml, que ahora también enumera los trozos— y nada se entrega sin
// verificar. Lo que cambia es el COSTO: proporcional a lo leído.
//
// El offset absoluto de los datos sale del directorio central del ZIP, que NO
// está firmado; no hace falta que lo esté, porque sólo dice DÓNDE mirar. Si
// apuntara a otro lado, el hash del trozo no daría y la lectura falla.
//
// Deflate no es direccionable por rango: una entrada deflateada no lleva
// trozos y `read_range` devuelve `None`, para que el llamador caiga a `read()`
// en vez de recibir datos sin verificar.
//
// ## What is signed
//
//   Lazy packs   — signature.bin = ED25519 over the raw bytes of
//                  integrity.toml, which enumerates and hashes every other
//                  entry (manifest included). Removing/altering any entry
//                  breaks either the open-time cross-check or the per-asset
//                  hash; stripping integrity.toml demotes the pack to the
//                  legacy path, where the signature can no longer match.
//   Legacy packs — signature.bin = ED25519 over the whole-content hash:
//                  SHA-256 of, for path in sorted(all paths excluding
//                  "signature.bin"):
//                    path_len : u32le  ||  path : UTF-8 bytes
//                    data_len : u64le  ||  data : bytes
//
// ## Key trust model
//
//   DEV_PUBLIC_KEY  — embedded here; ay_pack --create-dev uses the matching seed.
//   Hub packs use a production key; the engine trusts a key registry (v0.5+).
// ---------------------------------------------------------------------------

use ed25519_dalek::{Signature, Verifier, VerifyingKey};
use serde::Deserialize;
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::io::{Read, Seek, SeekFrom};
use std::sync::Mutex;

use crate::pack_security::{
    EntryMetadata, MAX_ENTRY_BYTES, MAX_ENTRY_COUNT, MAX_METADATA_BYTES, PackPolicyViolation,
    validate_archive_metadata, validate_archive_size, validate_canonical_logical_path,
};

// ---------------------------------------------------------------------------
// Dev test key (RFC 8032 test vector — clearly not a production key)
// ---------------------------------------------------------------------------
/// ED25519 verifying key corresponding to the DEV_SIGNING_SEED in ay_pack.
/// THIS IS A PUBLIC TEST KEY — it is safe to embed in open-source code.
/// Derived with: `ay_pack show-dev-keys`
const DEV_PUBLIC_KEY: [u8; 32] = [
    0xad, 0x25, 0xd7, 0x0a, 0x95, 0xc2, 0xc0, 0x8d, 0x12, 0x0f, 0x43, 0x71, 0x28, 0x12, 0x53, 0xe9,
    0xfb, 0xe6, 0x07, 0x90, 0x67, 0x22, 0x30, 0xcb, 0xc2, 0x7a, 0x68, 0x7a, 0x27, 0x89, 0x42, 0x3b,
];

/// Entry names with structural meaning — never served as assets.
pub const INTEGRITY_PATH: &str = "integrity.toml";
/// Detached signature entry that authenticates [`INTEGRITY_PATH`].
pub const SIGNATURE_PATH: &str = "signature.bin";

// ---------------------------------------------------------------------------
// Streaming por rango — política de trozos
//
// Estas tres funciones las comparten los DOS escritores de integrity.toml
// (core/src/pack_builder.rs y tools/ay_pack). ay_pack no puede depender de
// ayther_core (staticlib, y arrastraría mlua/notify/rustysynth por una
// función de cuentas), así que están duplicadas allá — con un test de deriva
// a cada lado que fija los mismos valores.
// ---------------------------------------------------------------------------

/// Minimum integrity-verification chunk size: 64 KiB.
pub const STREAM_CHUNK_MIN: u64 = 64 * 1024;

/// Maximum number of integrity chunks recorded for one entry.
pub const STREAM_CHUNK_MAX_COUNT: u64 = 2048;

/// Selects a power-of-two chunk size that keeps an entry within the chunk cap.
pub fn stream_chunk_size(size: u64) -> u64 {
    let mut c = STREAM_CHUNK_MIN;
    while size.div_ceil(c) > STREAM_CHUNK_MAX_COUNT {
        c *= 2;
    }
    c
}

/// Returns whether an entry should remain uncompressed for ranged access.
///
/// Currently this applies to pre-compressed IVF video.
pub fn is_stored_entry(path: &str) -> bool {
    path.ends_with(".ivf")
}

// ---------------------------------------------------------------------------
// Manifest types
// ---------------------------------------------------------------------------

/// Newest manifest schema written and understood by this build.
///
/// Unknown fields remain forward-compatible, while a larger declared schema is
/// rejected because it may require semantics this build does not implement.
pub const MANIFEST_SCHEMA: u32 = 2;

/// Pack-level metadata from `manifest.toml → [pack]`.
#[derive(Debug, Clone, Deserialize)]
pub struct PackMeta {
    /// Display name of the pack.
    pub name: String,
    /// Pack release version.
    pub version: String,
    /// Stable game identifier, such as `sonic2`.
    pub game_id: String, // e.g. "sonic2", "streets_of_rage"
    /// Primary author, when declared.
    pub author: Option<String>,
    /// Minimum compatible AYTHER Engine version.
    pub ayther_min: Option<String>, // minimum engine version required
    /// Optional human-readable pack description.
    pub description: Option<String>,
    /// Manifest schema version. Absence denotes legacy schema version 1.
    #[serde(default)]
    pub schema: Option<u32>,
    /// Additional contributors to the pack.
    #[serde(default)]
    pub contributors: Vec<String>,
    /// Declared content license, preferably an SPDX identifier.
    #[serde(default)]
    pub license: Option<String>,
    /// Recommended output profile, such as `crt`, `lcd`, or `pixel`.
    ///
    /// The runtime treats this as a recommendation; an explicit user choice
    /// takes precedence.
    #[serde(default)]
    pub output: Option<String>,
}

/// Regional configuration from `manifest.toml → [regions]`.
#[derive(Debug, Clone, Deserialize)]
pub struct RegionConfig {
    /// Region selected when the user has not chosen one.
    pub default: String,
    /// Region identifiers supported by the pack.
    pub supported: Vec<String>,
}

/// Resolution tiers included by `manifest.toml → [tiers]`.
///
/// Indices `0..=3` represent HD 3×, Full HD 4.5×, 4K 9×, and 8K 18×.
#[derive(Debug, Clone, Deserialize)]
pub struct TierConfig {
    /// Included tier indices.
    pub included: Vec<u32>,
}

/// Subsystems included by `manifest.toml → [systems]`.
///
/// Builders derive this table from actual contents. Unknown names are preserved
/// for forward compatibility.
#[derive(Debug, Clone, Deserialize, Default)]
pub struct SystemsConfig {
    /// Canonical subsystem names included by the pack.
    #[serde(default)]
    pub included: Vec<String>,
}

/// Canonical ordered list of subsystems that AYTHER can replace.
///
/// Indices form part of the C++ bit-mask ABI.
pub const SUBSYSTEMS: [&str; 8] = [
    "sprites",     // 0
    "metasprites", // 1
    "tiles",       // 2
    "planes",      // 3
    "ui",          // 4
    "music",       // 5
    "sfx",         // 6
    "shaders",     // 7
];

/// Remastering profile from `manifest.toml → [[profile]]`.
///
/// A profile filters the pack's shared assets by subsystem and muted audio bus;
/// it does not duplicate content.
#[derive(Debug, Clone, Deserialize, Default)]
pub struct ProfileDef {
    /// Stable identifier used by APIs and persisted configuration.
    pub id: String,
    /// Display name; falls back to [`Self::id`] when absent.
    #[serde(default)]
    pub name: Option<String>,
    /// Optional human-readable profile description.
    #[serde(default)]
    pub description: Option<String>,
    /// Canonical subsystem names enabled by this profile.
    #[serde(default)]
    pub systems: Vec<String>,
    /// Audio buses muted by this profile: `music`, `sfx`, or `voice`.
    #[serde(default)]
    pub muted_buses: Vec<String>,
    /// Whether this profile is selected when the pack opens.
    #[serde(default)]
    pub default: bool,
}

impl ProfileDef {
    /// Converts known subsystem names to the canonical C++ bit mask.
    pub fn systems_mask(&self) -> u32 {
        let mut m = 0u32;
        for s in &self.systems {
            if let Some(i) = SUBSYSTEMS.iter().position(|x| *x == s.as_str()) {
                m |= 1 << i;
            }
        }
        m
    }

    /// Returns the display name or falls back to the stable identifier.
    pub fn display_name(&self) -> &str {
        self.name
            .as_deref()
            .filter(|s| !s.is_empty())
            .unwrap_or(&self.id)
    }
}

/// Optional runtime requirements from `manifest.toml → [compat]`.
#[derive(Debug, Clone, Deserialize, Default)]
pub struct CompatConfig {
    /// Required ROM CRC-32 as hexadecimal text without a prefix.
    #[serde(default)]
    pub rom_crc32: Option<String>,
    /// Required game platform, such as `megadrive` or `segacd`.
    #[serde(default)]
    pub platform: Option<String>,
    /// Minimum emulator-core build identifier.
    #[serde(default)]
    pub core_min: Option<String>,
}

#[derive(Deserialize)]
struct RawManifest {
    pack: PackMeta,
    regions: Option<RegionConfig>,
    tiers: Option<TierConfig>,
    #[serde(default)]
    systems: Option<SystemsConfig>,
    #[serde(default)]
    compat: Option<CompatConfig>,
    ///. `Vec` y no `Option<Vec>`: un pack sin perfiles no es un caso
    /// especial — significa que sólo tiene el implícito, y eso ya es un
    /// contrato completo (ver `AyArchive::profiles`).
    #[serde(default, rename = "profile")]
    profiles: Vec<ProfileDef>,
}

// ---------------------------------------------------------------------------
// integrity.toml types
// ---------------------------------------------------------------------------

#[derive(Deserialize)]
struct RawIntegrityEntry {
    path: String,
    sha256: String,
    size: u64,
    ///  tamaño de trozo y hash por trozo. Ausentes en los packs
    /// horneados antes de y en toda entrada deflateada — su ausencia es
    /// exactamente «esta entrada no se puede leer por rango».
    chunk: Option<u64>,
    chunks: Option<Vec<String>>,
}

#[derive(Deserialize)]
struct RawIntegrity {
    #[allow(dead_code)]
    version: Option<u32>,
    #[serde(default)]
    entry: Vec<RawIntegrityEntry>,
}

// ---------------------------------------------------------------------------
// Error type
// ---------------------------------------------------------------------------

/// Error returned while opening or validating an `.ay` archive.
#[derive(Debug)]
pub enum AyError {
    /// Underlying filesystem I/O error.
    Io(std::io::Error),
    /// Malformed or unsupported ZIP container.
    Zip(zip::result::ZipError),
    /// The archive does not contain `manifest.toml`.
    MissingManifest,
    /// The manifest could not be decoded or validated.
    MalformedManifest(String),
    /// The detached signature is missing or invalid under the active policy.
    BadSignature,
    /// The container violates path or resource-consumption policy.
    PolicyViolation(PackPolicyViolation),
    /// The manifest requires a schema newer than this build understands.
    UnsupportedSchema {
        /// Schema declared by the pack.
        found: u32,
        /// Newest schema supported by this build.
        max: u32,
    },
}

impl From<std::io::Error> for AyError {
    fn from(e: std::io::Error) -> Self {
        AyError::Io(e)
    }
}
impl From<zip::result::ZipError> for AyError {
    fn from(e: zip::result::ZipError) -> Self {
        AyError::Zip(e)
    }
}
impl From<PackPolicyViolation> for AyError {
    fn from(e: PackPolicyViolation) -> Self {
        AyError::PolicyViolation(e)
    }
}

// ---------------------------------------------------------------------------
// Backend — resident (legacy) vs lazy
// ---------------------------------------------------------------------------

/// Per-entry record from integrity.toml, keyed by path in the lazy index.
struct LazyEntry {
    size: u64, // uncompressed size (cross-checked vs central directory)
    sha256: [u8; 32],
    ///  presente sólo si la entrada es leíble por rango — `Stored` en el
    /// ZIP **y** con trozos en el índice firmado. Las dos condiciones, no una:
    /// los trozos sin `Stored` no sirven (no hay offset) y `Stored` sin trozos
    /// no se puede verificar.
    stream: Option<StreamInfo>,
}

///  lo que hace falta para leer y verificar un pedazo de una entrada.
struct StreamInfo {
    /// Offset ABSOLUTO de los datos dentro del `.ay` (del directorio central).
    data_start: u64,
    chunk: u64,
    hashes: Vec<[u8; 32]>,
}

/// Una entrada tal como la ve el directorio central del ZIP. Se arma en
/// `open_verbose` porque recorrer el directorio dos veces (una para nombres,
/// otra para offsets) sería pagar el mismo paseo dos veces.
struct EntryLoc {
    name: String,
    size: u64,
    compressed_size: u64,
    data_start: u64,
    stored: bool,
}

enum Backend {
    /// Legacy pack (no integrity.toml): every entry resident in RAM.
    Resident(HashMap<String, Vec<u8>>),
    ///  central-directory index only; entries are decompressed and
    /// hash-verified on each `read`. The ZIP (and its file handle) lives for
    /// the archive's lifetime behind a Mutex — reads may come from the render
    /// thread and the async audio-decode worker concurrently.
    ///
    /// NOTE hotreload: if the pack file is rewritten on disk while this
    /// handle is open, in-flight reads can see torn data — the per-entry hash
    /// turns that into a clean `None` (asset degrades for one frame) until
    /// the PackWatcher reopens the archive.
    Lazy {
        zip: Mutex<zip::ZipArchive<std::fs::File>>,
        index: HashMap<String, LazyEntry>,
        ///  SEGUNDO handle al mismo `.ay`, sólo para las lecturas por
        /// rango. Es un handle aparte y no el del ZipArchive porque ése está
        /// tomado por el propio zip (y detrás de su Mutex): una lectura por
        /// rango no tiene por qué esperar a que termine una descompresión.
        raw: Mutex<std::fs::File>,
    },
}

// ---------------------------------------------------------------------------
// AyArchive
// ---------------------------------------------------------------------------

/// Open `.ay` archive with validated metadata and logical asset lookup.
pub struct AyArchive {
    backend: Backend,
    /// Parsed pack metadata.
    pub meta: PackMeta,
    /// Optional regional configuration.
    pub regions: Option<RegionConfig>,
    region: Option<String>,
    /// Included resolution-tier bit mask and currently selected tier.
    tiers_mask: u8,
    resolved_tier: Option<u8>,
    /// Cached null-terminated copy of `meta.game_id`.
    /// Owned by this struct — the raw pointer returned by `ayther_pack_game_id`
    /// is valid for exactly the lifetime of the `AyArchive` that owns it.
    pub game_id_cstr: std::ffi::CString,
    /// Build identifier derived from the signed `integrity.toml` bytes.
    ///
    /// It is empty for legacy packs without an integrity index.
    pub build_id: String,
    /// Cached null-terminated copy of [`Self::build_id`] for the C ABI.
    pub build_id_cstr: std::ffi::CString,
    /// Declared manifest schema; legacy packs use version 1.
    pub schema: u32,
    /// Subsystems declared by the pack builder.
    pub systems: SystemsConfig,
    /// Optional compatibility requirements.
    pub compat: CompatConfig,
    /// Whether the pack explicitly declared a `[systems]` table.
    pub systems_declared: bool,
    /// Sanitized authored profiles, excluding the implicit original profile.
    pub profiles: Vec<ProfileDef>,
    /// Storage for the most recently exported metadata string in the C ABI.
    pub meta_field_cstr: std::ffi::CString,
}

/// Identifier of the implicit unmodified-game profile.
///
/// Packs cannot redefine or remove this profile.
pub const ORIGINAL_PROFILE: &str = "original";

/// Sanea la lista declarada. Se hace al ABRIR y no al consultar: un perfil roto
/// que sobrevive hasta el punto de uso obliga a cada consumidor a repetir la
/// misma comprobación, y el que se la olvide aplica una máscara vacía y muestra
/// el juego sin remasterizar sin que nada avise.
///
///  · id vacío → se descarta: no se puede pedir por API.
///  · id repetido → gana el PRIMERO. Con dos «enhanced», quedarse con el último
///    haría que agregar un perfil al final cambiara en silencio lo que hace uno
///    que ya existía.
///  · «original» → se descarta: es implícito.
///  · exactamente UN default. Ninguno dejaría al runtime eligiendo; dos harían
///    que el pack se viera distinto según cómo se recorra la lista.
fn sanitize_profiles(raw: Vec<ProfileDef>) -> Vec<ProfileDef> {
    let mut out: Vec<ProfileDef> = Vec::new();
    for p in raw {
        if p.id.trim().is_empty() || p.id == ORIGINAL_PROFILE {
            continue;
        }
        if out.iter().any(|q| q.id == p.id) {
            continue;
        }
        out.push(p);
    }
    let mut seen_default = false;
    for p in out.iter_mut() {
        if p.default && seen_default {
            p.default = false;
        } else if p.default {
            seen_default = true;
        }
    }
    if !seen_default && let Some(first) = out.first_mut() {
        first.default = true;
    }
    out
}

/// Derives a 12-hex-character build identifier from `integrity.toml`.
pub fn build_id_from_integrity(integrity_bytes: &[u8]) -> String {
    let d = Sha256::digest(integrity_bytes);
    format!("{:x}", d)[..12].to_string()
}

impl AyArchive {
    // -----------------------------------------------------------------------
    // open
    // -----------------------------------------------------------------------

    /// Open and optionally verify a `.ay` pack file.
    ///
    /// Returns `None` on any error (missing manifest, bad signature, etc.).
    /// Call `open_verbose` for detailed error information.
    pub fn open(path: &str) -> Option<Self> {
        Self::open_verbose(path)
            .map_err(|e| eprintln!("[AyArchive] Failed to open '{}': {:?}", path, e))
            .ok()
    }

    /// Opens a pack and preserves the detailed failure reason.
    ///
    /// # Errors
    ///
    /// Returns [`AyError`] when the file, ZIP container, manifest, integrity
    /// index, schema, or signature cannot be accepted.
    pub fn open_verbose(path: &str) -> Result<Self, AyError> {
        let file = std::fs::File::open(path)?;
        let archive_size = file.metadata()?.len();
        validate_archive_size(archive_size)?;
        let mut zip = zip::ZipArchive::new(file)?;

        // ---- Central directory: names + sizes, nothing decompressed --------
        //  en el mismo paseo salen el offset absoluto de los datos y el
        // método de compresión, que es lo que decide si la entrada se puede
        // leer por rango.
        let mut names: Vec<EntryLoc> = Vec::with_capacity(zip.len());
        for i in 0..zip.len() {
            let entry = zip.by_index_raw(i)?;
            if entry.is_dir() {
                continue;
            }
            names.push(EntryLoc {
                name: entry.name().to_string(),
                size: entry.size(),
                compressed_size: entry.compressed_size(),
                data_start: entry.data_start(),
                stored: entry.compression() == zip::CompressionMethod::Stored,
            });
        }
        validate_archive_metadata(
            archive_size,
            zip.len(),
            names.iter().map(|entry| EntryMetadata {
                path: &entry.name,
                uncompressed_size: entry.size,
                compressed_size: entry.compressed_size,
            }),
        )?;

        if names.iter().any(|e| e.name == INTEGRITY_PATH) {
            Self::open_lazy(zip, &names, path)
        } else {
            Self::open_resident(zip, path)
        }
    }

    /// Legacy path: load everything, verify the whole-content hash. Unchanged
    /// behaviour for every pack baked before.
    fn open_resident(mut zip: zip::ZipArchive<std::fs::File>, path: &str) -> Result<Self, AyError> {
        let mut files: HashMap<String, Vec<u8>> = HashMap::new();
        for i in 0..zip.len() {
            let mut entry = zip.by_index(i)?;
            if entry.is_dir() {
                continue;
            }
            let name = entry.name().to_string();
            let declared_size = entry.size();
            let capacity = usize::try_from(declared_size).map_err(|_| {
                PackPolicyViolation::new(format!(
                    "entry '{name}' cannot fit in this platform's address space"
                ))
            })?;
            let mut data = Vec::with_capacity(capacity);
            entry
                .by_ref()
                .take(MAX_ENTRY_BYTES + 1)
                .read_to_end(&mut data)?;
            if data.len() as u64 != declared_size {
                return Err(PackPolicyViolation::new(format!(
                    "entry '{name}' decompressed to a size different from its central-directory declaration"
                ))
                .into());
            }
            files.insert(name, data);
        }

        let manifest_bytes = files.get("manifest.toml").ok_or(AyError::MissingManifest)?;

        // ---- Verify signature (whole-content hash) -------------------------
        let signed = match files.get(SIGNATURE_PATH) {
            None => false,
            Some(sig) => {
                let hash = Self::compute_content_hash(&files);
                Self::verify_signature(&hash, sig)?;
                true
            }
        };
        Self::enforce_signature_policy(signed, path)?;

        let manifest_bytes = manifest_bytes.clone();
        // Pack LEGACY: sin integrity.toml no hay conjunto de hashes del que
        // derivar el build id, y decir uno inventado sería peor que no decirlo.
        Self::finish_open(&manifest_bytes, Backend::Resident(files), String::new())
    }

    ///  index-only open. Reads exactly three entries (integrity.toml,
    /// signature.bin, manifest.toml); every other asset stays on disk until
    /// someone asks for it.
    fn open_lazy(
        mut zip: zip::ZipArchive<std::fs::File>,
        names: &[EntryLoc],
        path: &str,
    ) -> Result<Self, AyError> {
        fn read_entry(
            zip: &mut zip::ZipArchive<std::fs::File>,
            name: &str,
        ) -> Result<Vec<u8>, AyError> {
            let mut e = zip.by_name(name)?;
            validate_canonical_logical_path(name)?;
            if e.size() > MAX_METADATA_BYTES {
                return Err(PackPolicyViolation::new(format!(
                    "metadata entry '{name}' exceeds {MAX_METADATA_BYTES} bytes"
                ))
                .into());
            }
            let declared_size = e.size();
            let capacity = usize::try_from(declared_size).map_err(|_| {
                PackPolicyViolation::new(format!(
                    "metadata entry '{name}' cannot fit in this platform's address space"
                ))
            })?;
            let mut v = Vec::with_capacity(capacity);
            e.by_ref()
                .take(MAX_METADATA_BYTES + 1)
                .read_to_end(&mut v)?;
            if v.len() as u64 != declared_size {
                return Err(PackPolicyViolation::new(format!(
                    "metadata entry '{name}' decompressed to a size different from its central-directory declaration"
                ))
                .into());
            }
            Ok(v)
        }

        let integrity_bytes = read_entry(&mut zip, INTEGRITY_PATH)?;

        // ---- Signature covers the integrity.toml BYTES ----------------------
        let signed = if names.iter().any(|e| e.name == SIGNATURE_PATH) {
            let sig = read_entry(&mut zip, SIGNATURE_PATH)?;
            Self::verify_signature(&integrity_bytes, &sig)?;
            true
        } else {
            false
        };
        Self::enforce_signature_policy(signed, path)?;

        // ---- Parse the index -------------------------------------------------
        let integrity_str = std::str::from_utf8(&integrity_bytes)
            .map_err(|e| AyError::MalformedManifest(format!("integrity.toml: {}", e)))?;
        let raw: RawIntegrity = toml::from_str(integrity_str)
            .map_err(|e| AyError::MalformedManifest(format!("integrity.toml: {}", e)))?;
        if raw.entry.len() > MAX_ENTRY_COUNT {
            return Err(PackPolicyViolation::new(format!(
                "integrity.toml declares {} entries; maximum is {MAX_ENTRY_COUNT}",
                raw.entry.len()
            ))
            .into());
        }

        let mut index: HashMap<String, LazyEntry> = HashMap::with_capacity(raw.entry.len());
        //  hashes por trozo a la espera del offset, que sale del
        // directorio central en el cruce de más abajo.
        let mut pending: HashMap<String, (u64, Vec<[u8; 32]>)> = HashMap::new();
        for e in raw.entry {
            validate_canonical_logical_path(&e.path)?;
            if e.path == INTEGRITY_PATH || e.path == SIGNATURE_PATH {
                return Err(PackPolicyViolation::new(format!(
                    "integrity.toml cannot index reserved entry '{}'",
                    e.path
                ))
                .into());
            }
            if index.contains_key(&e.path) {
                return Err(PackPolicyViolation::new(format!(
                    "integrity.toml contains duplicate path '{}'",
                    e.path
                ))
                .into());
            }
            let sha256 = hex32(&e.sha256).ok_or_else(|| {
                AyError::MalformedManifest(format!(
                    "integrity.toml: sha256 inválido para '{}'",
                    e.path
                ))
            })?;
            match (e.chunk, e.chunks.as_ref()) {
                (Some(chunk), Some(list)) => {
                    let bad = |m: &str| {
                        AyError::MalformedManifest(format!("integrity.toml: '{}' {}", e.path, m))
                    };
                    if chunk < STREAM_CHUNK_MIN || !chunk.is_power_of_two() {
                        return Err(bad("declara un tamaño de trozo inválido"));
                    }
                    if list.len() as u64 > STREAM_CHUNK_MAX_COUNT {
                        return Err(bad("supera el máximo de trozos por entrada"));
                    }
                    if list.len() as u64 != e.size.div_ceil(chunk) {
                        return Err(bad("declara una cantidad de trozos que no \
                                    corresponde a su tamaño"));
                    }
                    let mut hashes = Vec::with_capacity(list.len());
                    for h in list {
                        hashes
                            .push(hex32(h).ok_or_else(|| bad("tiene un hash de trozo inválido"))?);
                    }
                    pending.insert(e.path.clone(), (chunk, hashes));
                }
                (None, None) => {}
                _ => {
                    return Err(AyError::MalformedManifest(format!(
                        "integrity.toml: '{}' debe declarar chunk y chunks juntos",
                        e.path
                    )));
                }
            }
            index.insert(
                e.path,
                LazyEntry {
                    size: e.size,
                    sha256,
                    stream: None,
                },
            );
        }

        // ---- Cross-check vs central directory --------------------------------
        // The signed index and the ZIP must describe the SAME entry set, with
        // the same uncompressed sizes. An entry added, removed or grown after
        // signing is rejected here — before a single asset is read.
        let mut listed = 0usize;
        for loc in names {
            if loc.name == INTEGRITY_PATH || loc.name == SIGNATURE_PATH {
                continue;
            }
            match index.get_mut(&loc.name) {
                Some(le) if le.size == loc.size => {
                    listed += 1;
                    //  la entrada se lee por rango sólo si el ZIP la tiene
                    // Stored Y el índice firmado trae sus trozos. Un pack viejo
                    // (sin trozos) o una entrada deflateada quedan sin `stream`
                    // y caen a `read()`, que es el camino de siempre.
                    if loc.stored
                        && let Some((chunk, hashes)) = pending.remove(&loc.name)
                    {
                        le.stream = Some(StreamInfo {
                            data_start: loc.data_start,
                            chunk,
                            hashes,
                        });
                    }
                }
                _ => return Err(AyError::BadSignature),
            }
        }
        if listed != index.len() {
            return Err(AyError::BadSignature); // signed entry missing from the ZIP
        }

        // ---- Manifest: read and verify NOW (small, parsed right here) --------
        if !index.contains_key("manifest.toml") {
            return Err(AyError::MissingManifest);
        }
        let manifest_bytes =
            read_entry(&mut zip, "manifest.toml").map_err(|_| AyError::MissingManifest)?;
        if Sha256::digest(&manifest_bytes)[..] != index["manifest.toml"].sha256 {
            return Err(AyError::BadSignature);
        }

        //  handle propio para las lecturas por rango (ver Backend::Lazy).
        let raw_handle = std::fs::File::open(path)?;
        Self::finish_open(
            &manifest_bytes,
            Backend::Lazy {
                zip: Mutex::new(zip),
                index,
                raw: Mutex::new(raw_handle),
            },
            build_id_from_integrity(&integrity_bytes),
        )
    }

    /// Shared tail of both open paths: parse the manifest, resolve tiers,
    /// build the cached game_id C string.
    ///
    /// `build_id` viene del llamador porque sólo el camino lazy lo puede
    /// derivar: sale de `integrity.toml`, que un pack legacy no tiene.
    fn finish_open(
        manifest_bytes: &[u8],
        backend: Backend,
        build_id: String,
    ) -> Result<Self, AyError> {
        let manifest_str = std::str::from_utf8(manifest_bytes)
            .map_err(|e| AyError::MalformedManifest(e.to_string()))?;
        let raw: RawManifest =
            toml::from_str(manifest_str).map_err(|e| AyError::MalformedManifest(e.to_string()))?;

        //  la puerta de la versión de esquema. Un pack más nuevo de lo que
        // este build sabe leer se RECHAZA con el número a la vista — abrirlo
        // igual sería servirlo a medias y llamarlo éxito, que es la clase de
        // fallo silencioso que este repo ya pagó caro. Ausente = 1 (todo lo
        // horneado antes de que esto existiera).
        let schema = raw.pack.schema.unwrap_or(1);
        if schema > MANIFEST_SCHEMA {
            return Err(AyError::UnsupportedSchema {
                found: schema,
                max: MANIFEST_SCHEMA,
            });
        }

        // Build the cached C string before moving `meta` into the struct.
        // `unwrap_or_default` gives an empty CString if game_id contains an
        // interior NUL — safe but would surface as an empty string on the C side.
        let meta = raw.pack;
        let game_id_cstr = std::ffi::CString::new(meta.game_id.as_str()).unwrap_or_default();

        //  máscara de tiers del manifest; el tier activo arranca en el MÁS
        // ALTO incluido (default seguro: downscalear siempre es válido, upscalear
        // no) — el host lo ajusta al display con set_tier().
        let mut tiers_mask: u8 = 0;
        if let Some(t) = &raw.tiers {
            for &i in &t.included {
                if i < 8 {
                    tiers_mask |= 1 << i;
                }
            }
        }
        let resolved_tier = (tiers_mask != 0).then(|| 7 - tiers_mask.leading_zeros() as u8);

        let build_id_cstr = std::ffi::CString::new(build_id.as_str()).unwrap_or_default();
        let systems_declared = raw.systems.is_some();
        Ok(AyArchive {
            backend,
            meta,
            regions: raw.regions,
            region: None,
            tiers_mask,
            resolved_tier,
            game_id_cstr,
            build_id,
            build_id_cstr,
            schema,
            systems: raw.systems.unwrap_or_default(),
            compat: raw.compat.unwrap_or_default(),
            systems_declared,
            profiles: sanitize_profiles(raw.profiles),
            meta_field_cstr: std::ffi::CString::default(),
        })
    }

    /// Unsigned packs: allowed (with a warning) in dev builds, rejected in
    /// release. Same policy as always — shared by both open paths.
    fn enforce_signature_policy(signed: bool, path: &str) -> Result<(), AyError> {
        if !signed {
            #[cfg(not(debug_assertions))]
            {
                let _ = path;
                return Err(AyError::BadSignature);
            } // Release: require signature
            #[cfg(debug_assertions)]
            eprintln!(
                "[AyArchive] WARNING: '{}' has no signature (dev build only)",
                path
            );
        }
        Ok(())
    }

    // -----------------------------------------------------------------------
    // signature helpers
    // -----------------------------------------------------------------------

    /// Verify `sig_bytes` (64-byte ED25519) over `msg` with the dev key.
    fn verify_signature(msg: &[u8], sig_bytes: &[u8]) -> Result<(), AyError> {
        if sig_bytes.len() != 64 {
            return Err(AyError::BadSignature);
        }
        let vk = VerifyingKey::from_bytes(&DEV_PUBLIC_KEY).map_err(|_| AyError::BadSignature)?;
        let sig_arr: [u8; 64] = sig_bytes.try_into().map_err(|_| AyError::BadSignature)?;
        vk.verify(msg, &Signature::from_bytes(&sig_arr))
            .map_err(|_| AyError::BadSignature)
    }

    /// Legacy content hash: SHA-256 over all entries except `signature.bin`,
    /// sorted by path. Each entry is: path_len_u32le || path_utf8 || data_len_u64le || data.
    pub fn compute_content_hash(files: &HashMap<String, Vec<u8>>) -> Vec<u8> {
        let mut hasher = Sha256::new();
        let mut sorted: Vec<&String> = files
            .keys()
            .filter(|p| p.as_str() != SIGNATURE_PATH)
            .collect();
        sorted.sort();

        for path in sorted {
            let data = &files[path];
            hasher.update((path.len() as u32).to_le_bytes());
            hasher.update(path.as_bytes());
            hasher.update((data.len() as u64).to_le_bytes());
            hasher.update(data.as_slice());
        }
        hasher.finalize().to_vec()
    }

    // -----------------------------------------------------------------------
    // public API
    // -----------------------------------------------------------------------

    /// Set the active region for transparent asset overrides.
    /// E.g. `set_region("JP")` → `read("graphics/title.png")` will first
    /// try `locales/JP/graphics/title.png`.
    pub fn set_region(&mut self, region: &str) {
        self.region = Some(region.to_string());
    }

    /// Reports whether the pack declares a subsystem.
    ///
    /// Returns `None` for legacy packs without a `[systems]` table.
    pub fn has_system(&self, name: &str) -> Option<bool> {
        if !self.systems_declared {
            return None;
        }
        Some(self.systems.included.iter().any(|s| s == name))
    }

    /// Returns declared subsystems as a bit mask in [`SUBSYSTEMS`] order.
    pub fn systems_mask(&self) -> u32 {
        let mut m = 0u32;
        for (i, name) in SUBSYSTEMS.iter().enumerate() {
            if self.systems.included.iter().any(|s| s == name) {
                m |= 1 << i;
            }
        }
        m
    }

    // -----------------------------------------------------------------------
    // — perfiles de remasterización
    // -----------------------------------------------------------------------

    /// Returns selectable profiles with the implicit original profile first.
    ///
    /// A pack without authored profiles receives a generated full-pack profile.
    pub fn profiles(&self) -> Vec<ProfileDef> {
        let mut out = vec![ProfileDef {
            id: ORIGINAL_PROFILE.to_string(),
            name: Some("Original".to_string()),
            description: Some("The unmodified game without substitutions".to_string()),
            systems: Vec::new(),
            muted_buses: Vec::new(),
            default: false,
        }];
        if self.profiles.is_empty() {
            // `[systems]` AUSENTE no es `[systems]` vacío — la distinción de
            //, que acá decide si un pack viejo sigue funcionando.
            //
            // Un pack legacy no declara nada, y su comportamiento de siempre es
            // sustituir TODO lo que tenga. Derivar el perfil de una lista vacía
            // haría que cargarlo apagara los ocho subsistemas: el pack se
            // abriría, no fallaría nada, y el juego se vería sin remasterizar.
            // Lo encontró el oráculo de sobre un pack de Sonic 2 real.
            let systems = if self.systems_declared {
                self.systems.included.clone()
            } else {
                SUBSYSTEMS.iter().map(|s| s.to_string()).collect()
            };
            out.push(ProfileDef {
                id: "full".to_string(),
                name: Some("Complete".to_string()),
                description: Some("Every subsystem included by the pack".to_string()),
                systems,
                muted_buses: Vec::new(),
                default: true,
            });
        } else {
            out.extend(self.profiles.iter().cloned());
        }
        out
    }

    /// Returns the profile selected when the pack opens.
    pub fn default_profile(&self) -> ProfileDef {
        let all = self.profiles();
        all.iter()
            .find(|p| p.default)
            .cloned()
            .unwrap_or_else(|| all[0].clone())
    }

    /// Looks up a selectable profile by stable identifier.
    pub fn profile(&self, id: &str) -> Option<ProfileDef> {
        self.profiles().into_iter().find(|p| p.id == id)
    }

    /// Returns the included resolution tiers as a bit mask.
    pub fn tiers_mask(&self) -> u8 {
        self.tiers_mask
    }

    /// Returns the selected tier, or `None` for a flat legacy pack.
    pub fn active_tier(&self) -> Option<u8> {
        self.resolved_tier
    }

    /// Selects the smallest included tier at least as large as `ideal`.
    ///
    /// Falls back to the largest included tier and does nothing for legacy packs.
    pub fn set_tier(&mut self, ideal: u8) {
        if self.tiers_mask == 0 {
            return;
        }
        let mut pick: Option<u8> = None;
        for t in ideal..8 {
            if self.tiers_mask & (1 << t) != 0 {
                pick = Some(t);
                break;
            }
        }
        if pick.is_none() {
            pick = Some(7 - self.tiers_mask.leading_zeros() as u8);
        }
        self.resolved_tier = pick;
    }

    /// Resolve a logical path to the actual entry name, applying regional
    /// overrides and the tier layout: `locales/<r>/` first, then
    /// `tiers/<activo>/<path>`, then the root (audio and legacy packs) — los
    /// consumidores siguen pidiendo el nombre pelado del catálogo.
    ///
    ///  el CONTENIDO vive bajo `assets/` y las tablas del pack —manifest,
    /// integrity, los TOML de las Identidades— en la raíz. Antes estaban
    /// mezclados: un asset caía al lado de manifest.toml y nada en el layout
    /// decía cuál era cuál. Ahora la separación es estructural, que es lo que
    /// permite mirar un `.ay` y saber qué es dato y qué es índice sin abrir
    /// nada.
    ///
    /// Cada forma se prueba con el prefijo NUEVO y después sin él: un pack ya
    /// instalado no se re-hornea porque cambió el writer. Son búsquedas en un
    /// hash, así que el camino legacy no cuesta nada medible.
    fn resolve(&self, logical_path: &str) -> Option<String> {
        if let Some(ref region) = self.region {
            for p in [
                format!("assets/locales/{}/{}", region, logical_path),
                format!("locales/{}/{}", region, logical_path),
            ] {
                if self.contains_entry(&p) {
                    return Some(p);
                }
            }
        }
        if let Some(t) = self.resolved_tier {
            for p in [
                format!("assets/tiers/{}/{}", t, logical_path),
                format!("tiers/{}/{}", t, logical_path),
            ] {
                if self.contains_entry(&p) {
                    return Some(p);
                }
            }
        }
        let under = format!("assets/{}", logical_path);
        if self.contains_entry(&under) {
            return Some(under);
        }
        self.contains_entry(logical_path)
            .then(|| logical_path.to_string())
    }

    fn contains_entry(&self, actual: &str) -> bool {
        match &self.backend {
            Backend::Resident(m) => m.contains_key(actual),
            Backend::Lazy { index, .. } => index.contains_key(actual),
        }
    }

    /// Read a logical asset. Returns owned bytes: resident packs copy from
    /// RAM; lazy packs decompress from disk and verify the per-entry hash
    /// — a mismatch (tamper or torn hotreload write) logs and returns
    /// `None`, degrading the single asset instead of the whole pack.
    pub fn read(&self, logical_path: &str) -> Option<Vec<u8>> {
        let actual = self.resolve(logical_path)?;
        match &self.backend {
            Backend::Resident(m) => m.get(&actual).cloned(),
            Backend::Lazy { zip, index, .. } => {
                let le = index.get(&actual)?;
                let capacity = usize::try_from(le.size).ok()?;
                let mut data = Vec::with_capacity(capacity);
                {
                    let mut z = zip.lock().ok()?;
                    let mut entry = z.by_name(&actual).ok()?;
                    entry
                        .by_ref()
                        .take(MAX_ENTRY_BYTES + 1)
                        .read_to_end(&mut data)
                        .ok()?;
                }
                if data.len() as u64 != le.size {
                    return None;
                }
                if Sha256::digest(&data)[..] != le.sha256 {
                    eprintln!(
                        "[AyArchive] '{}': hash mismatch — entrada alterada o corrupta",
                        actual
                    );
                    return None;
                }
                Some(data)
            }
        }
    }

    /// Returns whether an entry supports independently verified ranged reads.
    pub fn is_streamable(&self, logical_path: &str) -> bool {
        let Some(actual) = self.resolve(logical_path) else {
            return false;
        };
        match &self.backend {
            Backend::Resident(_) => false,
            Backend::Lazy { index, .. } => index.get(&actual).is_some_and(|e| e.stream.is_some()),
        }
    }

    /// Reads `len` bytes at `off` without materializing the complete entry.
    ///
    /// Every touched chunk is verified against the signed index. The range is
    /// clipped at end of file. Returns `None` for unsupported, invalid, or
    /// unverifiable ranges.
    pub fn read_range(&self, logical_path: &str, off: u64, len: usize) -> Option<Vec<u8>> {
        let actual = self.resolve(logical_path)?;
        let (index, raw) = match &self.backend {
            Backend::Lazy { index, raw, .. } => (index, raw),
            Backend::Resident(_) => return None,
        };
        let le = index.get(&actual)?;
        let si = le.stream.as_ref()?;
        if off >= le.size || len == 0 {
            return None;
        }
        let end = off.checked_add(len as u64)?.min(le.size);

        // Superset alineado al trozo: se lee lo que hace falta para poder
        // VERIFICAR, y recién después se recorta a lo pedido.
        let first = off / si.chunk;
        let last = (end - 1) / si.chunk;
        let rs = first * si.chunk;
        let re = ((last + 1) * si.chunk).min(le.size);

        let mut buf = vec![0u8; (re - rs) as usize];
        {
            let mut f = raw.lock().ok()?;
            f.seek(SeekFrom::Start(si.data_start + rs)).ok()?;
            f.read_exact(&mut buf).ok()?;
        }
        for (i, ci) in (first..=last).enumerate() {
            let a = i * si.chunk as usize;
            let b = ((i + 1) * si.chunk as usize).min(buf.len());
            if Sha256::digest(&buf[a..b])[..] != si.hashes.get(ci as usize)?[..] {
                eprintln!(
                    "[AyArchive] '{}' trozo {}: hash mismatch — entrada \
                           alterada o corrupta",
                    actual, ci
                );
                return None;
            }
        }

        // drain + truncate en vez de `buf[a..b].to_vec()`: recortar en el mismo
        // buffer evita copiar hasta un trozo entero por lectura.
        buf.drain(..(off - rs) as usize);
        buf.truncate((end - off) as usize);
        Some(buf)
    }

    /// Return the size in bytes of a logical asset, or `None` if not found.
    /// Never touches asset data: resident packs know it, lazy packs take it
    /// from the signed index.
    pub fn file_size(&self, logical_path: &str) -> Option<usize> {
        let actual = self.resolve(logical_path)?;
        match &self.backend {
            Backend::Resident(m) => m.get(&actual).map(Vec::len),
            Backend::Lazy { index, .. } => index.get(&actual).map(|e| e.size as usize),
        }
    }

    /// Iterate over all logical paths (no `locales/` overlay paths, no
    /// `signature.bin` / `integrity.toml`).
    pub fn iter_paths(&self) -> impl Iterator<Item = &str> {
        let keys: Box<dyn Iterator<Item = &String>> = match &self.backend {
            Backend::Resident(m) => Box::new(m.keys()),
            Backend::Lazy { index, .. } => Box::new(index.keys()),
        };
        keys.filter(|p| {
            !p.starts_with("locales/")
                && p.as_str() != SIGNATURE_PATH
                && p.as_str() != INTEGRITY_PATH
        })
        .map(String::as_str)
    }
}

/// Decode a 64-char lowercase/uppercase hex string into 32 bytes.
fn hex32(s: &str) -> Option<[u8; 32]> {
    if s.len() != 64 || !s.is_ascii() {
        return None;
    }
    let mut out = [0u8; 32];
    for (i, b) in out.iter_mut().enumerate() {
        *b = u8::from_str_radix(&s[2 * i..2 * i + 2], 16).ok()?;
    }
    Some(out)
}

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pack_builder::PackBuilder;
    use std::io::Write;

    const MANIFEST: &str = r#"
[pack]
name       = "Test Pack"
version    = "1.0.0"
game_id    = "sonic2"
author     = "CI"
ayther_min = "0.4.0"

[regions]
default   = "NTSC"
supported = ["NTSC", "JP"]
"#;

    // -----------------------------------------------------------------------
    // — el manifest extendido
    // -----------------------------------------------------------------------

    /// Hornea un pack con el manifest dado y lo abre. Devuelve el error tal
    /// cual: lo que estos tests miden es *qué* pasa con un manifest raro, y
    /// «no abrió» sin el motivo no distingue un rechazo correcto de un bug.
    fn open_with_manifest(manifest: &str) -> Result<AyArchive, AyError> {
        // Un directorio por LLAMADA. El nombre salía del largo del manifest, y
        // `cargo test` corre en paralelo: dos tests con manifests del mismo
        // largo —o directamente con el mismo manifest— escribían el mismo
        // `p.ay` a la vez, y uno leía el zip a medio escribir («Could not find
        // EOCD»). Fallaba de a ratos y sólo al agregar tests nuevos, que es la
        // peor forma de fallar.
        //
        // Por contenido tampoco alcanza: dos tests distintos pueden abrir el
        // MISMO manifest, y ahí vuelven a pisarse.
        static SEQ: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let n = SEQ.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let dir = std::env::temp_dir().join(format!("ay_manifest_test_{}", n));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("p.ay");
        let mut b = PackBuilder::new();
        b.add_bytes("manifest.toml", manifest.as_bytes().to_vec());
        b.add_bytes("graphics/a.png", vec![0x89, 0x50, 0x4E, 0x47]);
        b.finish(true, &path).unwrap();
        AyArchive::open_verbose(path.to_str().unwrap())
    }

    /// Un pack de HOY sigue abriendo: no declara `schema`, ni `[systems]`, ni
    /// `[compat]`. Es el caso que más veces se rompe al extender un formato, y
    /// por eso va primero.
    #[test]
    fn legacy_manifest_still_opens() {
        let a = open_with_manifest(MANIFEST).expect("un pack legacy tiene que abrir");
        assert_eq!(a.schema, 1, "sin campo, el esquema es 1");
        assert!(!a.systems_declared, "no declaró [systems]");
        assert_eq!(
            a.has_system("music"),
            None,
            "sin declaración la respuesta es «no sé», NO «no»"
        );
        assert_eq!(a.systems_mask(), 0);
        assert!(a.compat.rom_crc32.is_none());
    }

    /// El manifest nuevo entero, ida y vuelta.
    #[test]
    fn new_manifest_is_fully_read() {
        let m = r#"
[pack]
name         = "Test Pack"
version      = "1.0.0"
game_id      = "crc32:7b905383"
author       = "CI"
ayther_min   = "0.8.0"
schema       = 2
contributors = ["Ana", "Beto"]
license      = "CC-BY-4.0"

[systems]
included = ["sprites", "music"]

[compat]
rom_crc32 = "7b905383"
platform  = "megadrive"
"#;
        let a = open_with_manifest(m).expect("el manifest nuevo tiene que abrir");
        assert_eq!(a.schema, 2);
        assert_eq!(a.meta.license.as_deref(), Some("CC-BY-4.0"));
        assert_eq!(a.meta.contributors, vec!["Ana", "Beto"]);
        assert_eq!(a.compat.platform.as_deref(), Some("megadrive"));
        assert_eq!(a.has_system("sprites"), Some(true));
        assert_eq!(
            a.has_system("tiles"),
            Some(false),
            "declarado y ausente = NO, distinto de «no sé»"
        );
        // bit 0 = sprites, bit 5 = music (orden de SUBSYSTEMS)
        assert_eq!(a.systems_mask(), (1 << 0) | (1 << 5));
    }

    /// Forward-compat de DATOS: una tabla y un campo que este build no conoce
    /// se ignoran. Es lo que permite publicar metadatos nuevos sin partir el
    /// parque de Engines instalados.
    #[test]
    fn unknown_fields_do_not_break_parsing() {
        let m = r#"
[pack]
name       = "Test Pack"
version    = "1.0.0"
game_id    = "sonic2"
ayther_min = "0.4.0"
schema     = 2
cosa_nueva = "de un Engine posterior"

[tabla_del_futuro]
lo_que_sea = 42
"#;
        let a = open_with_manifest(m).expect("los campos desconocidos se ignoran");
        assert_eq!(a.schema, 2);
        assert_eq!(a.meta.name, "Test Pack");
    }

    /// …y el límite de esa tolerancia: un esquema MAYOR se rechaza. Ignorar
    /// campos sueltos es forward-compat; abrir un pack que dice depender de
    /// algo que este build no sabe leer sería servirlo a medias y llamarlo
    /// éxito — el modo de fallo que este repo ya pagó con dos días de audio
    /// mudo en verde.
    #[test]
    fn higher_schema_is_rejected_with_versions() {
        let m = format!(
            r#"
[pack]
name       = "Del futuro"
version    = "9.0.0"
game_id    = "sonic2"
ayther_min = "0.4.0"
schema     = {}
"#,
            MANIFEST_SCHEMA + 1
        );
        match open_with_manifest(&m) {
            Err(AyError::UnsupportedSchema { found, max }) => {
                assert_eq!(found, MANIFEST_SCHEMA + 1);
                assert_eq!(max, MANIFEST_SCHEMA);
            }
            other => panic!(
                "esperaba UnsupportedSchema, salió {:?}",
                other.map(|_| "abrió")
            ),
        }
    }

    // -----------------------------------------------------------------------
    // — perfiles de remasterización
    // -----------------------------------------------------------------------

    const WITH_PROFILES: &str = r#"
[pack]
name       = "Con perfiles"
version    = "1.0.0"
game_id    = "sonic2"
ayther_min = "0.4.0"
schema     = 2

[systems]
included = ["sprites", "metasprites", "tiles", "planes", "music", "sfx"]

[[profile]]
id      = "faithful"
name    = "Fiel"
systems = ["sprites", "metasprites"]

[[profile]]
id      = "enhanced"
name    = "Mejorado"
systems = ["sprites", "metasprites", "tiles", "planes", "music", "sfx"]
default = true
"#;

    /// «original» SIEMPRE está y va primero. Comparar con el juego sin
    /// remasterizar es la operación fundamental del producto: hacerla depender
    /// de que el autor la declare sería hacerla depender de un olvido.
    #[test]
    fn original_profile_is_implicit_and_first() {
        let a = open_with_manifest(WITH_PROFILES).unwrap();
        let ps = a.profiles();
        assert_eq!(ps[0].id, ORIGINAL_PROFILE);
        assert!(ps[0].systems.is_empty(), "original no enciende nada");
        assert_eq!(ps.len(), 3, "el implícito + los dos declarados");
        assert_eq!(a.profile(ORIGINAL_PROFILE).unwrap().systems_mask(), 0);
    }

    /// Un pack SIN perfiles no es un caso especial: tiene el implícito más uno
    /// derivado de `[systems]`. Ese perfil hace exactamente lo que el pack ya
    /// hacía —encender todo— así que nombrarlo no cambia el comportamiento y en
    /// cambio deja que la UI hable de perfiles siempre y no a veces.
    #[test]
    fn pack_without_profiles_offers_two() {
        let a = open_with_manifest(MANIFEST).unwrap();
        let ps = a.profiles();
        assert_eq!(ps.len(), 2);
        assert_eq!(ps[0].id, ORIGINAL_PROFILE);
        assert_eq!(ps[1].id, "full");
        assert!(ps[1].default, "y el default es el completo, no el original");
        assert_eq!(a.default_profile().id, "full");
    }

    /// UN PACK LEGACY NO SE APAGA SOLO.
    ///
    /// `[systems]` ausente no es `[systems]` vacío. Derivar el perfil
    /// «completo» de una lista vacía hacía que cargar un pack viejo apagara los
    /// ocho subsistemas: el pack abría, no fallaba nada, y el juego se veía sin
    /// remasterizar. Lo encontró el oráculo de sobre un pack de Sonic 2
    /// real — un test del parser solo no lo habría visto.
    #[test]
    fn legacy_pack_enables_all_systems() {
        let a = open_with_manifest(MANIFEST).unwrap();
        assert!(
            !a.systems_declared,
            "el manifest de prueba no declara [systems]"
        );
        let full = a.profiles().into_iter().find(|p| p.id == "full").unwrap();
        assert_eq!(
            full.systems_mask(),
            (1 << SUBSYSTEMS.len()) - 1,
            "sin [systems] el perfil completo enciende TODO"
        );

        // Y el par: un pack que SÍ declara y declara poco, enciende poco. Sin
        // este check, el de arriba pasaría también si «completo» encendiera
        // todo siempre — que sería ignorar lo que el pack dice traer.
        let m = format!("{}\n[systems]\nincluded = [\"sprites\"]\n", MANIFEST);
        let b = open_with_manifest(&m).unwrap();
        let bf = b.profiles().into_iter().find(|p| p.id == "full").unwrap();
        assert_eq!(bf.systems_mask(), 1);

        // Declarado y VACÍO es la tercera cosa: el pack dice que no trae nada.
        let m2 = format!("{}\n[systems]\nincluded = []\n", MANIFEST);
        let c = open_with_manifest(&m2).unwrap();
        let cf = c.profiles().into_iter().find(|p| p.id == "full").unwrap();
        assert_eq!(cf.systems_mask(), 0);
    }

    /// El default es EXACTAMENTE uno. Ninguno dejaría al runtime eligiendo; dos
    /// harían que el pack se viera distinto según cómo se recorra la lista.
    #[test]
    fn exactly_one_default_exists() {
        let a = open_with_manifest(WITH_PROFILES).unwrap();
        assert_eq!(a.default_profile().id, "enhanced");
        assert_eq!(a.profiles().iter().filter(|p| p.default).count(), 1);

        // Sin ninguno declarado gana el primero — un pack arranca siempre en un
        // perfil concreto.
        let m = WITH_PROFILES.replace("default = true", "");
        let b = open_with_manifest(&m).unwrap();
        assert_eq!(b.default_profile().id, "faithful");

        // Con dos, gana el primero y el otro deja de serlo.
        let m2 = WITH_PROFILES.replace(
            r#"systems = ["sprites", "metasprites"]"#,
            "systems = [\"sprites\"]\ndefault = true",
        );
        let c = open_with_manifest(&m2).unwrap();
        assert_eq!(c.default_profile().id, "faithful");
        assert_eq!(c.profiles().iter().filter(|p| p.default).count(), 1);
    }

    /// Los perfiles rotos se descartan al ABRIR y no al consultar: uno que
    /// sobrevive hasta el punto de uso obliga a cada consumidor a repetir la
    /// comprobación, y el que se la olvide aplica una máscara vacía y muestra
    /// el juego sin remasterizar sin que nada avise.
    #[test]
    fn broken_profiles_are_discarded() {
        let m = format!(
            r#"{}
[[profile]]
id = ""
systems = ["tiles"]

[[profile]]
id = "faithful"
name = "El impostor"
systems = ["planes"]

[[profile]]
id = "original"
systems = ["tiles"]
"#,
            WITH_PROFILES
        );
        let a = open_with_manifest(&m).unwrap();
        let ps = a.profiles();
        assert_eq!(ps.len(), 3, "el vacío, el repetido y el «original» se van");
        // Gana el PRIMERO: quedarse con el último haría que agregar un perfil
        // al final cambiara en silencio lo que hace uno que ya existía.
        assert_eq!(a.profile("faithful").unwrap().display_name(), "Fiel");
        // Y «original» sigue siendo el implícito, no el declarado.
        assert_eq!(a.profile(ORIGINAL_PROFILE).unwrap().systems_mask(), 0);
    }

    /// A profile filters the pack's content rather than multiplying it: the mask
    /// is derived from subsystem names, guarding against accidental duplication.
    #[test]
    fn profile_mask_uses_known_names() {
        let a = open_with_manifest(WITH_PROFILES).unwrap();
        let f = a.profile("faithful").unwrap();
        assert_eq!(f.systems_mask(), 0b11, "sprites + metasprites");
        assert_eq!(
            a.profile("enhanced").unwrap().systems_mask(),
            a.systems_mask()
        );
        assert!(
            a.profile("no-existe").is_none(),
            "«no existe» y «no enciende nada» son cosas distintas"
        );

        // Un subsistema que este build no conoce se ignora en vez de romper el
        // perfil: un pack de mañana puede declarar más, y eso no invalida lo
        // que sí existe hoy.
        let m = WITH_PROFILES.replace(
            r#"["sprites", "metasprites"]"#,
            r#"["sprites", "holografia"]"#,
        );
        let b = open_with_manifest(&m).unwrap();
        assert_eq!(b.profile("faithful").unwrap().systems_mask(), 0b1);
    }

    /// Build an in-memory file map matching a minimal valid pack
    /// (no signature — dev build only).
    fn minimal_files() -> HashMap<String, Vec<u8>> {
        let mut m = HashMap::new();
        m.insert("manifest.toml".to_string(), MANIFEST.as_bytes().to_vec());
        m.insert(
            "graphics/tile_0001.png".to_string(),
            vec![0x89, 0x50, 0x4E, 0x47],
        ); // PNG magic
        m
    }

    ///  el BUILD ID identifica un horneado y se DERIVA, no se declara.
    ///
    /// Las dos propiedades que lo hacen servir para diagnosticar un pack cuyos
    /// assets se nombran por hash, y que este test existe para fijar:
    ///
    ///  1. DOS HORNEADOS IDENTICOS DAN EL MISMO ID. Sin esto, el log de build
    ///     del autor deja de resolver en cuanto vuelve a hornear sin cambiar
    ///     nada — que es la mitad de las veces.
    ///  2. CAMBIAR UN SOLO BYTE DE UN ASSET LO CAMBIA. Sin esto, el id no
    ///     distingue horneados y resolver un hash contra el log daria el archivo
    ///     equivocado, que es peor que no dar ninguno.
    ///
    /// Y un tercero que es la razon de que se derive en vez de declararse: un
    /// campo del manifest se puede editar sin que nada se entere. Aca no hay
    /// campo — se recalcula de `integrity.toml`, que es lo que firma la firma.
    #[test]
    fn build_id_is_derived_and_stable() {
        let bake = |path: &std::path::Path, asset: &[u8]| {
            let mut b = PackBuilder::new();
            b.add_bytes("manifest.toml", MANIFEST.as_bytes().to_vec());
            b.add_bytes("graphics/a.png", asset.to_vec());
            b.finish(true, path).unwrap();
        };

        let p1 = tmp("bid_1.ay");
        let p2 = tmp("bid_2.ay");
        let p3 = tmp("bid_3.ay");
        bake(&p1, b"contenido");
        bake(&p2, b"contenido"); // identico
        bake(&p3, b"contenidos"); // un byte distinto

        let a1 = AyArchive::open(p1.to_str().unwrap()).unwrap();
        let a2 = AyArchive::open(p2.to_str().unwrap()).unwrap();
        let a3 = AyArchive::open(p3.to_str().unwrap()).unwrap();

        assert!(!a1.build_id.is_empty(), "un pack lazy tiene build id");
        assert_eq!(a1.build_id.len(), 12, "12 hex: entra en una linea de log");
        assert_eq!(
            a1.build_id, a2.build_id,
            "dos horneados IDENTICOS dan el mismo build id"
        );
        assert_ne!(
            a1.build_id, a3.build_id,
            "un byte distinto en un asset cambia el build id"
        );

        // Y es exactamente el digest de lo que firma signature.bin: no hay una
        // segunda fuente de verdad que se pueda desincronizar.
        let f = std::fs::File::open(&p1).unwrap();
        let mut z = zip::ZipArchive::new(f).unwrap();
        let mut raw = Vec::new();
        z.by_name(INTEGRITY_PATH)
            .unwrap()
            .read_to_end(&mut raw)
            .unwrap();
        assert_eq!(
            a1.build_id,
            build_id_from_integrity(&raw),
            "el build id es el digest de integrity.toml, sin intermediarios"
        );

        for p in [p1, p2, p3] {
            let _ = std::fs::remove_file(p);
        }
    }

    fn tmp(name: &str) -> std::path::PathBuf {
        let mut p = std::env::temp_dir();
        p.push(format!("ae_vfs_{}_{}", std::process::id(), name));
        p
    }

    /// Rewrite `src` into `dst`, mapping each entry through `f`
    /// (None = drop the entry) and appending `extra` entries at the end.
    /// Used to simulate post-signing tampering.
    fn rezip(
        src: &std::path::Path,
        dst: &std::path::Path,
        f: impl Fn(&str, Vec<u8>) -> Option<Vec<u8>>,
        extra: &[(&str, &[u8])],
    ) {
        let file = std::fs::File::open(src).unwrap();
        let mut zin = zip::ZipArchive::new(file).unwrap();
        let out = std::fs::File::create(dst).unwrap();
        let mut zout = zip::ZipWriter::new(out);
        let opts = zip::write::SimpleFileOptions::default()
            .compression_method(zip::CompressionMethod::Deflated);
        //  conservar Stored donde corresponde. Si el re-zip deflateara un
        // `.ivf`, la entrada dejaría de ser direccionable por rango y los tests
        // de manoseo medirían la caída del streaming en vez del hash del trozo.
        let stored = zip::write::SimpleFileOptions::default()
            .compression_method(zip::CompressionMethod::Stored);
        let method_for = |p: &str| if is_stored_entry(p) { stored } else { opts };
        for i in 0..zin.len() {
            let mut e = zin.by_index(i).unwrap();
            if e.is_dir() {
                continue;
            }
            let name = e.name().to_string();
            let mut data = Vec::new();
            e.read_to_end(&mut data).unwrap();
            if let Some(new_data) = f(&name, data) {
                zout.start_file(&name, method_for(&name)).unwrap();
                zout.write_all(&new_data).unwrap();
            }
        }
        for (name, data) in extra {
            zout.start_file(*name, method_for(name)).unwrap();
            zout.write_all(data).unwrap();
        }
        zout.finish().unwrap();
    }

    fn build_signed_pack(out: &std::path::Path) -> Vec<u8> {
        let asset = b"payload-payload-payload".to_vec();
        let mut b = PackBuilder::new();
        assert!(b.add_bytes("manifest.toml", MANIFEST.as_bytes().to_vec()));
        assert!(b.add_bytes("graphics/a.png", asset.clone()));
        b.finish(true, out).expect("finish");
        asset
    }

    #[test]
    fn content_hash_is_deterministic() {
        let files = minimal_files();
        let h1 = AyArchive::compute_content_hash(&files);
        let h2 = AyArchive::compute_content_hash(&files);
        assert_eq!(h1, h2);
        assert_eq!(h1.len(), 32);
    }

    #[test]
    fn content_hash_excludes_signature_bin() {
        let mut files = minimal_files();
        let h1 = AyArchive::compute_content_hash(&files);
        files.insert("signature.bin".to_string(), vec![0u8; 64]);
        let h2 = AyArchive::compute_content_hash(&files);
        assert_eq!(h1, h2, "signature.bin must not affect content hash");
    }

    #[test]
    fn content_hash_changes_with_content() {
        let files1 = minimal_files();
        let mut files2 = minimal_files();
        files2.insert("graphics/tile_0001.png".to_string(), vec![0x00]);
        assert_ne!(
            AyArchive::compute_content_hash(&files1),
            AyArchive::compute_content_hash(&files2)
        );
    }

    /// Build a mock resident archive directly (skips file I/O + signature).
    fn mock_resident(
        files: HashMap<String, Vec<u8>>,
        tiers_mask: u8,
        resolved_tier: Option<u8>,
    ) -> AyArchive {
        let raw: RawManifest =
            toml::from_str(std::str::from_utf8(files.get("manifest.toml").unwrap()).unwrap())
                .unwrap();
        let meta = raw.pack;
        let meta_schema = meta.schema.unwrap_or(1);
        let systems_declared = raw.systems.is_some();
        let game_id_cstr = std::ffi::CString::new(meta.game_id.as_str()).unwrap_or_default();
        AyArchive {
            backend: Backend::Resident(files),
            meta,
            regions: raw.regions,
            region: None,
            tiers_mask,
            resolved_tier,
            game_id_cstr,
            build_id: String::new(),
            build_id_cstr: std::ffi::CString::default(),
            schema: meta_schema,
            systems: raw.systems.unwrap_or_default(),
            compat: raw.compat.unwrap_or_default(),
            systems_declared,
            profiles: sanitize_profiles(raw.profiles),
            meta_field_cstr: std::ffi::CString::default(),
        }
    }

    #[test]
    fn regional_override() {
        let mut files = minimal_files();
        files.insert(
            "locales/JP/graphics/tile_0001.png".to_string(),
            b"jp_version".to_vec(),
        );
        let mut archive = mock_resident(files, 0, None);

        // Without region: default asset
        assert_eq!(
            archive.read("graphics/tile_0001.png"),
            Some(vec![0x89u8, 0x50, 0x4E, 0x47])
        );

        // With region JP: override
        archive.set_region("JP");
        assert_eq!(
            archive.read("graphics/tile_0001.png"),
            Some(b"jp_version".to_vec())
        );

        // Unknown path → None
        assert!(archive.read("graphics/nonexistent.png").is_none());
    }

    ///  layout multi-tier — `tiers/<t>/<nombre>` transparente al lookup,
    /// selección menor-incluido-≥-ideal con fallback al mayor, raíz para lo
    /// tier-independiente (audio) y packs legacy intactos.
    #[test]
    fn tier_overlay_and_selection() {
        let mut files = minimal_files();
        files.insert("tiers/0/a.png".to_string(), b"hd".to_vec());
        files.insert("tiers/1/a.png".to_string(), b"fullhd".to_vec());
        files.insert("boom.wav".to_string(), b"pcm".to_vec());
        // HD + Full HD (una Versión de Entregar); default del open: el más alto.
        let mut a = mock_resident(files, 0b0011, Some(1));

        // Default (más alto incluido): Full HD.
        assert_eq!(a.read("a.png"), Some(b"fullhd".to_vec()));
        // Display 720p → tier exacto HD.
        a.set_tier(0);
        assert_eq!(a.read("a.png"), Some(b"hd".to_vec()));
        // Display 4K (tier 2 no incluido) → el MAYOR disponible (Full HD),
        // nunca "upscalear" eligiendo de menos habiendo más.
        a.set_tier(2);
        assert_eq!(a.active_tier(), Some(1));
        assert_eq!(a.read("a.png"), Some(b"fullhd".to_vec()));
        // Audio tier-independiente: en la raíz, se lee igual con tier activo.
        assert_eq!(a.read("boom.wav"), Some(b"pcm".to_vec()));

        // Pack LEGACY (sin [tiers]): set_tier es no-op y la raíz manda.
        let mut legacy_files = minimal_files();
        legacy_files.insert("a.png".to_string(), b"flat".to_vec());
        let mut legacy = mock_resident(legacy_files, 0, None);
        legacy.set_tier(2);
        assert_eq!(legacy.active_tier(), None);
        assert_eq!(legacy.read("a.png"), Some(b"flat".to_vec()));
    }

    // -----------------------------------------------------------------------
    // — lazy open + per-entry verification
    // -----------------------------------------------------------------------

    #[test]
    fn lazy_signed_pack_round_trips() {
        let out = tmp("lazy_ok.ay");
        let asset = build_signed_pack(&out);

        let arch = AyArchive::open_verbose(out.to_str().unwrap())
            .expect("signed lazy pack must open + verify");
        assert!(
            matches!(arch.backend, Backend::Lazy { .. }),
            "pack con integrity.toml debe abrir lazy"
        );
        // file_size sale del índice firmado, sin descomprimir.
        assert_eq!(arch.file_size("graphics/a.png"), Some(asset.len()));
        // read descomprime + verifica hash.
        assert_eq!(arch.read("graphics/a.png"), Some(asset.clone()));
        // iter_paths no expone integrity.toml.
        assert!(
            arch.iter_paths()
                .all(|p| p != INTEGRITY_PATH && p != SIGNATURE_PATH)
        );
        let _ = std::fs::remove_file(&out);
    }

    #[test]
    fn lazy_tampered_asset_fails_only_that_read() {
        let out = tmp("lazy_src.ay");
        build_signed_pack(&out);
        let tampered = tmp("lazy_tampered.ay");
        // Mismo tamaño, contenido cambiado: pasa el cross-check del open,
        // cae en la verificación por-asset del read.
        rezip(
            &out,
            &tampered,
            |name, mut data| {
                if name == "graphics/a.png" {
                    data[0] ^= 0xFF;
                }
                Some(data)
            },
            &[],
        );

        let arch = AyArchive::open_verbose(tampered.to_str().unwrap())
            .expect("open no toca los assets, así que abre");
        assert_eq!(
            arch.read("graphics/a.png"),
            None,
            "asset alterado debe fallar el hash por entrada"
        );
        // El resto del pack sigue sano.
        assert!(arch.read("manifest.toml").is_some());
        let _ = std::fs::remove_file(&out);
        let _ = std::fs::remove_file(&tampered);
    }

    // -----------------------------------------------------------------------
    // — lectura por rango
    // -----------------------------------------------------------------------

    /// Pack con un `.ivf` de 70000 bytes: más de un trozo de 64 KiB, con el
    /// último corto. Devuelve el contenido esperado.
    fn build_pack_with_video(out: &std::path::Path) -> Vec<u8> {
        let clip: Vec<u8> = (0..70000u32).map(|i| (i % 251) as u8).collect();
        let mut b = PackBuilder::new();
        assert!(b.add_bytes("manifest.toml", MANIFEST.as_bytes().to_vec()));
        assert!(b.add_bytes("graphics/a.png", b"deflateado".to_vec()));
        assert!(b.add_bytes("video/clip.ivf", clip.clone()));
        b.finish(true, out).expect("finish");
        clip
    }

    #[test]
    fn range_read_matches_whole_read() {
        let out = tmp("range_ok.ay");
        let clip = build_pack_with_video(&out);
        let arch = AyArchive::open_verbose(out.to_str().unwrap()).expect("open");

        assert!(
            arch.is_streamable("video/clip.ivf"),
            "un .ivf va Stored y con trozos: tiene que ser direccionable"
        );
        assert!(
            !arch.is_streamable("graphics/a.png"),
            "una entrada deflateada NO se puede leer por rango"
        );
        assert!(!arch.is_streamable("video/no_existe.ivf"));

        // El header IVF, un frame del medio, y rangos que cruzan el borde de
        // trozo (65536) — que es donde un off-by-one se vería.
        for &(off, len) in &[
            (0u64, 32usize),
            (12, 187),
            (65530, 12),
            (65536, 64),
            (1, 65535),
            (69999, 1),
        ] {
            let got = arch
                .read_range("video/clip.ivf", off, len)
                .unwrap_or_else(|| panic!("rango {}+{} debe leerse", off, len));
            assert_eq!(
                got,
                &clip[off as usize..off as usize + len],
                "rango {}+{} distinto del contenido",
                off,
                len
            );
        }

        // Un pedido que se pasa del final se RECORTA (no falla): así el último
        // bloque de un barrido secuencial no necesita saber el tamaño exacto.
        assert_eq!(
            arch.read_range("video/clip.ivf", 69990, 4096)
                .unwrap()
                .len(),
            10
        );
        // Fuera de rango del todo, y largo cero: None, no un vector vacío.
        assert!(arch.read_range("video/clip.ivf", 70000, 16).is_none());
        assert!(arch.read_range("video/clip.ivf", 0, 0).is_none());

        // Y lo leído por rango es EXACTAMENTE lo que da read() completo.
        assert_eq!(arch.read("video/clip.ivf").unwrap(), clip);
        let _ = std::fs::remove_file(&out);
    }

    /// La propiedad que hace que no afloje  un byte cambiado NO pasa,
    /// y la degradación es del TROZO — el resto de la entrada sigue sirviendo.
    /// Con la verificación por entrada esto era todo-o-nada.
    #[test]
    fn range_read_rejects_only_the_tampered_chunk() {
        let out = tmp("range_src.ay");
        build_pack_with_video(&out);
        let tampered = tmp("range_tampered.ay");
        rezip(
            &out,
            &tampered,
            |name, mut data| {
                if name == "video/clip.ivf" {
                    data[100] ^= 0xFF;
                } // primer trozo
                Some(data)
            },
            &[],
        );

        let arch = AyArchive::open_verbose(tampered.to_str().unwrap())
            .expect("el open no toca los datos del asset");
        assert!(
            arch.read_range("video/clip.ivf", 100, 16).is_none(),
            "el trozo alterado no puede entregarse"
        );
        assert!(
            arch.read_range("video/clip.ivf", 0, 32).is_none(),
            "cualquier rango DENTRO de ese trozo cae igual"
        );
        assert!(
            arch.read_range("video/clip.ivf", 65536, 64).is_some(),
            "el segundo trozo está sano: la degradación es por trozo"
        );
        assert!(
            arch.read("video/clip.ivf").is_none(),
            "y la lectura entera sigue fallando por el hash de entrada"
        );
        let _ = std::fs::remove_file(&out);
        let _ = std::fs::remove_file(&tampered);
    }

    /// Un pack legacy (sin integrity.toml) no tiene índice ni handle: el
    /// llamador tiene que poder distinguirlo y caer a read().
    #[test]
    fn range_read_unavailable_on_legacy_pack() {
        let out = tmp("range_legacy.ay");
        build_pack_with_video(&out);
        let legacy = tmp("range_legacy_stripped.ay");
        rezip(
            &out,
            &legacy,
            |name, d| (name != INTEGRITY_PATH && name != SIGNATURE_PATH).then_some(d),
            &[],
        );

        let arch = AyArchive::open_verbose(legacy.to_str().unwrap()).expect("legacy abre");
        assert!(matches!(arch.backend, Backend::Resident(_)));
        assert!(!arch.is_streamable("video/clip.ivf"));
        assert!(arch.read_range("video/clip.ivf", 0, 32).is_none());
        assert!(
            arch.read("video/clip.ivf").is_some(),
            "pero read() sigue andando"
        );
        let _ = std::fs::remove_file(&out);
        let _ = std::fs::remove_file(&legacy);
    }

    #[test]
    fn lazy_extra_entry_rejected_at_open() {
        let out = tmp("lazy_src2.ay");
        build_signed_pack(&out);
        let tampered = tmp("lazy_extra.ay");
        rezip(
            &out,
            &tampered,
            |_, d| Some(d),
            &[("graphics/smuggled.png", b"evil")],
        );

        assert!(
            matches!(
                AyArchive::open_verbose(tampered.to_str().unwrap()),
                Err(AyError::BadSignature)
            ),
            "entrada no listada en integrity.toml debe rechazarse al abrir"
        );
        let _ = std::fs::remove_file(&out);
        let _ = std::fs::remove_file(&tampered);
    }

    #[test]
    fn lazy_tampered_integrity_rejected_at_open() {
        let out = tmp("lazy_src3.ay");
        build_signed_pack(&out);
        let tampered = tmp("lazy_bad_integrity.ay");
        rezip(
            &out,
            &tampered,
            |name, mut data| {
                if name == INTEGRITY_PATH {
                    // Cambiar un byte del índice invalida la firma sobre sus bytes.
                    let pos = data.len() / 2;
                    data[pos] ^= 0x01;
                }
                Some(data)
            },
            &[],
        );

        assert!(matches!(
            AyArchive::open_verbose(tampered.to_str().unwrap()),
            Err(AyError::BadSignature)
        ));
        let _ = std::fs::remove_file(&out);
        let _ = std::fs::remove_file(&tampered);
    }

    #[test]
    fn stripping_integrity_cannot_downgrade_to_legacy() {
        let out = tmp("lazy_src4.ay");
        build_signed_pack(&out);
        let stripped = tmp("lazy_stripped.ay");
        // Sin integrity.toml el open cae al camino legacy, donde la firma
        // (que cubre los bytes de integrity.toml) no puede coincidir con el
        // content hash → BadSignature, no un pack "válido" sin verificación.
        rezip(
            &out,
            &stripped,
            |name, data| {
                if name == INTEGRITY_PATH {
                    None
                } else {
                    Some(data)
                }
            },
            &[],
        );

        assert!(matches!(
            AyArchive::open_verbose(stripped.to_str().unwrap()),
            Err(AyError::BadSignature)
        ));
        let _ = std::fs::remove_file(&out);
        let _ = std::fs::remove_file(&stripped);
    }

    /// Medición, no test: apertura lazy vs residente con un pack grande.
    /// `cargo test -p ayther_core --release bench_lazy -- --ignored --nocapture`
    /// Assets pseudo-aleatorios (incompresibles) para que el deflate no
    /// disimule el costo del camino residente.
    #[test]
    #[ignore]
    fn bench_lazy_open_vs_resident() {
        let n_assets = 40usize;
        let asset_mb = 5usize;
        let mut rng: u64 = 0x243F6A8885A308D3; // xorshift — sin Math.random
        let mut asset = vec![0u8; asset_mb * 1024 * 1024];
        for chunk in asset.chunks_mut(8) {
            rng ^= rng << 13;
            rng ^= rng >> 7;
            rng ^= rng << 17;
            let b = rng.to_le_bytes();
            chunk.copy_from_slice(&b[..chunk.len()]);
        }

        let lazy_path = tmp("bench_lazy.ay");
        {
            let mut b = PackBuilder::new();
            b.add_bytes("manifest.toml", MANIFEST.as_bytes().to_vec());
            for i in 0..n_assets {
                let mut a = asset.clone();
                a[0] = i as u8; // entradas distintas
                b.add_bytes(&format!("graphics/a{:03}.png", i), a);
            }
            b.finish(true, &lazy_path).unwrap();
        }
        // Variante legacy: mismo contenido, sin integrity.toml, firmada al
        // estilo pre-(content hash) — así el bench también corre en
        // --release, donde un pack sin firma se rechaza.
        let legacy_path = tmp("bench_legacy.ay");
        {
            let file = std::fs::File::open(&lazy_path).unwrap();
            let mut zin = zip::ZipArchive::new(file).unwrap();
            let mut files: HashMap<String, Vec<u8>> = HashMap::new();
            for i in 0..zin.len() {
                let mut e = zin.by_index(i).unwrap();
                let name = e.name().to_string();
                if name == INTEGRITY_PATH || name == SIGNATURE_PATH {
                    continue;
                }
                let mut d = Vec::new();
                e.read_to_end(&mut d).unwrap();
                files.insert(name, d);
            }
            let hash = AyArchive::compute_content_hash(&files);
            // RFC 8032 test vector — el mismo DEV_SIGNING_SEED de PackBuilder.
            let seed: [u8; 32] = [
                0x4c, 0xcd, 0x08, 0x9b, 0x28, 0xff, 0x96, 0xda, 0x9d, 0xb6, 0xc3, 0x46, 0xec, 0x11,
                0x4e, 0x0f, 0x5b, 0x8a, 0x31, 0x9f, 0x35, 0xab, 0xa6, 0x24, 0xda, 0x8c, 0xf6, 0xed,
                0x4d, 0x0b, 0xd6, 0xd9,
            ];
            use ed25519_dalek::Signer;
            let sig = ed25519_dalek::SigningKey::from_bytes(&seed).sign(&hash);
            files.insert(SIGNATURE_PATH.to_string(), sig.to_bytes().to_vec());

            let out = std::fs::File::create(&legacy_path).unwrap();
            let mut zout = zip::ZipWriter::new(out);
            let opts = zip::write::SimpleFileOptions::default()
                .compression_method(zip::CompressionMethod::Deflated);
            for (name, data) in &files {
                zout.start_file(name, opts).unwrap();
                zout.write_all(data).unwrap();
            }
            zout.finish().unwrap();
        }

        let t0 = std::time::Instant::now();
        let lazy = AyArchive::open_verbose(lazy_path.to_str().unwrap()).unwrap();
        let t_lazy_open = t0.elapsed();

        let t0 = std::time::Instant::now();
        let resident = AyArchive::open_verbose(legacy_path.to_str().unwrap()).unwrap();
        let t_resident_open = t0.elapsed();

        let t0 = std::time::Instant::now();
        let data = lazy.read("graphics/a007.png").unwrap();
        let t_lazy_first_read = t0.elapsed();
        assert_eq!(data.len(), asset.len());
        assert!(resident.read("graphics/a007.png").is_some());

        println!(
            "pack: {} assets x {} MB = {} MB",
            n_assets,
            asset_mb,
            n_assets * asset_mb
        );
        println!(
            "open lazy      : {:>8.1?}  (directorio + manifest + integrity)",
            t_lazy_open
        );
        println!(
            "open residente : {:>8.1?}  (todo a RAM + hash completo)",
            t_resident_open
        );
        println!(
            "primer read    : {:>8.1?}  (inflate + sha256 de 1 asset)",
            t_lazy_first_read
        );

        let _ = std::fs::remove_file(&lazy_path);
        let _ = std::fs::remove_file(&legacy_path);
    }

    #[test]
    fn legacy_pack_without_integrity_still_opens() {
        // Pack pre- manifest + asset, sin integrity.toml ni firma
        // (unsigned abre en builds de debug, que es donde corren los tests).
        let out = tmp("legacy.ay");
        {
            let file = std::fs::File::create(&out).unwrap();
            let mut z = zip::ZipWriter::new(file);
            let opts = zip::write::SimpleFileOptions::default()
                .compression_method(zip::CompressionMethod::Deflated);
            z.start_file("manifest.toml", opts).unwrap();
            z.write_all(MANIFEST.as_bytes()).unwrap();
            z.start_file("graphics/a.png", opts).unwrap();
            z.write_all(b"flat-legacy").unwrap();
            z.finish().unwrap();
        }
        let arch = AyArchive::open_verbose(out.to_str().unwrap())
            .expect("legacy unsigned pack must open in dev builds");
        assert!(matches!(arch.backend, Backend::Resident(_)));
        assert_eq!(arch.read("graphics/a.png"), Some(b"flat-legacy".to_vec()));
        assert_eq!(arch.file_size("graphics/a.png"), Some(11));
        let _ = std::fs::remove_file(&out);
    }

    #[test]
    fn rejects_traversing_archive_entries() {
        let out = tmp("traversal.ay");
        let file = std::fs::File::create(&out).unwrap();
        let mut zip = zip::ZipWriter::new(file);
        let options = zip::write::SimpleFileOptions::default();
        for (path, bytes) in [
            ("manifest.toml", MANIFEST.as_bytes()),
            ("../outside.bin", b"hostile".as_slice()),
        ] {
            zip.start_file(path, options).unwrap();
            zip.write_all(bytes).unwrap();
        }
        zip.finish().unwrap();

        assert!(matches!(
            AyArchive::open_verbose(out.to_str().unwrap()),
            Err(AyError::PolicyViolation(_))
        ));
        let _ = std::fs::remove_file(out);
    }

    #[test]
    fn rejects_high_expansion_zip_entry_before_reading_it() {
        let out = tmp("compression_bomb.ay");
        let file = std::fs::File::create(&out).unwrap();
        let mut zip = zip::ZipWriter::new(file);
        let options = zip::write::SimpleFileOptions::default()
            .compression_method(zip::CompressionMethod::Deflated);
        zip.start_file("manifest.toml", options).unwrap();
        zip.write_all(MANIFEST.as_bytes()).unwrap();
        zip.start_file("assets/zeros.bin", options).unwrap();
        zip.write_all(&vec![0; 1024 * 1024]).unwrap();
        zip.finish().unwrap();

        assert!(matches!(
            AyArchive::open_verbose(out.to_str().unwrap()),
            Err(AyError::PolicyViolation(_))
        ));
        let _ = std::fs::remove_file(out);
    }
}
