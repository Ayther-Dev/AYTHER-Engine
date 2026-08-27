//! Declarative mapping from game RAM to anchored entity positions.
//!
//! A [`GameProfile`] parses game-specific RAM layouts and produces the entity
//! observations consumed by [`crate::ram_anchor`].

// ---------------------------------------------------------------------------
// game_profile.rs — Mode 3 input plumbing: where entities live in game RAM.
//
// `ram_anchor::assign_sprites` is the pure geometric core (world+scroll → which
// sprites belong to which entity). It needs, per frame, the entities' *world
// positions*. Those come from game RAM at game-specific addresses — so we need a
// small, declarative **game profile** that says, for each kind of entity, where
// to read its world X/Y and what match box it uses. This module is that profile
// (TOML-backed) plus the structured RAM read that turns it into the
// `AnchoredEntity` list `assign_sprites` consumes.
//
// It is the layer between "raw RAM bytes" (host) and the pure matcher: it reads
// RAM (passed in as a slice) but no VDP — the camera scroll is fed in — so it is
// deterministic and unit-testable, and the same code serves the live engine and
// offline `.arp` replay.
//
// ## Format (see tools/modo3_spike/sonic2.toml)
//
//     word_swap = true          # Genesis: 68k words are byte-swapped in the
//                               # retro work-RAM buffer on little-endian hosts
//     [[anchor]]
//     name   = "player"
//     base   = 0xB000           # object struct address → entity id
//     x_off  = 0x08             # world-X word, relative to base
//     y_off  = 0x0C             # world-Y word
//     count  = 2                # array of identical instances (default 1)
//     stride = 0x40             # bytes between instances (id = base + i*stride)
//     pivot  = [0, 0]           # object origin → bbox anchor
//     half   = [40, 40]         # match tolerance (half-extents, px)
//     min_sprites = 2           #  descartar instancias con menos matches
//
//     [gate]                    #  validez GLOBAL del perfil (opcional)
//     addr   = 0xF600           # byte de RAM (p.ej. el game mode de Sonic)
//     one_of = [0x0C, 0x08]     # valores en los que las entidades EXISTEN
//
// A slot whose world reads (0,0) is treated as inactive and skipped (empty
// object slots in Sonic-style engines zero their position).  sin gate,
// la RAM de objetos reusada por el menú/título proyectaba instancias FANTASMA
// (el filtro world != (0,0) no alcanza) — el gate corta todo el perfil fuera
// del gameplay, y `min_sprites` filtra instancias con evidencia pobre.
// ---------------------------------------------------------------------------

use serde::Deserialize;

use crate::ram_anchor::{AnchorAssignment, AnchorBox, AnchoredEntity, assign_sprites};

fn default_count() -> u32 {
    1
}
fn default_true() -> bool {
    true
}
fn default_min_sprites() -> u32 {
    1
}

/// One *kind* of entity: a rule for reading 1+ instances of it from RAM.
#[derive(Debug, Clone, Deserialize)]
pub struct AnchorSpec {
    /// Human-readable name of the entity kind.
    #[serde(default)]
    pub name: String,
    /// Object struct address of the first instance; becomes the entity id.
    pub base: u32,
    /// World-X word offset relative to the instance base.
    pub x_off: u32,
    /// World-Y word offset relative to the instance base.
    pub y_off: u32,
    /// Number of consecutive instances (1 = singleton).
    #[serde(default = "default_count")]
    pub count: u32,
    /// Byte stride between instances (`id = base + i*stride`).
    #[serde(default)]
    pub stride: u32,
    /// Pivot from the object origin to the bounding-box anchor, in pixels.
    pub pivot: [i32; 2],
    /// Bounding-box half-extents used as matching tolerances, in pixels.
    pub half: [i32; 2],
    /// Minimum number of matched sprites required to accept an instance.
    ///
    /// The default is one. A larger value filters weak matches caused by stale
    /// or reused object memory.
    #[serde(default = "default_min_sprites")]
    pub min_sprites: u32,
}

/// Global validity gate for a game profile.
///
/// The byte at [`GateSpec::addr`] must equal one of [`GateSpec::one_of`] for
/// anchored entities to exist. This prevents reused menu or title-screen RAM
/// from producing phantom entities.
#[derive(Debug, Clone, Deserialize)]
pub struct GateSpec {
    /// 68k address of the gate byte.
    ///
    /// Reads honor [`GameProfile::word_swap`].
    pub addr: u32,
    /// Accepted byte values; an empty list keeps the gate closed.
    pub one_of: Vec<u8>,
}

