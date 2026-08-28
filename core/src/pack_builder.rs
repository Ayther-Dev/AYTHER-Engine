//! In-process builder and signer for `.ay` packs.
//!
//! [`PackBuilder`] assembles entries, generates the integrity index, preserves
//! streamable entries, and optionally signs the canonical pack contents.

// ---------------------------------------------------------------------------
// pack_builder.rs — assemble + sign a `.ay` pack in-process.
//
// The `ay_pack` CLI builds packs from the command line; this module does the
// same job from inside the engine so the Ayther Lab's **Deliver** workspace can
// produce a signed pack with one click (R8). A `.ay` is a ZIP containing:
//
//   manifest.toml              — pack identity (name, version, game_id, regions)
//   sprite_substitutions.toml  — hash → asset map (and tiles/audio likewise)
//   <assets…>                  — the referenced HD PNG/WAV files
//   integrity.toml             — per-entry SHA-256 + size
//   signature.bin              — ED25519 over the integrity.toml bytes (on sign)
//
//  the signature covers integrity.toml — which enumerates and hashes
// every other entry — instead of a whole-content hash. That is what lets
// `AyArchive` open the pack lazily and verify each asset on read, without
// ever having the pack resident. The dev seed below is the RFC-8032 test
// vector and is intentionally NOT secret — Hub distribution re-signs with a
// privately-held key via `ay_pack sign`.
// ---------------------------------------------------------------------------

use std::collections::HashMap;
use std::io::Write;
use std::path::Path;

use ed25519_dalek::{Signer, SigningKey};
use sha2::{Digest, Sha256};
use zip::{CompressionMethod, ZipWriter, write::SimpleFileOptions};

use crate::archive_vfs::{INTEGRITY_PATH, SIGNATURE_PATH, is_stored_entry, stream_chunk_size};

// Dev signing seed — must match ay_pack's DEV_SIGNING_SEED and yield
// archive_vfs::DEV_PUBLIC_KEY. RFC 8032 test vector. Not secret.
const DEV_SIGNING_SEED: [u8; 32] = [
    0x4c, 0xcd, 0x08, 0x9b, 0x28, 0xff, 0x96, 0xda, 0x9d, 0xb6, 0xc3, 0x46, 0xec, 0x11, 0x4e, 0x0f,
    0x5b, 0x8a, 0x31, 0x9f, 0x35, 0xab, 0xa6, 0x24, 0xda, 0x8c, 0xf6, 0xed, 0x4d, 0x0b, 0xd6, 0xd9,
];

/// Builds a deterministic `.ay` pack from staged entries.
///
/// Assets are named by content hash rather than extension, so the caller may
/// explicitly mark entries that must remain uncompressed for ranged streaming.
/// Streamability is selected when an entry is staged, while its source extension
/// is still known, rather than inferred from the content-derived destination name.
#[derive(Default)]
pub struct PackBuilder {
    files: HashMap<String, Vec<u8>>,
    /// Entradas a guardar SIN comprimir (= direccionables por rango).
    stored: std::collections::HashSet<String>,
}

impl PackBuilder {
    /// Creates an empty pack builder.
    pub fn new() -> Self {
        Self::default()
    }

    /// Number of entries staged (excludes the not-yet-added signature).
    pub fn file_count(&self) -> usize {
        self.files.len()
    }

    /// Stage an entry from raw bytes. Paths are normalised to forward slashes;
    /// `signature.bin` and `integrity.toml` are rejected (produced by `finish`).
    /// Streamability is inferred from the normalized path. Use
    /// [`Self::add_bytes_stored`] to select it explicitly.
    pub fn add_bytes(&mut self, path: &str, data: Vec<u8>) -> bool {
        let stored = is_stored_entry(&path.replace('\\', "/"));
        self.add_bytes_stored(path, data, stored)
    }

