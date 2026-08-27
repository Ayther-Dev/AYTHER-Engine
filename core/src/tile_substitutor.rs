//! Resolution of tile identities to authored replacement assets.
//!
//! Runtime overrides take precedence over pack catalog entries, and optional
//! declarative conditions are evaluated in declaration order.

// ---------------------------------------------------------------------------
// TileSubstitutor — maps tile hashes to HD asset paths.
//
// Data sources (in priority order, highest first):
//   1. Runtime overrides — registered by Lua via `ayther.tiles.replace()`.
//   2. Pack catalog     — loaded from `tile_substitutions.toml` in the.ay pack.
//
// ## tile_substitutions.toml format
//
//   [[sub]]
//   hash  = "0xdeadbeefcafebabe"
//   asset = "hd/tiles/ground.png"
//
//   [[sub]]
//   hash  = "0x0123456789abcdef"
//   asset = "hd/tiles/sky.png"
//
// Hashes are xxHash3-64 values produced by TileHasher, formatted as
// "0x" + 16 hex digits (matching the output of `ayther_tile_hasher_dump_toml`).
//
// ## Condiciones (EM-2 )
//
// Un `[[sub]]` puede llevar condiciones; sólo aplica si TODAS dan verdadero.
// Varias entradas pueden compartir el MISMO hash: gana la primera que matchea
// (el orden de declaración es significativo), y una entrada sin condiciones
// actúa de default.
//
//   [[sub]]
//   hash  = "0x1a2b..."
//   asset = "hd/tiles/columna_noche.png"
//   [[sub.condition]]
//   kind  = "memory_const"
//   addr  = "0xFF7E26"
//   op    = ">="
//   value = 4
//
//   [[sub]]
//   hash  = "0x1a2b..."                  # default: sin condiciones
//   asset = "hd/tiles/columna.png"
//
// Las condiciones de este subconjunto son de FRAME (memoria / número de
// frame), no de posición: se evalúan UNA vez por frame en `begin_frame` y el
// lookup por tile sigue siendo O(1). Cuando entren las espaciales (`*Nearby`),
// esto pasa a ser la primera pasada y las de posición se evalúan por
// occurrence.
//
// ## Lifecycle
//
//   let mut sub = TileSubstitutor::new();
//   sub.load_from_pack(&pack);          // once on pack load
//
//   // each frame:
//   sub.begin_frame(n, ram, swapped);   // evalúa las condiciones del frame
//   sub.clear_overrides();              // discard last frame's Lua overrides
//   sub.add_override(hash, path);       // for each entry from ayther._tile_overrides
//   let subs = sub.resolve(occurrences); // → Vec<TileSub>
//
// Sin `begin_frame` (cores/tests que no pasan RAM) sólo aplican las entradas
// SIN condiciones — una condicional nunca dispara "por las dudas".
// ---------------------------------------------------------------------------

use crate::conditions::{Condition, FrameCtx, RamView, build_conditions, eval_all};
use crate::sprite_hasher::TileOccurrence;
use std::collections::HashMap;

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

/// One resolved HD substitution: replace the tile at screen position
/// (tile_x × TILE_W, tile_y × TILE_H) with the given pack asset.
#[derive(Clone, Debug)]
pub struct TileSub {
    /// Logical asset path inside the `.ay` pack.
    pub asset_path: String, // logical path inside the.ay pack (e.g. "hd/tiles/ground.png")
    /// Column in the native tile grid.
    pub tile_x: u32, // tile-grid column
    /// Row in the native tile grid.
    pub tile_y: u32, // tile-grid row
}

// ---------------------------------------------------------------------------
// TileSubstitutor
// ---------------------------------------------------------------------------

/// Una entrada del catálogo: el asset y las condiciones que lo habilitan
/// (vacías = siempre). Varias entradas pueden compartir hash.
#[derive(Clone, Debug)]
struct CatalogEntry {
    asset: String,
    conds: Vec<Condition>,
}