/// A whole game's Mode 3 anchor profile.
#[derive(Debug, Clone, Deserialize)]
pub struct GameProfile {
    /// True when the work-RAM buffer is word-swapped (Genesis on LE hosts): a
    /// 16-bit value at an even address is read little-endian to recover the true
    /// big-endian 68k word. False = straight big-endian buffer.
    #[serde(default = "default_true")]
    pub word_swap: bool,
    /// Optional global validity gate. `None` leaves the profile always enabled.
    #[serde(default)]
    pub gate: Option<GateSpec>,
    /// Entity kinds read and matched by this profile.
    #[serde(rename = "anchor", default)]
    pub anchors: Vec<AnchorSpec>,
}

impl GameProfile {
    /// Parses a game profile from TOML.
    ///
    /// # Errors
    ///
    /// Returns the parser's diagnostic when `s` is not a valid profile.
    pub fn from_toml_str(s: &str) -> Result<Self, String> {
        toml::from_str(s).map_err(|e| e.to_string())
    }

    /// Read a signed 16-bit world coordinate at 68k `addr` from the work-RAM
    /// buffer, honouring `word_swap`. Returns None if out of range.
    fn read_world(&self, ram: &[u8], addr: u32) -> Option<i32> {
        let a = addr as usize;
        if a + 1 >= ram.len() {
            return None;
        }
        let raw = if self.word_swap {
            (ram[a] as u16) | ((ram[a + 1] as u16) << 8) // LE = swapped BE word
        } else {
            ((ram[a] as u16) << 8) | (ram[a + 1] as u16) // straight BE
        };
        Some(raw as i16 as i32)
    }

    /// Reads the byte at `addr`, honoring the profile's word-swap setting.
    fn read_byte(&self, ram: &[u8], addr: u32) -> Option<u8> {
        let a = if self.word_swap {
            (addr ^ 1) as usize
        } else {
            addr as usize
        };
        ram.get(a).copied()
    }

    /// Returns whether the global gate permits entities for this frame.
    pub fn gate_open(&self, ram: &[u8]) -> bool {
        match &self.gate {
            None => true,
            Some(g) => self
                .read_byte(ram, g.addr)
                .map(|v| g.one_of.contains(&v))
                .unwrap_or(false),
        }
    }

    /// Returns every active instance, tagged with its anchor-spec index.
    ///
    /// A closed global gate produces no entities.
    pub fn entities(&self, ram: &[u8]) -> Vec<(usize, AnchoredEntity)> {
        let mut out = Vec::new();
        if !self.gate_open(ram) {
            return out;
        }
        for (ki, k) in self.anchors.iter().enumerate() {
            for i in 0..k.count.max(1) {
                let obj = k.base.wrapping_add(i.wrapping_mul(k.stride));
                let wx = self.read_world(ram, obj.wrapping_add(k.x_off));
                let wy = self.read_world(ram, obj.wrapping_add(k.y_off));
                if let (Some(wx), Some(wy)) = (wx, wy) {
                    if wx == 0 && wy == 0 {
                        continue;
                    } // inactive slot
                    out.push((
                        ki,
                        AnchoredEntity {
                            id: obj as u64,
                            world_x: wx,
                            world_y: wy,
                        },
                    ));
                }
            }
        }
        out
    }

    /// Number of anchor kinds.
    pub fn kind_count(&self) -> usize {
        self.anchors.len()
    }

    /// Name of anchor kind `idx`.
    pub fn kind_name(&self, idx: usize) -> Option<&str> {
        self.anchors.get(idx).map(|a| a.name.as_str())
    }

    /// Which anchor kind an entity `id` (an object base address) belongs to —
    /// i.e. the kind whose `base + i*stride` (for some instance `i`) equals `id`.
    /// Lets a consumer map a resolved instance back to its kind (→ HD asset).
    pub fn kind_of_id(&self, id: u64) -> Option<usize> {
        for (ki, k) in self.anchors.iter().enumerate() {
            for i in 0..k.count.max(1) {
                if k.base.wrapping_add(i.wrapping_mul(k.stride)) as u64 == id {
                    return Some(ki);
                }
            }
        }
        None
    }