    /// Stages an entry and explicitly selects whether it is stored uncompressed.
    ///
    /// A stored entry is addressable by range and receives per-chunk hashes in
    /// the integrity index.
    pub fn add_bytes_stored(&mut self, path: &str, data: Vec<u8>, stored: bool) -> bool {
        let p = path.replace('\\', "/");
        if p.is_empty() || p == SIGNATURE_PATH || p == INTEGRITY_PATH {
            return false;
        }
        if stored {
            self.stored.insert(p.clone());
        } else {
            self.stored.remove(&p);
        }
        self.files.insert(p, data);
        true
    }

    /// Stage an entry by reading a file from disk into the pack at `path_in_pack`.
    ///
    /// Streamability is inferred from the source path because the content-derived
    /// destination path may no longer retain a file extension.
    pub fn add_file(&mut self, path_in_pack: &str, source: &Path) -> std::io::Result<()> {
        let data = std::fs::read(source)?;
        let stored = is_stored_entry(&source.to_string_lossy().replace('\\', "/"));
        if !self.add_bytes_stored(path_in_pack, data, stored) {
            return Err(std::io::Error::new(
                std::io::ErrorKind::InvalidInput,
                "invalid pack entry path",
            ));
        }
        Ok(())
    }

    /// Write the pack to `out`. Always emits `integrity.toml` (per-entry
    /// SHA-256 + size, ) so the engine can open the pack lazily; when
    /// `sign` is set, appends an ED25519 `signature.bin` over the
    /// integrity.toml bytes. Requires `manifest.toml`.
    /// Entries are written in sorted order for a deterministic archive.
    pub fn finish(&self, sign: bool, out: &Path) -> Result<(), String> {
        if !self.files.contains_key("manifest.toml") {
            return Err("pack has no manifest.toml".into());
        }

        let integrity = Self::integrity_toml_with(&self.files, &self.stored);
        let sig: Option<Vec<u8>> = if sign {
            let sk = SigningKey::from_bytes(&DEV_SIGNING_SEED);
            Some(sk.sign(integrity.as_bytes()).to_bytes().to_vec())
        } else {
            None
        };

        let file = std::fs::File::create(out)
            .map_err(|e| format!("creating '{}': {}", out.display(), e))?;
        let mut zip = ZipWriter::new(file);
        let opts = SimpleFileOptions::default().compression_method(CompressionMethod::Deflated);
        // Un video VP9 ya está comprimido: deflatearlo no ahorra nada y se paga
        // el inflate en cada apertura del pack — que el hotreload hace en cada
        // guardado del artista.
        //
        //  la elección ya NO se deriva del nombre acá. Viene del conjunto
        // `stored`, poblado al agregar la entrada, cuando el llamador todavía
        // conocía el origen. Ver la nota del struct: con nombres por hash,
        // derivarla del nombre apagaba el streaming en silencio.
        let stored = SimpleFileOptions::default().compression_method(CompressionMethod::Stored);
        // El MISMO conjunto decide qué entradas llevan hashes por trozo en
        // integrity.toml. Que sea uno solo es lo que impide el estado imposible
        // «con trozos pero deflateada».
        let method_for = |p: &str| {
            if self.stored.contains(p) {
                stored
            } else {
                opts
            }
        };

        let mut paths: Vec<&String> = self.files.keys().collect();
        paths.sort();
        for p in paths {
            zip.start_file(p, method_for(p))
                .map_err(|e| e.to_string())?;
            zip.write_all(&self.files[p]).map_err(|e| e.to_string())?;
        }
        zip.start_file(INTEGRITY_PATH, opts)
            .map_err(|e| e.to_string())?;
        zip.write_all(integrity.as_bytes())
            .map_err(|e| e.to_string())?;
        if let Some(s) = sig {
            zip.start_file(SIGNATURE_PATH, opts)
                .map_err(|e| e.to_string())?;
            zip.write_all(&s).map_err(|e| e.to_string())?;
        }
        zip.finish().map_err(|e| e.to_string())?;
        Ok(())
    }