/// Resolves observed tile hashes to authored HD assets.
pub struct TileSubstitutor {
    /// Loaded from `tile_substitutions.toml` in the.ay pack. Por hash, en
    /// orden de declaración: primer match gana.
    catalog: HashMap<u64, Vec<CatalogEntry>>,
    /// Ganador vigente por hash. Se recalcula en `begin_frame` cuando hay
    /// condiciones; si no las hay, es el catálogo plano de siempre.
    resolved: HashMap<u64, String>,
    /// ¿Algún `[[sub]]` trae condiciones? Si no, `begin_frame` es un no-op y
    /// el costo por frame es cero (el 100% de los packs de hoy).
    conditional: bool,
    /// Registered by Lua scripts via `ayther.tiles.replace()`.
    /// Cleared at the start of each frame before re-running callbacks.
    overrides: HashMap<u64, String>,
}

impl TileSubstitutor {
    /// Creates an empty substitution catalog.
    pub fn new() -> Self {
        Self {
            catalog: HashMap::new(),
            resolved: HashMap::new(),
            conditional: false,
            overrides: HashMap::new(),
        }
    }

    // -----------------------------------------------------------------------
    // Pack catalog
    // -----------------------------------------------------------------------

    /// Load the substitution catalog from a `.ay` pack.
    ///
    /// Reads `tile_substitutions.toml` if present; silently ignores missing
    /// file or parse errors.
    pub fn load_from_pack(&mut self, pack: &crate::archive_vfs::AyArchive) {
        self.load_from_pack_named(pack, "tile_substitutions.toml");
    }

    /// Loads `[[sub]]` hash/asset entries from a named TOML file in the pack.
    ///
    /// Missing files and parse errors are ignored.
    pub fn load_from_pack_named(&mut self, pack: &crate::archive_vfs::AyArchive, file: &str) {
        self.catalog.clear();
        self.resolved.clear();
        self.conditional = false;
        if let Some(data) = pack.read(file)
            && let Ok(s) = std::str::from_utf8(&data)
        {
            self.parse_toml(s);
        }
    }

    /// Returns the number of distinct hashes with at least one catalog entry.
    pub fn catalog_len(&self) -> usize {
        self.catalog.len()
    }

    /// Returns the total number of loaded entries, including conditional variants.
    pub fn entry_len(&self) -> usize {
        self.catalog.values().map(Vec::len).sum()
    }

    /// Returns whether any catalog entry has runtime conditions.
    pub fn is_conditional(&self) -> bool {
        self.conditional
    }

    // -----------------------------------------------------------------------
    // Condiciones (EM-2)
    // -----------------------------------------------------------------------

    /// Evaluates catalog conditions and selects one winner per hash for a frame.
    ///
    /// `ram` is the raw work-RAM buffer. Set `word_swapped` when logical address
    /// `a` is stored at `ram[a ^ 1]`.
    pub fn begin_frame(&mut self, frame_number: u64, ram: &[u8], word_swapped: bool) {
        if !self.conditional {
            return;
        } // fast path: packs sin condiciones
        let view = if word_swapped {
            RamView::word_swapped(ram)
        } else {
            RamView::linear(ram)
        };
        let ctx = FrameCtx::new(frame_number, view);
        self.rebuild_resolved(Some(&ctx));
    }

    /// Restores context-free resolution in which only unconditional entries win.
    pub fn end_frame(&mut self) {
        if self.conditional {
            self.rebuild_resolved(None);
        }
    }

    fn rebuild_resolved(&mut self, ctx: Option<&FrameCtx>) {
        self.resolved.clear();
        for (hash, entries) in &self.catalog {
            // Primer match gana; sin contexto, sólo las incondicionales pueden
            // ganar (una condicional jamás dispara a ciegas).
            let win = entries.iter().find(|e| match ctx {
                Some(c) => eval_all(&e.conds, c),
                None => e.conds.is_empty(),
            });
            if let Some(e) = win {
                self.resolved.insert(*hash, e.asset.clone());
            }
        }
    }