    /// Assign this frame's SAT sprites to the profile's entities.
    ///
    /// `sprite_pos` are screen-space top-lefts; `scroll_x/y` is the camera of the
    /// plane the entities live on (read from the VDP by the caller); `plane_w/h`
    /// are the plane size in pixels — the world→screen projection wraps modulo
    /// the plane, since Genesis scroll wraps there (pass 0 to disable wrapping).
    /// Runs `assign_sprites` once per kind (each kind has its own box) and
    /// concatenates the per-entity results (same order as `entities`).
    pub fn assign(
        &self,
        ram: &[u8],
        scroll_x: i32,
        scroll_y: i32,
        plane_w: i32,
        plane_h: i32,
        sprite_pos: &[(i16, i16)],
    ) -> Vec<AnchorAssignment> {
        let ents = self.entities(ram);
        let mut out = Vec::new();
        for (ki, k) in self.anchors.iter().enumerate() {
            // Project this kind's entities world→screen (wrap into the plane) and
            // hand them to the pure matcher with scroll already applied.
            let screen: Vec<AnchoredEntity> = ents
                .iter()
                .filter(|(i, _)| *i == ki)
                .map(|(_, e)| AnchoredEntity {
                    id: e.id,
                    world_x: wrap(e.world_x - scroll_x, plane_w),
                    world_y: wrap(e.world_y - scroll_y, plane_h),
                })
                .collect();
            if screen.is_empty() {
                continue;
            }
            let b = AnchorBox {
                pivot_x: k.pivot[0],
                pivot_y: k.pivot[1],
                half_w: k.half[0],
                half_h: k.half[1],
            };
            //  min_sprites — una instancia con menos matches que el piso
            // del kind se descarta (evidencia pobre = probable fantasma).
            let min = k.min_sprites.max(1) as usize;
            out.extend(
                assign_sprites(&screen, 0, 0, &b, sprite_pos)
                    .into_iter()
                    .filter(|a| a.members.len() >= min),
            );
        }
        out
    }
}