    ///  serialise the per-entry index that the signature covers. Sorted
    /// by path so the same staged content always yields the same bytes (the
    /// signature is over these bytes — determinism is not cosmetic here).
    ///
    ///  las entradas `Stored` llevan además `chunk` + `chunks`, que es lo
    /// que permite verificar una LECTURA POR RANGO sin materializar la entrada.
    /// Estos bytes tienen que salir idénticos a los de `integrity_toml` en
    /// tools/ay_pack (`ay_pack sign` regenera el índice): un test a cada lado
    /// fija el mismo vector para que la deriva se note al compilar y no al
    /// abrir un pack firmado.
    ///
    ///  `stored` es el conjunto de entradas sin comprimir, decidido al
    /// AGREGAR. Antes esto llamaba a `is_stored_entry(p)` sobre el nombre de la
    /// entrada; con nombres por hash eso dejaba de reconocer los videos.
    fn integrity_toml_with(
        files: &HashMap<String, Vec<u8>>,
        stored: &std::collections::HashSet<String>,
    ) -> String {
        let mut paths: Vec<&String> = files.keys().collect();
        paths.sort();
        let mut s = String::from(
            "# integrity.toml — per-entry hashes; signature.bin signs these exact bytes\n\
             version = 1\n",
        );
        for p in paths {
            let data = &files[p];
            let hash = Sha256::digest(data);
            let escaped = p.replace('\\', "\\\\").replace('"', "\\\"");
            s.push_str(&format!(
                "\n[[entry]]\npath   = \"{}\"\nsha256 = \"{:x}\"\nsize   = {}\n",
                escaped,
                hash,
                data.len()
            ));
            if stored.contains(p) && !data.is_empty() {
                let chunk = stream_chunk_size(data.len() as u64);
                s.push_str(&format!("chunk  = {}\nchunks = [\n", chunk));
                for c in data.chunks(chunk as usize) {
                    s.push_str(&format!("  \"{:x}\",\n", Sha256::digest(c)));
                }
                s.push_str("]\n");
            }
        }
        s
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(name: &str) -> std::path::PathBuf {
        let mut p = std::env::temp_dir();
        p.push(format!("ay_packbuild_{}_{}", std::process::id(), name));
        p
    }

    const MANIFEST: &str = "[pack]\nname = \"Test\"\nversion = \"1.0.0\"\n\
        game_id = \"sonic2\"\nayther_min = \"0.8.0\"\n\
        [regions]\ndefault = \"NTSC\"\nsupported = [\"NTSC\"]\n";

    #[test]
    fn signed_pack_round_trips_through_archive() {
        let mut b = PackBuilder::new();
        assert!(b.add_bytes("manifest.toml", MANIFEST.as_bytes().to_vec()));
        assert!(b.add_bytes(
            "sprite_substitutions.toml",
            b"[[sub]]\nhash = \"0x00000000000000ab\"\nasset = \"hero.png\"\n".to_vec()
        ));
        assert!(b.add_bytes("hero.png", vec![0x89, b'P', b'N', b'G', 1, 2, 3, 4]));

        let out = tmp("signed.ay");
        b.finish(true, &out)
            .expect("finish should write a signed pack");

        // Opening verifies the signature (a bad sig would Err here).
        let arch = crate::archive_vfs::AyArchive::open_verbose(out.to_str().unwrap())
            .expect("signed pack must open + verify");
        assert_eq!(
            arch.read("hero.png"),
            Some(vec![0x89, b'P', b'N', b'G', 1, 2, 3, 4])
        );
        assert_eq!(arch.meta.game_id, "sonic2");
        let _ = std::fs::remove_file(&out);
    }

    ///  vector FIJO del formato de integrity.toml, con la misma entrada
    /// que `integrity_format_matches_core` en tools/ay_pack. Los dos tests
    /// afirman el mismo hash: si un emisor cambia y el otro no, falla el lado
    /// que cambió y la deriva se ve al compilar, no al abrir un pack firmado.
    ///
    /// El `.ivf` mide 70000 bytes A PROPÓSITO: pasa los 64 KiB del trozo
    /// mínimo, así el vector fija también el caso de VARIOS trozos y el del
    /// último trozo corto.
    pub(crate) fn drift_fixture() -> Vec<(&'static str, Vec<u8>)> {
        vec![
            ("manifest.toml", b"x".to_vec()),
            ("graphics/a.png", b"png".to_vec()),
            (
                "video/clip.ivf",
                (0..70000u32).map(|i| (i % 251) as u8).collect(),
            ),
            ("video/vacio.ivf", Vec::new()),
        ]
    }
    pub(crate) const DRIFT_SHA: &str =
        "d543cc2fc93ae47573026d86ce8b06d548de2cad2deb7cf868fa3ff0fc858e19";

    ///  UNA ENTRADA SIN EXTENSION PUEDE SER STREAMEABLE.
    ///
    /// Es la propiedad que el contrato necesita y que antes no existia: el pack
    /// va a nombrar los assets por hash de su contenido, sin extension. Si la
    /// decision de guardar-sin-comprimir siguiera saliendo del nombre, todo
    /// video entraria deflateado y el streaming de se apagaria SIN UN SOLO
    /// ERROR — el lector keyea por la presencia de trozos, asi que un pack sin
    /// trozos es indistinguible de uno que no se puede streamear.
    ///
    /// Se prueban los tres caminos, porque cada uno decide distinto:
    ///   · add_bytes_stored — declarado por el llamador
    ///   · add_bytes        — inferido del NOMBRE (compatibilidad)
    ///   · add_file         — inferido del ORIGEN, que es el unico que conserva
    ///                        la extension cuando el destino ya es un hash
    #[test]
    fn extensionless_entry_can_be_streamable() {
        let data: Vec<u8> = (0..70000u32).map(|i| (i % 251) as u8).collect();
        let hash_name = "assets/a3f9c1d2e5b8"; // Content-derived destination name.

        // 1. Declarado: sin extension y CON trozos.
        let mut b = PackBuilder::new();
        b.add_bytes("manifest.toml", MANIFEST.as_bytes().to_vec());
        b.add_bytes_stored(hash_name, data.clone(), true);
        let t = PackBuilder::integrity_toml_with(&b.files, &b.stored);
        assert!(
            t.contains("chunk  = 65536"),
            "declarado stored: la entrada sin extension lleva trozos"
        );

        // 2. Inferido del nombre: sin extension, SIN trozos. No es un defecto —
        //    es exactamente lo que hacia el codigo viejo, y por eso hacia falta
        //    poder declararlo.
        let mut b2 = PackBuilder::new();
        b2.add_bytes("manifest.toml", MANIFEST.as_bytes().to_vec());
        b2.add_bytes(hash_name, data.clone());
        let t2 = PackBuilder::integrity_toml_with(&b2.files, &b2.stored);
        assert!(
            !t2.contains("chunk  ="),
            "inferido del nombre: sin extension no hay trozos"
        );

        // 3. Inferido del ORIGEN: destino sin extension, origen.ivf → CON
        //    trozos. Este es el camino que usa el bake del Lab.
        let mut src = std::env::temp_dir();
        src.push(format!("ae_pb_{}_clip.ivf", std::process::id()));
        std::fs::write(&src, &data).unwrap();
        let mut b3 = PackBuilder::new();
        b3.add_bytes("manifest.toml", MANIFEST.as_bytes().to_vec());
        b3.add_file(hash_name, &src).unwrap();
        let t3 = PackBuilder::integrity_toml_with(&b3.files, &b3.stored);
        assert!(
            t3.contains("chunk  = 65536"),
            "add_file: la streamabilidad sale del ORIGEN, no del destino"
        );
        let _ = std::fs::remove_file(src);
    }

    #[test]
    fn integrity_format_is_pinned() {
        let files: HashMap<String, Vec<u8>> = drift_fixture()
            .into_iter()
            .map(|(p, d)| (p.to_string(), d))
            .collect();
        // El conjunto `stored` se deriva igual que antes: lo que este vector fija
        // es el FORMATO, y el formato no cambio con — cambio de donde sale
        // la decision. Si el SHA sigue dando, la refactorizacion fue neutra.
        let stored: std::collections::HashSet<String> = files
            .keys()
            .filter(|p| is_stored_entry(p))
            .cloned()
            .collect();
        let toml = PackBuilder::integrity_toml_with(&files, &stored);
        assert_eq!(format!("{:x}", Sha256::digest(toml.as_bytes())), DRIFT_SHA);
    }

    #[test]
    fn stored_entries_carry_chunk_hashes() {
        let files: HashMap<String, Vec<u8>> = drift_fixture()
            .into_iter()
            .map(|(p, d)| (p.to_string(), d))
            .collect();
        // El conjunto `stored` se deriva igual que antes: lo que este vector fija
        // es el FORMATO, y el formato no cambio con — cambio de donde sale
        // la decision. Si el SHA sigue dando, la refactorizacion fue neutra.
        let stored: std::collections::HashSet<String> = files
            .keys()
            .filter(|p| is_stored_entry(p))
            .cloned()
            .collect();
        let toml = PackBuilder::integrity_toml_with(&files, &stored);
        // 70000 bytes con trozo de 64 KiB = 2 trozos (el segundo, corto).
        assert!(
            toml.contains("chunk  = 65536"),
            "el .ivf debe declarar trozo"
        );
        assert_eq!(
            toml.matches("chunk  =").count(),
            1,
            "sólo las entradas Stored NO vacías llevan trozos"
        );
        let chunks_block = toml.split("chunks = [").nth(1).unwrap();
        assert_eq!(
            chunks_block.split(']').next().unwrap().matches('"').count(),
            4,
            "2 hashes de trozo"
        );
        // El.png es Deflate: sin trozos, y por eso read_range lo rechaza.
        assert!(
            !toml
                .split("graphics/a.png")
                .nth(1)
                .unwrap()
                .split("[[entry]]")
                .next()
                .unwrap()
                .contains("chunk")
        );
    }

    #[test]
    fn chunk_size_grows_to_keep_the_list_bounded() {
        use crate::archive_vfs::{STREAM_CHUNK_MAX_COUNT, stream_chunk_size};
        assert_eq!(stream_chunk_size(1), 65536);
        assert_eq!(stream_chunk_size(64 * 1024 * 2048), 65536); // justo el techo
        assert_eq!(stream_chunk_size(64 * 1024 * 2048 + 1), 131072); // un byte más: duplica
        // Un video 8K de 1 GB: la lista sigue acotada.
        assert!(
            1_073_741_824u64.div_ceil(stream_chunk_size(1_073_741_824)) <= STREAM_CHUNK_MAX_COUNT
        );
    }

    #[test]
    fn finish_without_manifest_errors() {
        let mut b = PackBuilder::new();
        b.add_bytes("sprite_substitutions.toml", b"".to_vec());
        let out = tmp("nomani.ay");
        assert!(b.finish(true, &out).is_err());
    }

    #[test]
    fn rejects_signature_entry_and_normalises_paths() {
        let mut b = PackBuilder::new();
        assert!(!b.add_bytes("signature.bin", vec![1, 2, 3]));
        assert!(!b.add_bytes("integrity.toml", vec![4, 5, 6]));
        assert!(b.add_bytes("hd\\sprites\\x.png", vec![9]));
        assert_eq!(b.file_count(), 1);
    }
}