    /// Looks up the current asset for a hash, preferring runtime overrides.
    pub fn lookup(&self, hash: u64) -> Option<&str> {
        self.overrides
            .get(&hash)
            .or_else(|| self.resolved.get(&hash))
            .map(|s| s.as_str())
    }

    // -----------------------------------------------------------------------
    // Runtime overrides (Lua-driven)
    // -----------------------------------------------------------------------

    /// Register a runtime override.  Takes priority over the catalog.
    pub fn add_override(&mut self, hash: u64, asset_path: String) {
        self.overrides.insert(hash, asset_path);
    }

    /// Clear all Lua-registered overrides.  Call at the start of each frame.
    pub fn clear_overrides(&mut self) {
        self.overrides.clear();
    }

    /// Returns the number of active runtime overrides.
    pub fn override_len(&self) -> usize {
        self.overrides.len()
    }

    // -----------------------------------------------------------------------
    // Resolution
    // -----------------------------------------------------------------------

    /// Resolve a frame's tile occurrences to substitution instructions.
    ///
    /// Returns only the occurrences that have a matching entry (override > catalog).
    /// The same hash may appear multiple times if the same tile is visible at
    /// multiple positions — each yields a separate `TileSub`.
    pub fn resolve(&self, occurrences: &[TileOccurrence]) -> Vec<TileSub> {
        let mut result = Vec::new();
        for occ in occurrences {
            let asset = self
                .overrides
                .get(&occ.hash)
                .or_else(|| self.resolved.get(&occ.hash));
            if let Some(path) = asset {
                result.push(TileSub {
                    asset_path: path.clone(),
                    tile_x: occ.tile_x,
                    tile_y: occ.tile_y,
                });
            }
        }
        result
    }

    // -----------------------------------------------------------------------
    // TOML parsing
    // -----------------------------------------------------------------------

    fn parse_toml(&mut self, s: &str) {
        #[derive(serde::Deserialize)]
        struct SubEntry {
            hash: String,
            asset: String,
            /// EM-2: `[[sub.condition]]` — ausente ⇒ match global (formato legacy).
            #[serde(default)]
            condition: Vec<crate::conditions::CondSpec>,
        }
        #[derive(serde::Deserialize)]
        struct SubFile {
            #[serde(rename = "sub")]
            subs: Option<Vec<SubEntry>>,
        }

        match toml::from_str::<SubFile>(s) {
            Ok(file) => {
                for entry in file.subs.unwrap_or_default() {
                    let hash = match parse_hash_hex(&entry.hash) {
                        Ok(h) => h,
                        Err(_) => {
                            eprintln!("[TileSubstitutor] bad hash: {}", entry.hash);
                            continue;
                        }
                    };
                    // Una condición que no compila DESCARTA la entrada: aplicarla
                    // igual la volvería global, que es justo lo contrario de lo
                    // que el autor pidió.
                    let conds = match build_conditions(&entry.condition) {
                        Ok(c) => c,
                        Err(e) => {
                            eprintln!(
                                "[TileSubstitutor] {} ({}): {} — entrada ignorada",
                                entry.hash, entry.asset, e
                            );
                            continue;
                        }
                    };
                    if !conds.is_empty() {
                        self.conditional = true;
                    }
                    let list = self.catalog.entry(hash).or_default();
                    // Compatibilidad exacta con el mapa plano de antes: dos
                    // `[[sub]]` SIN condiciones para el mismo hash eran un
                    // `HashMap::insert` — ganaba el ÚLTIMO. Se conserva
                    // reemplazando en su lugar; las condicionales sí se apilan
                    // y entre ellas gana la PRIMERA que matchea.
                    match list.iter_mut().find(|e| e.conds.is_empty()) {
                        Some(slot) if conds.is_empty() => slot.asset = entry.asset,
                        _ => list.push(CatalogEntry {
                            asset: entry.asset,
                            conds,
                        }),
                    }
                }
            }
            Err(e) => eprintln!("[TileSubstitutor] toml parse error: {}", e),
        }
        self.rebuild_resolved(None);
    }
}