/// Reduce `v` into `[0, plane)` when `plane > 0`, else pass through.
fn wrap(v: i32, plane: i32) -> i32 {
    if plane > 0 {
        ((v % plane) + plane) % plane
    } else {
        v
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    // Store a big-endian 68k word into a *word-swapped* retro buffer (Genesis on
    // LE hosts): logical byte `addr` lives at `addr^1`.
    fn poke_swapped(ram: &mut [u8], addr: usize, val: u16) {
        ram[addr ^ 1] = (val >> 8) as u8; // logical high byte
        ram[addr] = (val & 0xFF) as u8; // logical low byte  (addr^1^1)
    }

    const PROFILE: &str = r#"
        word_swap = true
        [[anchor]]
        name   = "player"
        base   = 0xB000
        x_off  = 0x08
        y_off  = 0x0C
        count  = 2
        stride = 0x40
        pivot  = [0, 0]
        half   = [16, 16]
    "#;

    #[test]
    fn parses_and_defaults() {
        let p = GameProfile::from_toml_str(PROFILE).unwrap();
        assert!(p.word_swap);
        assert_eq!(p.anchors.len(), 1);
        let a = &p.anchors[0];
        assert_eq!(a.base, 0xB000);
        assert_eq!(a.count, 2);
        assert_eq!(a.stride, 0x40);
    }

    #[test]
    fn count_default_is_one() {
        let p = GameProfile::from_toml_str(
            "[[anchor]]\nbase=0x100\nx_off=0\ny_off=2\npivot=[0,0]\nhalf=[8,8]\n",
        )
        .unwrap();
        assert_eq!(p.anchors[0].count, 1);
        assert_eq!(p.anchors[0].stride, 0);
    }

    #[test]
    fn reads_word_swapped_world() {
        let p = GameProfile::from_toml_str(PROFILE).unwrap();
        let mut ram = vec![0u8; 0x10000];
        poke_swapped(&mut ram, 0xB008, 0x0140); // Sonic X = 320
        poke_swapped(&mut ram, 0xB00C, 0x0064); // Sonic Y = 100
        // Tails (0xB040) left at (0,0) → inactive, skipped.
        let ents = p.entities(&ram);
        assert_eq!(ents.len(), 1, "only the active (non-zero) instance");
        assert_eq!(ents[0].1.id, 0xB000);
        assert_eq!(ents[0].1.world_x, 320);
        assert_eq!(ents[0].1.world_y, 100);
    }

    #[test]
    fn array_reads_both_active_instances() {
        let p = GameProfile::from_toml_str(PROFILE).unwrap();
        let mut ram = vec![0u8; 0x10000];
        poke_swapped(&mut ram, 0xB008, 300);
        poke_swapped(&mut ram, 0xB00C, 400);
        poke_swapped(&mut ram, 0xB048, 305);
        poke_swapped(&mut ram, 0xB04C, 402);
        let ents = p.entities(&ram);
        assert_eq!(ents.len(), 2);
        assert_eq!(ents[1].1.id, 0xB040);
        assert_eq!(ents[1].1.world_x, 305);
    }

    #[test]
    fn assign_splits_two_instances_by_world_pos() {
        // Two players far apart; scroll shifts both; each claims its own sprites.
        let p = GameProfile::from_toml_str(PROFILE).unwrap();
        let mut ram = vec![0u8; 0x10000];
        poke_swapped(&mut ram, 0xB008, 200);
        poke_swapped(&mut ram, 0xB00C, 100); // → screen (120,100)
        poke_swapped(&mut ram, 0xB048, 300);
        poke_swapped(&mut ram, 0xB04C, 100); // → screen (220,100)
        // scroll (80,0); no wrap.
        let sprites = [(120i16, 100i16), (122, 98), (220, 100), (218, 102)];
        let out = p.assign(&ram, 80, 0, 0, 0, &sprites);
        assert_eq!(out.len(), 2);
        assert_eq!(out[0].id, 0xB000);
        assert_eq!(out[0].members, vec![0, 1]);
        assert_eq!(out[1].id, 0xB040);
        assert_eq!(out[1].members, vec![2, 3]);
    }

    #[test]
    fn kind_lookup_by_id_and_name() {
        let p = GameProfile::from_toml_str(PROFILE).unwrap();
        assert_eq!(p.kind_count(), 1);
        assert_eq!(p.kind_name(0), Some("player"));
        assert_eq!(p.kind_of_id(0xB000), Some(0)); // Sonic (base)
        assert_eq!(p.kind_of_id(0xB040), Some(0)); // Tails (base + stride)
        assert_eq!(p.kind_of_id(0xB020), None); // between instances → no kind
        assert_eq!(p.kind_name(9), None);
    }

    #[test]
    fn assign_wraps_world_into_plane() {
        // world beyond one plane still projects on-screen via the modulo.
        let p = GameProfile::from_toml_str(PROFILE).unwrap();
        let mut ram = vec![0u8; 0x10000];
        poke_swapped(&mut ram, 0xB008, 512 + 150);
        poke_swapped(&mut ram, 0xB00C, 100);
        let sprites = [(150i16, 100i16)];
        let out = p.assign(&ram, 0, 0, 512, 256, &sprites); // (662 mod 512) = 150
        assert_eq!(out[0].members, vec![0]);
    }

    ///  el gate de validez corta TODO el perfil fuera del gameplay — la
    /// RAM de objetos reusada por el menú/título proyectaba fantasmas.
    #[test]
    fn gate_blocks_entities_outside_gameplay() {
        const GATED: &str = r#"
            word_swap = true
            [gate]
            addr   = 0xF600
            one_of = [0x0C, 0x08]
            [[anchor]]
            name  = "player"
            base  = 0xB000
            x_off = 0x08
            y_off = 0x0C
            pivot = [0, 0]
            half  = [16, 16]
        "#;
        let p = GameProfile::from_toml_str(GATED).unwrap();
        let mut ram = vec![0u8; 0x10000];
        poke_swapped(&mut ram, 0xB008, 100);
        poke_swapped(&mut ram, 0xB00C, 100);
        // Título (game mode 0x04): gate cerrado → sin entidades ni assigns.
        ram[0xF600 ^ 1] = 0x04; // byte 68k con word_swap → addr^1
        assert!(!p.gate_open(&ram));
        assert!(p.entities(&ram).is_empty(), "gate cerrado = sin fantasmas");
        assert!(p.assign(&ram, 0, 0, 0, 0, &[(100, 100)]).is_empty());
        // Gameplay (0x0C): gate abierto → la entidad existe y asigna.
        ram[0xF600 ^ 1] = 0x0C;
        assert!(p.gate_open(&ram));
        assert_eq!(p.entities(&ram).len(), 1);
        assert_eq!(
            p.assign(&ram, 0, 0, 0, 0, &[(100, 100)])[0].members,
            vec![0]
        );
        // Sin [gate] (perfil legacy): siempre abierto.
        let legacy = GameProfile::from_toml_str(PROFILE).unwrap();
        assert!(legacy.gate_open(&ram));
    }

    ///  `min_sprites` descarta instancias con evidencia pobre (menos
    /// matches que el piso del kind).
    #[test]
    fn min_sprites_filters_weak_instances() {
        const MIN2: &str = r#"
            word_swap = true
            [[anchor]]
            name        = "player"
            base        = 0xB000
            x_off       = 0x08
            y_off       = 0x0C
            pivot       = [0, 0]
            half        = [16, 16]
            min_sprites = 2
        "#;
        let p = GameProfile::from_toml_str(MIN2).unwrap();
        let mut ram = vec![0u8; 0x10000];
        poke_swapped(&mut ram, 0xB008, 100);
        poke_swapped(&mut ram, 0xB00C, 100);
        // 1 solo sprite en caja → descartada (fantasma probable).
        assert!(p.assign(&ram, 0, 0, 0, 0, &[(100, 100)]).is_empty());
        // 2 sprites en caja → pasa el piso.
        let out = p.assign(&ram, 0, 0, 0, 0, &[(100, 100), (104, 104)]);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].members.len(), 2);
    }
}