impl Default for TileSubstitutor {
    fn default() -> Self {
        Self::new()
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Parse "0x0123456789abcdef" (or without "0x") → u64.
fn parse_hash_hex(s: &str) -> Result<u64, std::num::ParseIntError> {
    let trimmed = s.trim();
    let hex = trimmed
        .strip_prefix("0x")
        .or_else(|| trimmed.strip_prefix("0X"))
        .unwrap_or(trimmed);
    u64::from_str_radix(hex, 16)
}

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn occ(hash: u64, tx: u32, ty: u32) -> TileOccurrence {
        TileOccurrence {
            hash,
            tile_x: tx,
            tile_y: ty,
        }
    }

    #[test]
    fn empty_catalog_returns_nothing() {
        let sub = TileSubstitutor::new();
        assert!(sub.resolve(&[occ(0xdeadbeef, 0, 0)]).is_empty());
    }

    #[test]
    fn override_resolves() {
        let mut sub = TileSubstitutor::new();
        sub.add_override(0xdeadbeef, "hd/tile.png".into());
        let result = sub.resolve(&[occ(0xdeadbeef, 3, 7)]);
        assert_eq!(result.len(), 1);
        assert_eq!(result[0].asset_path, "hd/tile.png");
        assert_eq!(result[0].tile_x, 3);
        assert_eq!(result[0].tile_y, 7);
    }

    #[test]
    fn override_beats_catalog() {
        let mut sub = TileSubstitutor::new();
        // Inject directly into catalog to bypass pack loading
        sub.catalog.insert(
            0x1234,
            vec![CatalogEntry {
                asset: "catalog.png".into(),
                conds: vec![],
            }],
        );
        sub.rebuild_resolved(None);
        sub.add_override(0x1234, "override.png".into());
        let result = sub.resolve(&[occ(0x1234, 0, 0)]);
        assert_eq!(result[0].asset_path, "override.png");
    }

    #[test]
    fn same_hash_multiple_positions() {
        let mut sub = TileSubstitutor::new();
        sub.add_override(0xaaaa, "hd/t.png".into());
        let occs = vec![occ(0xaaaa, 0, 0), occ(0xaaaa, 5, 2), occ(0xaaaa, 10, 4)];
        let result = sub.resolve(&occs);
        assert_eq!(result.len(), 3);
    }

    #[test]
    fn parse_toml_catalog() {
        let mut sub = TileSubstitutor::new();
        let toml_src = r#"
[[sub]]
hash  = "0xdeadbeefcafebabe"
asset = "hd/ground.png"

[[sub]]
hash  = "0x0123456789abcdef"
asset = "hd/sky.png"
"#;
        sub.parse_toml(toml_src);
        assert_eq!(sub.catalog_len(), 2);

        let result = sub.resolve(&[occ(0xdeadbeefcafebabe, 0, 0), occ(0x0123456789abcdef, 1, 0)]);
        assert_eq!(result.len(), 2);
        assert!(result.iter().any(|s| s.asset_path == "hd/ground.png"));
        assert!(result.iter().any(|s| s.asset_path == "hd/sky.png"));
    }

    #[test]
    fn parse_toml_round_trips_deliver_format() {
        // Exactly what the lab DeliverPanel writes for tile_substitutions.toml:
        // [[sub]] with hash = "0x%016llx" and asset by basename.
        let mut sub = TileSubstitutor::new();
        sub.parse_toml(
            "[[sub]]\nhash = \"0x00000000deadbeef\"\nasset = \"ground.png\"\n\n\
             [[sub]]\nhash = \"0x0123456789abcdef\"\nasset = \"sky.png\"\n\n",
        );
        assert_eq!(sub.catalog_len(), 2);
        let r = sub.resolve(&[occ(0x00000000deadbeef, 2, 3)]);
        assert_eq!(r.len(), 1);
        assert_eq!(r[0].asset_path, "ground.png");
    }

    #[test]
    fn clear_overrides() {
        let mut sub = TileSubstitutor::new();
        sub.add_override(0x1111, "a.png".into());
        assert_eq!(sub.override_len(), 1);
        sub.clear_overrides();
        assert_eq!(sub.override_len(), 0);
        assert!(sub.resolve(&[occ(0x1111, 0, 0)]).is_empty());
    }

    // -- Condiciones (EM-2 ) -------------------------------------------

    /// El AC del diseño: MISMO hash, dos assets, desambiguados por el estado
    /// de la RAM. La entrada sin condiciones actúa de default.
    #[test]
    fn same_hash_two_assets_disambiguated_by_memory() {
        let mut sub = TileSubstitutor::new();
        sub.parse_toml(
            r#"
[[sub]]
hash  = "0xaaaa"
asset = "columna_noche.png"
[[sub.condition]]
kind  = "memory_const"
addr  = "0x0002"
op    = ">="
value = 4

[[sub]]
hash  = "0xaaaa"
asset = "columna_dia.png"
"#,
        );
        assert!(sub.is_conditional());
        assert_eq!(sub.catalog_len(), 1); // un hash…
        assert_eq!(sub.entry_len(), 2); // …con dos variantes

        // Sin begin_frame ⇒ sólo la incondicional (nunca dispara a ciegas).
        assert_eq!(sub.lookup(0xaaaa), Some("columna_dia.png"));

        // ram[2] = 7 ≥ 4 ⇒ gana la de noche (viene primera).
        sub.begin_frame(0, &[0, 0, 7, 0], false);
        assert_eq!(sub.lookup(0xaaaa), Some("columna_noche.png"));

        // ram[2] = 1 < 4 ⇒ cae al default.
        sub.begin_frame(1, &[0, 0, 1, 0], false);
        assert_eq!(sub.lookup(0xaaaa), Some("columna_dia.png"));

        // El override de Lua sigue ganándole a todo.
        sub.add_override(0xaaaa, "lua.png".into());
        assert_eq!(sub.lookup(0xaaaa), Some("lua.png"));
    }

    #[test]
    fn conditional_entry_without_fallback_yields_nothing_when_unmet() {
        let mut sub = TileSubstitutor::new();
        sub.parse_toml(
            "[[sub]]\nhash=\"0xbbbb\"\nasset=\"solo.png\"\n\
                        [[sub.condition]]\nkind=\"memory_const\"\naddr=0\nop=\"=\"\nvalue=9\n",
        );
        assert_eq!(sub.lookup(0xbbbb), None); // sin frame
        sub.begin_frame(0, &[1], false);
        assert_eq!(sub.lookup(0xbbbb), None); // condición falsa
        sub.begin_frame(1, &[9], false);
        assert_eq!(sub.lookup(0xbbbb), Some("solo.png"));
        sub.end_frame();
        assert_eq!(sub.lookup(0xbbbb), None); // vuelve a "sin contexto"
    }

    #[test]
    fn declaration_order_decides_the_winner() {
        // Dos condicionales que matchean a la vez: gana la PRIMERA declarada.
        let toml_src = |first: &str, second: &str| {
            format!(
                "[[sub]]\nhash=\"0xcccc\"\nasset=\"{}\"\n\
             [[sub.condition]]\nkind=\"frame_range\"\ndivisor=1\nat_least=0\n\n\
             [[sub]]\nhash=\"0xcccc\"\nasset=\"{}\"\n\
             [[sub.condition]]\nkind=\"frame_range\"\ndivisor=1\nat_least=0\n",
                first, second
            )
        };

        let mut a = TileSubstitutor::new();
        a.parse_toml(&toml_src("uno.png", "dos.png"));
        a.begin_frame(0, &[], false);
        assert_eq!(a.lookup(0xcccc), Some("uno.png"));

        let mut b = TileSubstitutor::new();
        b.parse_toml(&toml_src("dos.png", "uno.png"));
        b.begin_frame(0, &[], false);
        assert_eq!(b.lookup(0xcccc), Some("dos.png"));
    }

    #[test]
    fn legacy_pack_is_bit_for_bit_the_old_behaviour() {
        let mut sub = TileSubstitutor::new();
        sub.parse_toml(
            "[[sub]]\nhash=\"0x1\"\nasset=\"a.png\"\n\n\
                        [[sub]]\nhash=\"0x2\"\nasset=\"b.png\"\n",
        );
        assert!(!sub.is_conditional());
        assert_eq!(sub.lookup(0x1), Some("a.png")); // sin begin_frame
        assert_eq!(sub.resolve(&[occ(0x2, 4, 5)])[0].asset_path, "b.png");
        // begin_frame es no-op cuando no hay condiciones.
        sub.begin_frame(99, &[], true);
        assert_eq!(sub.lookup(0x1), Some("a.png"));
    }

    #[test]
    fn broken_condition_drops_the_entry_instead_of_going_global() {
        let mut sub = TileSubstitutor::new();
        sub.parse_toml(
            "[[sub]]\nhash=\"0xdddd\"\nasset=\"malo.png\"\n\
                        [[sub.condition]]\nkind=\"memory_const\"\naddr=0\nvalue=1\n\n\
                        [[sub]]\nhash=\"0xdddd\"\nasset=\"bueno.png\"\n",
        );
        assert_eq!(sub.entry_len(), 1);
        sub.begin_frame(0, &[1], false);
        assert_eq!(sub.lookup(0xdddd), Some("bueno.png"));
    }

    /// Regresión: antes el catálogo era un HashMap plano y un hash duplicado
    /// sin condiciones ganaba el ÚLTIMO. Eso NO cambia.
    #[test]
    fn duplicate_unconditional_entries_keep_last_wins() {
        let mut sub = TileSubstitutor::new();
        sub.parse_toml(
            "[[sub]]\nhash=\"0x7\"\nasset=\"primero.png\"\n\n\
                        [[sub]]\nhash=\"0x7\"\nasset=\"ultimo.png\"\n",
        );
        assert_eq!(sub.entry_len(), 1);
        assert_eq!(sub.lookup(0x7), Some("ultimo.png"));
    }

    #[test]
    fn word_swapped_ram_uses_68k_addresses() {
        let mut sub = TileSubstitutor::new();
        sub.parse_toml("[[sub]]\nhash=\"0xeeee\"\nasset=\"hd.png\"\n\
                        [[sub.condition]]\nkind=\"memory_const\"\naddr=0\nop=\"=\"\nvalue=\"0x11\"\n");
        // Buffer libretro LE: el byte de la dirección 0 vive en el índice 1.
        sub.begin_frame(0, &[0x22, 0x11], true);
        assert_eq!(sub.lookup(0xeeee), Some("hd.png"));
        sub.begin_frame(1, &[0x22, 0x11], false); // vista lineal ⇒ no matchea
        assert_eq!(sub.lookup(0xeeee), None);
    }

    #[test]
    fn parse_hash_without_prefix() {
        assert_eq!(parse_hash_hex("deadbeef").unwrap(), 0xdeadbeef);
        assert_eq!(parse_hash_hex("0xdeadbeef").unwrap(), 0xdeadbeef);
        assert_eq!(parse_hash_hex("0Xdeadbeef").unwrap(), 0xdeadbeef);
    }
}
