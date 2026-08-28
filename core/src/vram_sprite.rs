//! Sprite discovery, identity, grouping, and substitution from Mega Drive VRAM.
//!
//! The module decodes the VDP sprite table, hashes position-independent tile
//! content, groups poses and animations, and resolves authored sprite assets.

// ---------------------------------------------------------------------------
// vram_sprite.rs — Sprite-layer detection from Mega Drive VRAM.
//
// The MD VDP Sprite Attribute Table (SAT) lives inside VRAM at a base address
// set by VDP register $5.  For the most common Genesis Plus GX H40 mode
// (320-pixel wide) the SAT starts at 0xD800.
//
// Unlike TileHasher (which fingerprints the rendered framebuffer), SpriteHasher
// reads raw VRAM tile patterns to produce *position-independent* hashes: the
// same sprite character has the same hash whether it is at x=100 or x=200.
//
// ## SAT entry layout (8 bytes, big-endian words)
//
//   Word 0:  bits 9-0  = Y screen position (–128 offset, so 0x80 = top)
//   Word 1:  bits 13-8 = height (2b) | width (2b) | link (7b)
//            Encoded as: (height-1)<<10 | (width-1)<<8 | link
//            The actual layout documented by Sega:
//              [15:8] = Y[8:0] (sign-extended) — only [8:0] matter
//              [7:4]  = height-1 in tiles (0-3 = 1-4 tiles)
//              [3:0]  = link   (index of next sprite, 0 = end of chain)
//   Word 1 (second byte): width-1 in tiles packed differently…
//
//   The actual byte layout per Sega MD hardware manual:
//     Byte 0: Y[8] (MSB)     Byte 1: Y[7:0]       ← 9-bit signed Y
//     Byte 2: HH SS LL LL LL LL LL  where HH = height-1 (2b),
//             SS = size (actually: bits [7:4] = H (2b) + W (2b))
//     Byte 3: Link (7 bits: index of next sprite in chain)
//     Byte 4: P R HF VF palette[1:0] tile_hi[10]
//             (P=priority, R=reserved, HF=hflip, VF=vflip, tile MSBs)
//     Byte 5: tile_lo[7:0]   ← lower 8 bits of tile index
//     Byte 6: X[8] (MSB)     Byte 7: X[7:0]        ← 9-bit signed X
//
// Combined 11-bit tile index = (byte4[3:0] << 8) | byte5[7:0]
//
// ## Hash computation
//
// We hash the VRAM tile data for each tile in the sprite's pattern, after
// normalising for h-flip (pixel reversal).  This makes the hash stable
// across palette changes (luma quantisation, same as TileHasher) and
// positional changes (we don't include X/Y in the hash).
// ---------------------------------------------------------------------------

use std::collections::{HashMap, HashSet, VecDeque};
use xxhash_rust::xxh3::xxh3_64;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// SAT base address in VRAM for H40 (320px) mode.
/// This is *one* common value, but the real base is set by VDP register $5 and
/// varies by game/mode — do not rely on it. Kept for the unit tests, which place
/// a synthetic SAT here. Production code should pass [`SAT_AUTODETECT`].
pub const SAT_BASE_H40: usize = 0xD800;

/// Sentinel `sat_base` for [`SpriteHasher::process_vram`]: instead of trusting a
/// fixed address, scan VRAM and recover the SAT base from the sprite link chain.
///
/// The MD SAT base = `(VDP reg $5 & mask) << 9` and differs per game (and between
/// H32/H40), so any hardcoded constant detects sprites for at most a subset of
/// titles. genesis_plus_gx does not expose reg $5 over the libretro memory API,
/// so we derive the base from VRAM structure instead (see [`SpriteHasher::detect_sat_base`]).
pub const SAT_AUTODETECT: usize = usize::MAX;

/// Each sprite entry is 8 bytes.
pub const SAT_ENTRY_SIZE: usize = 8;

/// Maximum sprites in H40 mode.
pub const MAX_SPRITES_H40: usize = 80;

/// Each tile in VRAM is 32 bytes (8×8 pixels, 4bpp = 2 pixels/byte).
pub const VRAM_TILE_BYTES: usize = 32;

/// Screen offset added to stored X/Y coordinates.
pub const SPRITE_COORD_OFFSET: i16 = 128;

/// Returns whether a sprite lies completely outside the hasher's scan bounds.
///
/// The bounds include an H40-sized screen plus one tile of slack, allowing both
/// raw-coordinate parked sprites and stale entries beyond the right or lower edge
/// to be rejected consistently by [`SpriteHasher::process_vram`] and
/// [`SpriteHasher::process_parsed_sprites`]. Pose matching separately uses the
/// active display area's stricter visible bounds.
pub fn offscreen_discarded(x: i16, y: i16, w_tiles: u8, h_tiles: u8) -> bool {
    x <= -(w_tiles as i16 * 8) || y <= -(h_tiles as i16 * 8) || x >= 336 || y >= 240
}

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// AnimationGrouper — detects animation cycles from SAT slot history
//
// Each SAT slot (0-79) is tracked over a rolling window of ANIM_WINDOW frames.
// If ≥ ANIM_MIN_DISTINCT different hashes appear at the same slot (each at
// least ANIM_MIN_OCCURRENCES times), they are considered animation frames of
// the same character and receive a shared group_id.
//
// group_id = xxhash3_64(sorted_member_hashes) — deterministic and stable as
// long as the animation set doesn't change.  group_id == 0 means ungrouped.
//
// Groups are recomputed every ANIM_RECOMPUTE_PERIOD frames to amortise cost.
// With 80 slots × 64-frame windows the recompute is ~5 120 hash accesses —
// completely negligible at 60 fps.
// ---------------------------------------------------------------------------

const ANIM_WINDOW: usize = 64; // frames of rolling history per slot
const ANIM_MIN_DISTINCT: usize = 2; // need ≥2 distinct hashes to form a group
const ANIM_MIN_OCCURRENCES: u32 = 2; // each hash must appear ≥2× in the window
const ANIM_RECOMPUTE_PERIOD: u64 = 16; // recompute every N frames

/// One frame (pose) of a detected animation clip: the pose hash + how long it is
/// held, in game frames (ticks). C-S1.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AnimFrame {
    /// Stable pose hash.
    pub pose: u64,
    /// Number of emulated frames for which the pose is held.
    pub duration: u16,
}

/// A detected animation clip: the ordered sequence of poses (each with its
/// per-frame duration) that cycle at one SAT slot, plus whether it loops.
///
/// `id` equals the `anim_group_id` (xxhash of the sorted member set) so it is
/// stable across sessions and matches the occurrences' `anim_group_id`. The frame
/// order is canonicalised — rotated to start at the smallest pose hash — so it is
/// deterministic regardless of where the rolling window happened to start. C-S1.
#[derive(Clone, Debug)]
pub struct AnimationClip {
    /// Stable animation-group identifier.
    pub id: u64,
    /// Ordered poses and their observed durations.
    pub frames: Vec<AnimFrame>,
    /// Whether the observed sequence returns to its first pose.
    pub looping: bool,
}

struct AnimationGrouper {
    /// SAT slot index → ring buffer of hashes seen at that slot (capped at ANIM_WINDOW).
    slot_histories: HashMap<u8, VecDeque<u64>>,
    frame_counter: u64,
    /// Stable cache: hash → group_id (0 = ungrouped).
    /// Updated every ANIM_RECOMPUTE_PERIOD frames.
    hash_to_group: HashMap<u64, u64>,
    /// Ordered clip per group (sequence + duration + looping), rebuilt on
    /// recompute. Keyed by group_id; first-seen wins (matches hash_to_group). C-S1.
    group_to_clip: HashMap<u64, AnimationClip>,
    /// Deterministic enumeration order for the FFI (sorted by id). C-S1.
    clip_list: Vec<AnimationClip>,
}

impl AnimationGrouper {
    fn new() -> Self {
        Self {
            slot_histories: HashMap::new(),
            frame_counter: 0,
            hash_to_group: HashMap::new(),
            group_to_clip: HashMap::new(),
            clip_list: Vec::new(),
        }
    }

    /// Record that `hash` was seen at SAT `slot`.
    fn record(&mut self, slot: u8, hash: u64) {
        let hist = self.slot_histories.entry(slot).or_default();
        hist.push_back(hash);
        if hist.len() > ANIM_WINDOW {
            hist.pop_front();
        }
    }

    /// Advance the internal frame counter; triggers a group recompute every
    /// ANIM_RECOMPUTE_PERIOD frames.
    fn advance_frame(&mut self) {
        self.frame_counter += 1;
        if self.frame_counter.is_multiple_of(ANIM_RECOMPUTE_PERIOD) {
            self.recompute();
        }
    }

    /// Recompute the hash→group_id cache from current slot histories.
    fn recompute(&mut self) {
        self.hash_to_group.clear();
        self.group_to_clip.clear();

        for hist in self.slot_histories.values() {
            // Count occurrences of each distinct hash in this slot's window.
            let mut counts: HashMap<u64, u32> = HashMap::new();
            for &hash in hist {
                *counts.entry(hash).or_insert(0) += 1;
            }

            // Collect hashes that appear often enough.
            let mut members: Vec<u64> = counts
                .into_iter()
                .filter(|(_, cnt)| *cnt >= ANIM_MIN_OCCURRENCES)
                .map(|(h, _)| h)
                .collect();

            if members.len() < ANIM_MIN_DISTINCT {
                continue; // single (static) sprite — not an animation
            }

            // Group ID = xxhash3_64 of the sorted member set → deterministic.
            members.sort_unstable();
            let raw: Vec<u8> = members.iter().flat_map(|h| h.to_le_bytes()).collect();
            let group_id = xxh3_64(&raw);

            for &hash in &members {
                // First-seen wins: a hash cycling through two slots keeps its
                // first group assignment (edge case, should be rare).
                self.hash_to_group.entry(hash).or_insert(group_id);
            }

            // C-S1: consolidate the ordered clip (sequence + timing) from this
            // slot's history. First-seen wins, mirroring hash_to_group above.
            if !self.group_to_clip.contains_key(&group_id)
                && let Some(clip) = consolidate_clip(group_id, hist)
            {
                self.group_to_clip.insert(group_id, clip);
            }
        }

        // Deterministic enumeration order (sorted by id) for the FFI. C-S1.
        self.clip_list = self.group_to_clip.values().cloned().collect();
        self.clip_list.sort_by_key(|c| c.id);
    }

    /// Return the group_id for `hash`, or 0 if it is not yet grouped.
    fn group_id_for(&self, hash: u64) -> u64 {
        *self.hash_to_group.get(&hash).unwrap_or(&0)
    }

    /// Detected animation clips (C-S1), in deterministic order (sorted by id).
    /// Empty until the grouper has warmed up (ANIM_RECOMPUTE_PERIOD frames).
    fn clips(&self) -> &[AnimationClip] {
        &self.clip_list
    }

    /// Wipe all accumulated history/groups/clips. Called when (re)starting clip
    /// generation so the result reflects only the recording being scanned (C-S5).
    fn reset(&mut self) {
        self.slot_histories.clear();
        self.frame_counter = 0;
        self.hash_to_group.clear();
        self.group_to_clip.clear();
        self.clip_list.clear();
    }
}

/// Consolidate one SAT slot's rolling hash history into an ordered animation clip
/// — the missing "phase" the §4 plan calls for (the set is known; this adds order
/// + timing). C-S1.
///
/// Pipeline:
///   1. Run-length encode the per-frame history → runs of `(pose, frames_held)`.
///   2. Find the smallest *clean period* of the pose sequence (the repeating
///      cycle). Falls back to distinct-poses-in-first-seen-order when the history
///      is too noisy to be cleanly periodic.
///   3. Duration per pose = the mode of its **interior** run lengths (the first
///      and last runs are usually truncated by the rolling window, so they are
///      excluded unless that leaves no sample).
///   4. Rotate the cycle to start at the smallest pose hash → deterministic order
///      independent of the window's phase.
///
/// Returns `None` when the history is not a real cycle (fewer than two distinct
/// consecutive poses).
fn consolidate_clip(group_id: u64, hist: &VecDeque<u64>) -> Option<AnimationClip> {
    if hist.len() < 2 {
        return None;
    }

    // 1. Run-length encode.
    let mut runs: Vec<(u64, u32)> = Vec::new();
    for &h in hist {
        match runs.last_mut() {
            Some(last) if last.0 == h => last.1 += 1,
            _ => runs.push((h, 1)),
        }
    }
    if runs.len() < 2 {
        return None;
    } // a single pose held → static, not a cycle
    let last_idx = runs.len() - 1;
    let poses: Vec<u64> = runs.iter().map(|r| r.0).collect();
    let n = poses.len();

    // 1b. Rechazar slots casi-estáticos: si UNA pose acapara >70% de la ventana es
    // un sprite casi-quieto con poses transitorias (flicker de 1 frame), no una
    // animación real. Filtra la basura tipo [1t,1t,…,54t] que ensuciaba la lista.
    let total: u32 = runs.iter().map(|(_, l)| *l).sum();
    let mut by_pose: HashMap<u64, u32> = HashMap::new();
    for &(h, l) in &runs {
        *by_pose.entry(h).or_insert(0) += l;
    }
    let dominant = by_pose.values().copied().max().unwrap_or(0);
    if dominant * 10 > total * 7 {
        return None;
    }

    // 2. Smallest clean period (≥2 distinct poses) over the run sequence.
    let mut period = 0usize;
    'outer: for p in 1..=n / 2 {
        for i in 0..n - p {
            if poses[i] != poses[i + p] {
                continue 'outer;
            }
        }
        let distinct: std::collections::HashSet<u64> = poses[..p].iter().copied().collect();
        if distinct.len() >= 2 {
            period = p;
            break;
        }
    }
    let cycle_poses: Vec<u64> = if period > 0 {
        poses[..period].to_vec()
    } else {
        // Fallback: distinct poses in first-seen order (dedup, keep order).
        let mut seen = std::collections::HashSet::new();
        poses.iter().copied().filter(|h| seen.insert(*h)).collect()
    };
    if cycle_poses.len() < 2 {
        return None;
    }

    // 3. Duration per pose: mode of interior runs (fallback: all runs of the pose).
    let dur_for = |pose: u64| -> u16 {
        let mut samples: Vec<u32> = runs
            .iter()
            .enumerate()
            .filter(|(i, (h, _))| *h == pose && *i != 0 && *i != last_idx)
            .map(|(_, (_, l))| *l)
            .collect();
        if samples.is_empty() {
            samples = runs
                .iter()
                .filter(|(h, _)| *h == pose)
                .map(|(_, l)| *l)
                .collect();
        }
        // Mode; on a tie prefer the smaller length (deterministic).
        let mut counts: HashMap<u32, u32> = HashMap::new();
        for s in samples {
            *counts.entry(s).or_insert(0) += 1;
        }
        let mut best: (u32, u32) = (0, 1); // (count, len)
        for (&len, &cnt) in &counts {
            if cnt > best.0 || (cnt == best.0 && len < best.1) {
                best = (cnt, len);
            }
        }
        best.1.clamp(1, u16::MAX as u32) as u16
    };

    // 4. Rotate to start at the smallest pose hash (phase-independent order).
    let start = cycle_poses
        .iter()
        .enumerate()
        .min_by_key(|(_, h)| **h)
        .map(|(i, _)| i)
        .unwrap_or(0);
    let len = cycle_poses.len();
    let frames: Vec<AnimFrame> = (0..len)
        .map(|k| {
            let pose = cycle_poses[(start + k) % len];
            AnimFrame {
                pose,
                duration: dur_for(pose),
            }
        })
        .collect();

    // Un período LIMPIO ya implica que la secuencia se repite → es un loop. Antes
    // se exigía ver 2 ciclos completos (n >= 2*period), pero si la ventana captura
    // 1.x ciclos el período igual está y el clip SÍ loopea (la caminata real caía
    // por esto, marcada como no-loop).
    let looping = period > 0;
    Some(AnimationClip {
        id: group_id,
        frames,
        looping,
    })
}

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

/// One detected sprite occurrence this frame.
///
/// `hash` is stable for the same sprite pattern regardless of screen position,
/// flip state normalisation, or palette selection.
/// `screen_x` and `screen_y` are the top-left pixel corner of the sprite.
/// `anim_group_id` is non-zero when this hash has been identified as part of
/// an animation cycle (multiple hashes cycling at the same SAT slot).  All
/// frames of the same animation share the same `anim_group_id`.
#[derive(Clone, Debug)]
pub struct SpriteOccurrence {
    /// Palette- and flip-independent sprite pattern hash.
    pub hash: u64,
    /// Animation group — non-zero once the grouper has seen enough history.
    /// Frames of the same animation share this value.  0 = ungrouped.
    pub anim_group_id: u64,
    /// Sprite width in tiles (1–4).
    pub w_tiles: u8,
    /// Sprite height in tiles (1–4).
    pub h_tiles: u8,
    /// Top-left X in screen pixels.
    pub screen_x: i16,
    /// Top-left Y in screen pixels.
    pub screen_y: i16,
    /// SAT link field (index of the next sprite in the chain, 0–127). The
    /// strongest metasprite grouping hint — games chain an entity's sprites
    /// with consecutive links. (R1.5)
    pub link: u8,
    /// Palette index (0–3) from the VDP sprite attributes. Secondary grouping
    /// hint (members of one entity usually share a palette). (R1.5)
    pub palette: u8,
    /// VDP priority bit (0 = low, 1 = high) — the sprite's depth vs the
    /// background planes. Lets a metasprite split into front/back HD layers
    /// (addendum §1). MSB of the sprite attribute word.
    pub priority: u8,
    /// SAT slot index (0–79) of this occurrence. Lets Ayther suppress a specific
    /// sprite in the render (hide-by-hash) without re-deriving the chain.
    pub slot: u8,
    /// VDP horizontal-flip bit used to mirror the HD asset automatically.
    pub hflip: u8,
    /// VDP vertical flip bit (CU-AN-11).
    pub vflip: u8,
}

/// Computes the stable in-between identity of an ordered pose-member list.
pub fn pose_key_of(hashes: &[u64]) -> u64 {
    let mut b = Vec::with_capacity(hashes.len() * 8);
    for h in hashes {
        b.extend_from_slice(&h.to_le_bytes());
    }
    xxh3_64(&b)
}

/// One resolved HD substitution for a sprite.
#[derive(Clone, Debug)]
pub struct SpriteSub {
    /// Logical HD asset path inside the pack.
    pub asset_path: String,
    /// Horizontal top-left position in screen pixels.
    pub screen_x: i16,
    /// Vertical top-left position in screen pixels.
    pub screen_y: i16,
    /// Destination width in native tiles.
    pub w_tiles: u8,
    /// Destination height in native tiles.
    pub h_tiles: u8,
    /// Exact destination width in pixels.
    ///
    /// Pose bounding boxes need not be tile-aligned. Zero derives the width from
    /// [`Self::w_tiles`] for tile-exact producers.
    pub w_px: u16,
    /// Exact destination height in pixels; zero derives it from [`Self::h_tiles`].
    pub h_px: u16,
    /// Pose-match transform: bit 0 is horizontal mirror and bit 1 is vertical
    /// mirror; zero is the captured orientation.
    ///
    /// The renderer applies these bits to the canonical asset. Per-sprite results
    /// and variant-selected poses use zero because orientation is handled by their
    /// own candidate or occurrence data.
    pub mirror: u8,
    /// Palette observed on the sprite or pose anchor, or `0xFF` when unknown.
    ///
    /// Anchoring color modulation to a specific member avoids selecting the
    /// palette of an unrelated overlapping occurrence.
    pub palette: u8,
    /// Stable identity of the pose that emitted this substitution.
    ///
    /// It is xxHash3 over member hashes in captured order; a per-sprite result
    /// hashes its single member, so one-sprite `[[sub]]` and `[[pose]]` entries
    /// share an identity. [`TweenPlayer`] uses the value to track instances and
    /// ignore variant changes that do not represent a new pose.
    pub pose_key: u64,
    /// Authored palette of the chosen candidate when it differs from the observed
    /// palette, or `0xFF` when no palette synthesis is required.
    pub synth_pal: u8,
    /// Authored E1 tint reference: average 0–255 RGB of the CRAM line captured
    /// with the pose. `[0, 0, 0]` selects the scalar peak-hold fallback.
    pub ref_rgb: [u8; 3],
    /// Horizontal origin of the normalized asset sub-rectangle.
    pub u0: f32,
    /// Vertical origin of the normalized asset sub-rectangle.
    pub v0: f32,
    /// Width of the normalized asset sub-rectangle.
    pub uw: f32,
    /// Height of the normalized asset sub-rectangle.
    pub vh: f32,
    /// Grayscale costume-tint mask for a pose's base asset.
    ///
    /// White follows the full palette tint and black preserves only luminance.
    /// An empty string means no mask. The mask uses the same UV sub-rectangle as
    /// the asset.
    pub mask_path: String,
}

// ---------------------------------------------------------------------------
// SpriteHasher
// ---------------------------------------------------------------------------

/// Extracts and catalogs sprite identities from VDP sprite-table data.
pub struct SpriteHasher {
    /// Total unique sprites accumulated across frames.
    catalog: HashMap<u64, (u64, u8, u8)>, // hash → (first_frame, w, h)
    frame_index: u64,
    last_occurrences: Vec<SpriteOccurrence>,
    /// Detects animation cycles from SAT slot histories.
    anim_grouper: AnimationGrouper,
}

impl SpriteHasher {
    /// Creates an empty sprite catalog and animation grouper.
    pub fn new() -> Self {
        Self {
            catalog: HashMap::new(),
            frame_index: 0,
            last_occurrences: Vec::new(),
            anim_grouper: AnimationGrouper::new(),
        }
    }

    /// Process one frame of VRAM data.
    ///
    /// Parses the SAT starting at `sat_base` (default: `SAT_BASE_H40`).
    /// Returns the number of *new* unique sprites discovered this frame.
    pub fn process_vram(&mut self, vram: &[u8], sat_base: usize) -> u32 {
        self.last_occurrences.clear();

        // Resolve the SAT base. SAT_AUTODETECT recovers it from VRAM structure
        // (the production path); an explicit value is used as-is (unit tests).
        let sat_base = if sat_base == SAT_AUTODETECT {
            match Self::detect_sat_base(vram) {
                Some(b) => b,
                None => return 0, // no plausible SAT this frame → no sprites
            }
        } else {
            sat_base
        };

        if vram.len() < sat_base + MAX_SPRITES_H40 * SAT_ENTRY_SIZE {
            return 0;
        }

        let mut new_this_frame = 0u32;

        // Escaneo LINEAL de los 80 slots — NO seguir la cadena de links. La lista
        // autoritativa de Y/size/link vive en el cache interno de SAT del VDP
        // (actualizado al ESCRIBIR la SAT); la copia en VRAM puede quedar stale:
        // Sonic 2 EHZ deja una cadena CÍCLICA (17→68→49→17…) que atrapa a un
        // chain-walk en 3 sprites y nunca llega al jugador (validado contra ROM
        // en tools/mode3_spike). Los slots no usados quedan parqueados fuera de
        // pantalla (Y crudo = 0 → screen −128) → el filtro de abajo los descarta.
        for slot in 0..MAX_SPRITES_H40 {
            let entry_off = sat_base + slot * SAT_ENTRY_SIZE;
            if entry_off + SAT_ENTRY_SIZE > vram.len() {
                break;
            }

            let entry = &vram[entry_off..entry_off + SAT_ENTRY_SIZE];

            // La entrada SAT son 4 WORDS del 68k. El buffer de VRAM del fork viene
            // word-swapped en hosts LE (byte lógico en off^1): leer cada word en
            // LITTLE-ENDIAN reconstruye el word big-endian del bus — la MISMA
            // convención que los lectores de nametables (ayther_session rd16) y
            // el escaneo SAT de tools/mode3_spike (vrd16), validada contra ROM.
            let rdw = |o: usize| -> u16 { (entry[o] as u16) | ((entry[o + 1] as u16) << 8) };
            let w0 = rdw(0); // Y (bits 9:0)
            let w1 = rdw(2); // vsize (11:10) · hsize (9:8) · link (6:0)
            let w2 = rdw(4); // pri (15) · pal (14:13) · vflip (12) · hflip (11) · tile (10:0)
            let w3 = rdw(6); // X (bits 8:0)

            let screen_y = (w0 & 0x3FF) as i16 - SPRITE_COORD_OFFSET;
            let h = (((w1 >> 10) & 0x3) as u8) + 1; // alto en tiles (1..4)
            let w = (((w1 >> 8) & 0x3) as u8) + 1; // ancho en tiles (1..4)
            let priority = ((w2 >> 15) & 0x1) as u8;
            let palette = ((w2 >> 13) & 0x3) as u8;
            let vflip = (w2 & 0x1000) != 0;
            let hflip = (w2 & 0x0800) != 0;
            let tile_idx = w2 & 0x7FF;
            let screen_x = (w3 & 0x1FF) as i16 - SPRITE_COORD_OFFSET;

            // Saltar sprites fuera de pantalla (ver offscreen_discarded: parqueados/
            // escondidos o stale más allá del borde — predicado compartido con el
            // matching de poses).
            if offscreen_discarded(screen_x, screen_y, w, h) {
                continue;
            }

            // Compute the hash of the sprite's tile pattern data from VRAM.
            let hash = self.hash_sprite_pattern(vram, tile_idx as usize, w, h);

            // Record to animation grouper (uses current group cache — updated every
            // ANIM_RECOMPUTE_PERIOD frames by advance_frame below).
            self.anim_grouper.record(slot as u8, hash);
            let anim_group_id = self.anim_grouper.group_id_for(hash);

            // Record occurrence.
            self.last_occurrences.push(SpriteOccurrence {
                hash,
                anim_group_id,
                w_tiles: w,
                h_tiles: h,
                screen_x,
                screen_y,
                link: (w1 & 0x7F) as u8,
                palette,
                priority,
                slot: slot as u8,
                hflip: hflip as u8,
                vflip: vflip as u8,
            });

            use std::collections::hash_map::Entry;
            if let Entry::Vacant(e) = self.catalog.entry(hash) {
                e.insert((self.frame_index, w, h));
                new_this_frame += 1;
            }
        }

        // Advance the grouper frame counter; may trigger a group recompute.
        self.anim_grouper.advance_frame();
        self.frame_index += 1;
        new_this_frame
    }

    /// Process the list of sprites the VDP actually PARSED this frame, captured by
    /// the fork in `parse_satb` (id 0x10B). Each record is 8 bytes: yr(u16 LE) ·
    /// xr(u16) · attr(u16) · w(u8) · h(u8), already deduped by (yr,xr,attr). This is
    /// the authoritative "what was drawn" source — robust to the SAT being rewritten
    /// mid-frame / its base swapped (e.g. Aladdin's Sega-logo genie, where reading
    /// the SAT at frame-end shows only placeholders). Tiles are hashed from `vram`.
    /// Empty `data` → returns 0 (the engine falls back to single-base autodetect).
    /// Records are 10 bytes: `{yr: u16, xr: u16, attr: u16, w: u8, h: u8,
    /// sat_idx: u8, chain_pos: u8}`. `sat_idx` is the actual SAT entry index used
    /// by the suppression mask, not the record-list position. `chain_pos` is the
    /// VDP link-chain draw priority, with smaller values in front.
    pub fn process_parsed_sprites(&mut self, data: &[u8], vram: &[u8]) -> u32 {
        self.last_occurrences.clear();
        let mut new_this_frame = 0u32;
        let n = data.len() / 10;
        for i in 0..n {
            let o = i * 10;
            let yr = (u16::from_le_bytes([data[o], data[o + 1]]) & 0x1FF) as i16;
            let xr = (u16::from_le_bytes([data[o + 2], data[o + 3]]) & 0x1FF) as i16;
            let attr = u16::from_le_bytes([data[o + 4], data[o + 5]]);
            let w = data[o + 6].max(1);
            let h = data[o + 7].max(1);
            let sat_idx = data[o + 8];
            let chain_pos = data[o + 9];
            let screen_x = xr - SPRITE_COORD_OFFSET;
            let screen_y = yr - SPRITE_COORD_OFFSET;
            if offscreen_discarded(screen_x, screen_y, w, h) {
                continue;
            }
            let tile_idx = (attr & 0x07FF) as usize;
            if tile_idx == 0 {
                continue;
            } // sprite vacío
            let hflip = (attr & 0x0800) != 0;
            let vflip = (attr & 0x1000) != 0;
            let palette = ((attr >> 13) & 0x3) as u8;
            let priority = ((attr >> 15) & 0x1) as u8;

            let hash = self.hash_sprite_pattern(vram, tile_idx, w, h);
            self.anim_grouper.record(sat_idx, hash); // por slot SAT real (identidad estable)
            let anim_group_id = self.anim_grouper.group_id_for(hash);
            self.last_occurrences.push(SpriteOccurrence {
                hash,
                anim_group_id,
                w_tiles: w,
                h_tiles: h,
                screen_x,
                screen_y,
                link: chain_pos, // prioridad real de dibujo del VDP (C1)
                palette,
                priority,
                slot: sat_idx, // índice SAT real → supresión 0x103 alineada
                hflip: hflip as u8,
                vflip: vflip as u8,
            });
            use std::collections::hash_map::Entry;
            if let Entry::Vacant(e) = self.catalog.entry(hash) {
                e.insert((self.frame_index, w, h));
                new_this_frame += 1;
            }
        }
        self.anim_grouper.advance_frame();
        self.frame_index += 1;
        new_this_frame
    }

    /// Returns the cumulative number of unique sprite patterns.
    pub fn unique_sprite_count(&self) -> u32 {
        self.catalog.len() as u32
    }
    /// Returns sprite occurrences from the most recently processed frame.
    pub fn last_occurrences(&self) -> &[SpriteOccurrence] {
        &self.last_occurrences
    }

    /// Detected animation clips (C-S1): the ordered pose sequence + per-frame
    /// duration + looping for each animation group, derived from the SAT slot
    /// histories. Stable order (sorted by id). Empty until the grouper has warmed
    /// up (ANIM_RECOMPUTE_PERIOD frames). The artist refines these in the
    /// animation workspace, where artists can refine the detected result.
    pub fn animation_clips(&self) -> &[AnimationClip] {
        self.anim_grouper.clips()
    }

    /// Reset the animation detector (clear all slot history / groups / clips).
    /// Called at the start of clip generation (C-S5) so the detected clips reflect
    /// only the recording being scanned, not whatever played before.
    pub fn reset_animation_grouper(&mut self) {
        self.anim_grouper.reset();
    }

    /// Recover the SAT base address from VRAM contents.
    ///
    /// The hardware processes sprites by following the SAT *link chain* from
    /// entry 0, so a real SAT has a coherent chain (acyclic, in-range links,
    /// terminating at link 0) whose sprites sit on the visible screen. Random
    /// VRAM tile data almost never forms such a chain. We score every
    /// 512-byte-aligned candidate base (the MD SAT alignment) and return the
    /// best one, or `None` if no plausible SAT is present this frame.
    ///
    /// Pure function of `vram` — deterministic, holds no cross-frame state, so it
    /// is safe for the savestate-determinism guarantees.
    pub fn detect_sat_base(vram: &[u8]) -> Option<usize> {
        const ALIGN: usize = 0x200; // MD SAT is 512-byte aligned (1 KB in H40)
        let mut best: Option<(usize, u32)> = None;
        let mut base = 0usize;
        while base + MAX_SPRITES_H40 * SAT_ENTRY_SIZE <= vram.len() {
            if let Some(score) = Self::sat_chain_score(vram, base) {
                // Strictly-greater keeps the lowest-addressed base on ties →
                // deterministic regardless of scan order.
                if best.is_none_or(|(_, b)| score > b) {
                    best = Some((base, score));
                }
            }
            base += ALIGN;
        }
        best.map(|(b, _)| b)
    }

    /// Score a candidate SAT base by walking the link chain from entry 0.
    /// Returns `Some(score)` for a coherent chain (score favours more on-screen
    /// sprites, then a longer chain), or `None` if the chain is implausible
    /// (out-of-range link, or too few on-screen sprites).
    ///
    /// The caller guarantees `base + MAX_SPRITES_H40 * SAT_ENTRY_SIZE <= len`, so
    /// any slot index in `0..MAX_SPRITES_H40` is in bounds.
    fn sat_chain_score(vram: &[u8], base: usize) -> Option<u32> {
        const MIN_HOPS: u32 = 3;
        const MIN_ONSCREEN: u32 = 3;

        let mut seen = [false; MAX_SPRITES_H40];
        let mut next = 0usize;
        let mut hops = 0u32;
        let mut on_screen = 0u32;

        loop {
            if seen[next] {
                break;
            } // cycle → stop walking
            seen[next] = true;

            let off = base + next * SAT_ENTRY_SIZE;
            let e = &vram[off..off + SAT_ENTRY_SIZE];

            // Words LE del buffer del fork (ver process_vram): reconstruyen los
            // words big-endian del 68k.
            let rdw = |o: usize| -> u16 { (e[o] as u16) | ((e[o + 1] as u16) << 8) };
            let w1 = rdw(2); // vsize (11:10) · hsize (9:8) · link (6:0)

            let screen_y = (rdw(0) & 0x3FF) as i16 - SPRITE_COORD_OFFSET;
            let screen_x = (rdw(6) & 0x1FF) as i16 - SPRITE_COORD_OFFSET;
            let h = (((w1 >> 10) & 0x3) as i16 + 1) * 8;
            let w = (((w1 >> 8) & 0x3) as i16 + 1) * 8;

            // On a 320×224 (H40) frame with one tile of slack — covers H32 too.
            if screen_x > -w && screen_x < 336 && screen_y > -h && screen_y < 240 {
                on_screen += 1;
            }
            hops += 1;

            let link = (w1 & 0x7F) as usize;
            if link == 0 {
                break;
            } // normal end of chain
            if link >= MAX_SPRITES_H40 {
                return None;
            } // OOR link → not a SAT
            next = link;
        }

        (hops >= MIN_HOPS && on_screen >= MIN_ONSCREEN).then_some(on_screen * 1000 + hops.min(999))
    }

    // ---- private ----

    /// Hash all tile data for a sprite pattern, normalised for flips.
    /// Hash del PATRÓN CRUDO del sprite en VRAM — flip-INVARIANTE por
    /// construcción: una instancia espejada por el VDP usa los MISMOS tiles, así
    /// que comparte hash con la cara original. La identidad de pose, los
    /// arreglos espejados del matching y las variantes por flip DEPENDEN de esta
    /// propiedad; la versión anterior aplicaba hflip/vflip al decodificar (hash
    /// de la apariencia) y un espejo hasheaba distinto → «Walk 01» y «Walk 05»
    /// de Golden Axe se creaban como poses distintas. La orientación observada
    /// viaja aparte en la occurrence (hflip/vflip). Paleta-ciego: se hashea el
    /// índice de color 0-15, no el color. Para occurrences SIN flip el buffer es
    /// byte-idéntico al de la versión anterior → esos hashes no migran.
    fn hash_sprite_pattern(&self, vram: &[u8], tile_idx: usize, w: u8, h: u8) -> u64 {
        let total_tiles = w as usize * h as usize;
        let mut luma_buf = Vec::with_capacity(total_tiles * 64);

        // Tiles del sprite: contiguos en VRAM (column-major del SAT = orden de
        // almacenamiento). Se recorren en ese orden, sin transformar.
        for t in 0..total_tiles {
            let tile_off = (tile_idx + t) * VRAM_TILE_BYTES;
            if tile_off + VRAM_TILE_BYTES > vram.len() {
                luma_buf.extend_from_slice(&[0u8; 64]);
                continue;
            }
            // Decode 4bpp: each byte = 2 pixels (high nibble first).
            // Quantise palette index directly (0-15 → 4-bit value, palette-blind).
            for &byte in &vram[tile_off..tile_off + VRAM_TILE_BYTES] {
                luma_buf.push((byte >> 4) & 0x0F);
                luma_buf.push(byte & 0x0F);
            }
        }

        xxh3_64(&luma_buf)
    }
}

impl Default for SpriteHasher {
    fn default() -> Self {
        Self::new()
    }
}

// ---------------------------------------------------------------------------
// SpriteSubstitutor
// ---------------------------------------------------------------------------

/// Maps a sprite identity → HD asset path.
///
/// The `hash` is flip- and palette-blind by design, so a bare hash is the most
/// general key (rule 4 in `lookup_in` = the historical behaviour). A `[[sub]]`
/// in the pack TOML may additionally pin `palette` and/or `flip`; `resolve` then
/// picks the **most specific** match, letting one pattern carry different HD art
/// per palette (day/night, P1/P2) or per direction (hand-mirrored, not the
/// automatic flip) — the "variantes" of the authoring model.
///
/// Priority model (unchanged): overrides (Lua) > catalog (pack TOML).
/// Per-sprite catalog entry with its authored color reference.
#[derive(Clone, Debug, PartialEq)]
pub struct SubDef {
    /// Logical HD asset path.
    pub asset: String,
    /// Authored reference color; zero selects legacy scalar tinting.
    pub ref_rgb: [u8; 3],
}

/// Resolves individual sprite hashes to HD assets.
pub struct SpriteSubstitutor {
    catalog: HashMap<u64, SubDef>, // hash → def (comodín: cualquier paleta)
    pal_catalog: HashMap<(u64, u8), SubDef>, // (hash, paleta) → def (variante por paleta, CU-AN-10)
    overrides: HashMap<u64, SubDef>,
}

impl SpriteSubstitutor {
    /// Creates an empty sprite substitution catalog.
    pub fn new() -> Self {
        Self {
            catalog: HashMap::new(),
            pal_catalog: HashMap::new(),
            overrides: HashMap::new(),
        }
    }

    /// Load from `sprite_substitutions.toml` in the pack.
    pub fn load_from_pack(&mut self, pack: &crate::archive_vfs::AyArchive) {
        self.catalog.clear();
        self.pal_catalog.clear();
        if let Some(data) = pack.read("sprite_substitutions.toml")
            && let Ok(s) = std::str::from_utf8(&data)
        {
            self.parse_toml(s);
        }
    }

    /// Returns the total number of wildcard and palette-specific catalog entries.
    pub fn catalog_len(&self) -> usize {
        self.catalog.len() + self.pal_catalog.len()
    }

    /// Live authoring override (Lua / Lab). Hash-only = wildcard on palette and
    /// orientation (rule 4), preserving the historical hash-keyed behaviour.
    /// An absent color reference selects the legacy scalar-tint path.
    pub fn add_override(&mut self, hash: u64, asset_path: String) {
        self.add_override_ref(hash, asset_path, [0, 0, 0]);
    }
    /// Registers a live override with an authored reference color.
    pub fn add_override_ref(&mut self, hash: u64, asset_path: String, ref_rgb: [u8; 3]) {
        self.overrides.insert(
            hash,
            SubDef {
                asset: asset_path,
                ref_rgb,
            },
        );
    }
    /// Removes all live per-sprite overrides.
    pub fn clear_overrides(&mut self) {
        self.overrides.clear();
    }
    /// Returns the number of live per-sprite overrides.
    pub fn override_len(&self) -> usize {
        self.overrides.len()
    }

    /// Resolves sprite occurrences to HD substitution instructions.
    ///
    /// Priority is live override, then palette-specific `(hash, palette)`, then
    /// wildcard `hash`.
    pub fn resolve(&self, occurrences: &[SpriteOccurrence]) -> Vec<SpriteSub> {
        occurrences
            .iter()
            .filter_map(|occ| {
                let def = self
                    .overrides
                    .get(&occ.hash)
                    .or_else(|| self.pal_catalog.get(&(occ.hash, occ.palette)))
                    .or_else(|| self.catalog.get(&occ.hash))?;
                Some(SpriteSub {
                    asset_path: def.asset.clone(),
                    screen_x: occ.screen_x,
                    screen_y: occ.screen_y,
                    w_tiles: occ.w_tiles,
                    h_tiles: occ.h_tiles,
                    w_px: occ.w_tiles as u16 * 8,
                    h_px: occ.h_tiles as u16 * 8,
                    mirror: 0, // per-sprite: el flip lo deriva el motor de la occ
                    palette: occ.palette,
                    pose_key: pose_key_of(&[occ.hash]),
                    synth_pal: 0xFF,      // per-sprite: sin síntesis de paleta
                    ref_rgb: def.ref_rgb, //  ref autorada; [0,0,0] → peak-hold
                    u0: 0.0,
                    v0: 0.0,
                    uw: 1.0,
                    vh: 1.0,
                    mask_path: String::new(), // per-sprite: sin Vestuario
                })
            })
            .collect()
    }

    fn parse_toml(&mut self, s: &str) {
        #[derive(serde::Deserialize)]
        struct SubEntry {
            hash: String,
            asset: String,
            palette: Option<u8>,
            //  referencia cromática E1 "r,g,b" (mismo dialecto que [[pose]] ref).
            #[serde(rename = "ref")]
            ref_rgb: Option<String>,
        }
        #[derive(serde::Deserialize)]
        struct SubFile {
            #[serde(rename = "sub")]
            subs: Option<Vec<SubEntry>>,
        }

        if let Ok(file) = toml::from_str::<SubFile>(s) {
            for entry in file.subs.unwrap_or_default() {
                let hex = entry
                    .hash
                    .trim()
                    .trim_start_matches("0x")
                    .trim_start_matches("0X");
                if let Ok(hash) = u64::from_str_radix(hex, 16) {
                    let ref_rgb = entry
                        .ref_rgb
                        .as_deref()
                        .map(|r| {
                            let mut it = r.split(',').map(|x| x.trim().parse::<u8>().unwrap_or(0));
                            let mut out = [0u8; 3];
                            for c in &mut out {
                                *c = it.next().unwrap_or(0);
                            }
                            out
                        })
                        .unwrap_or([0, 0, 0]);
                    let def = SubDef {
                        asset: entry.asset,
                        ref_rgb,
                    };
                    match entry.palette {
                        // `palette = N` → variante por paleta (CU-AN-10). Sin el campo →
                        // comodín (cualquier paleta), el comportamiento histórico.
                        Some(p) => {
                            self.pal_catalog.insert((hash, p & 0x3), def);
                        }
                        None => {
                            self.catalog.insert(hash, def);
                        }
                    }
                }
            }
        }
    }
}

impl Default for SpriteSubstitutor {
    fn default() -> Self {
        Self::new()
    }
}

// ---------------------------------------------------------------------------
// PoseSetSubstitutor (CU-AN multi-sprite) — sustitución por FIRMA DE POSE. Una
// pose multi-sprite ANIMADA = un conjunto de hashes que sólo está presente en SU
// frame; cuando todos sus hashes aparecen (sin reclamar), se sustituye por UN HD en
// el bbox de los miembros y se reclaman (claim) para que el per-sprite no los
// re-sustituya. Distinto del metasprite (matchea por anim_group_id, ESTABLE entre
// frames → estático): acá la clave es el SET de hashes → anima por frame.
// ---------------------------------------------------------------------------
/// Authored variant key for a pose asset candidate.
///
/// A value of `-1` makes a palette or flip axis a wildcard. `slots` and `sig`
/// optionally identify stable palette content rather than only its line index.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct VariantKey {
    /// Palette line, or `-1` for any line.
    pub palette: i8,
    /// Horizontal-flip requirement, or `-1` for either state.
    pub hflip: i8,
    /// Vertical-flip requirement, or `-1` for either state.
    pub vflip: i8,
    /// Palette-slot mask used by the content signature.
    pub slots: u16,
    /// Stable palette-content signature; zero disables this axis.
    pub sig: u64,
}

impl Default for VariantKey {
    fn default() -> Self {
        VariantKey {
            palette: -1,
            hflip: -1,
            vflip: -1,
            slots: 0,
            sig: 0,
        }
    }
}

/// Computes a stable content signature for selected slots in one palette line.
///
/// The signature is xxHash3 over raw 9-bit R0–2/G3–5/B6–8 colors in slot order.
/// Callers provide a latched stable line so fades preserve identity. Authoring and
/// runtime use the same function to prevent signature drift.
pub fn palette_signature(cram_words: &[u16], line: u8, slots: u16) -> u64 {
    let mut buf = [0u8; 32];
    let mut n = 0usize;
    for s in 0..16usize {
        if slots & (1 << s) == 0 {
            continue;
        }
        let c = cram_words
            .get((line as usize & 3) * 16 + s)
            .copied()
            .unwrap_or(0)
            & 0x01FF;
        buf[n] = (c & 0xFF) as u8;
        buf[n + 1] = (c >> 8) as u8;
        n += 2;
    }
    xxh3_64(&buf[..n])
}

/// Relationship between a candidate signature and latched live palette content.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SigState {
    /// Candidate has no content signature.
    None,
    /// Candidate signature matches the latched palette.
    Match,
    /// Candidate signature conflicts with the latched palette.
    Mismatch,
    /// The palette has not remained stable long enough to compare.
    Unknown,
}

/// Result of selecting the closest authored variant.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct VariantResolve {
    /// Selected candidate index, or `-1` to use the default asset.
    pub index: i32, // candidato elegido (-1 = ninguno → default)
    /// Whether every discriminating axis matched exactly.
    pub exact: bool, // cubre el target sin sintetizar nada
    /// Whether the renderer must synthesize a horizontal mirror.
    pub apply_hflip: bool, // sintetizar espejo horizontal
    /// Whether the renderer must synthesize a vertical mirror.
    pub apply_vflip: bool, // sintetizar espejo vertical
    /// Whether the renderer must approximate a palette mismatch by tinting.
    pub palette_synth: bool, // paleta no matcheó → aproximar como tono
}

/// Selects the candidate closest to an observed palette and flip state.
///
/// Exact matches outrank wildcards, which outrank synthesized palette or flip
/// states. Palette differences carry more weight than flip differences, and ties
/// select the lower candidate index. No candidates yields index -1.
pub fn resolve_nearest_variant(t: VariantKey, cands: &[VariantKey]) -> VariantResolve {
    resolve_nearest_variant_sig(t, cands, &[])
}

/// Selects the closest candidate while also considering palette signatures.
///
/// `sigs` is parallel to `cands`; missing entries are treated as
/// [`SigState::None`]. A matching signature outranks palette index and wildcard
/// matches, while a mismatching signature is penalized below ordinary synthesis.
/// When no signatures are supplied, ordering matches [`resolve_nearest_variant`].
pub fn resolve_nearest_variant_sig(
    t: VariantKey,
    cands: &[VariantKey],
    sigs: &[SigState],
) -> VariantResolve {
    let mism = |cv: i8, tv: i8| cv != -1 && tv != -1 && cv != tv;
    let wild = |cv: i8, tv: i8| cv == -1 && tv != -1;
    let mut best = VariantResolve {
        index: -1,
        ..Default::default()
    };
    let mut best_cost = i32::MAX;
    for (i, c) in cands.iter().enumerate() {
        let pal_m = mism(c.palette, t.palette);
        let hf_m = mism(c.hflip, t.hflip);
        let vf_m = mism(c.vflip, t.vflip);
        let sig = sigs.get(i).copied().unwrap_or(SigState::None);
        let cost = if pal_m {
            200
        } else if wild(c.palette, t.palette) {
            20
        } else {
            0
        } + if hf_m {
            2
        } else if wild(c.hflip, t.hflip) {
            1
        } else {
            0
        } + if vf_m {
            2
        } else if wild(c.vflip, t.vflip) {
            1
        } else {
            0
        } + match sig {
            SigState::Match => 0,
            SigState::Mismatch => 400,
            SigState::None | SigState::Unknown => 50,
        };
        if cost < best_cost {
            best_cost = cost;
            best = VariantResolve {
                index: i as i32,
                exact: !pal_m && !hf_m && !vf_m,
                apply_hflip: hf_m,
                apply_vflip: vf_m,
                palette_synth: pal_m,
            };
        }
    }
    best
}

struct PoseEntry {
    hashes: Vec<u64>,
    /// Offsets relativos de cada miembro respecto del origen de la pose (paralelo a
    /// `hashes`). Some = pose INSTANCIADA: sólo matchea cuando los miembros aparecen
    /// en ESTAS posiciones relativas exactas (bbox 1:1, sin estirar; si los sprites
    /// derivan — plumas que flotan — no matchea y el original queda intacto).
    /// None = legacy por set de hashes (packs viejos).
    rel: Option<Vec<(i16, i16)>>,
    /// Tamaño del bbox de captura en px (0 = desconocido). GUARD del matching legacy:
    /// se rechaza el match si el bbox unión excede ~2× este tamaño → evita el "gigante"
    /// cuando una pose mal mapeada tiene hashes que aparecen DISPERSOS (plumas cayendo).
    max_w: u16,
    max_h: u16,
    /// Asset DEFAULT del elemento (paso 3 de la cascada). Se usa si no hay
    /// candidatos o si ninguno matchea.
    asset: String,
    /// Vestuario: máscara de tinte del asset BASE ("" = sin máscara). Sólo se
    /// adjunta al sub cuando el asset elegido ES el base (los candidatos de
    /// variante son recolor autorado a mano y no la llevan).
    mask: String,
    /// Candidatos por variante: (clave, asset). Vacío = sólo el
    /// default (comportamiento previo). Al matchear, se elige el más próximo a la
    /// variante OBSERVADA del ancla (paleta se matchea, flip se sintetiza).
    candidates: Vec<(VariantKey, String)>,
    /// Tamaño (w,h) en PX de cada miembro, paralelo a `hashes` (None = pose sin
    /// dims, pre-2026-07). La tolerancia off-screen de un miembro AUSENTE necesita
    /// SUS dims reales: adivinarlas con las del primer miembro visible juzgaba
    /// "parcialmente visible" a una cabeza 8×8 ya totalmente fuera de pantalla →
    /// el match caía en los bordes y el bbox del sub se engordaba mal.
    dims: Option<Vec<(i16, i16)>>,
    /// Cara en que está dibujado el ASSET respecto de la capturada (bit0 = espejo
    /// H · bit1 = V). El flip de presentación de Posar espeja el máster exportado
    /// → el HD se autora en ESA cara; el sub debe XORearlo al espejo del arreglo
    /// detectado o la dirección sale invertida (Run 02 de Tyris).
    base_mirror: u8,
    /// Referencia AUTORADA del tinte E1 (promedio RGB 0-255 de la línea CRAM de
    /// la pose al capturarla). [0,0,0] = sin referencia (poses pre-migración).
    ref_rgb: [u8; 3],
    ///  referencia E1 POR LÍNEA de paleta (0-3) para poses de paletas
    /// MIXTAS — cada quad de grupo tinta contra la ref de SU línea. [0,0,0] =
    /// línea sin ref (el grupo del ancla cae a `ref_rgb`; los demás, al
    /// peak-hold escalar de su línea).
    ref_line: [[u8; 3]; 4],
    ///  flips SAT observados por miembro al CAPTURAR (bit0 = hflip ·
    /// bit1 = vflip, paralelo a `hashes`). Desempata instancias PARCIALES
    /// geométricamente equivalentes en fase 2 por AGREEMENT
    /// (occ.flip == member_flip ^ arr_bits). None = pose legacy sin flips ->
    /// comportamiento previo (orden estable).
    flips: Option<Vec<u8>>,
}

/// Elección de asset por variante + síntesis pendiente. Además del
/// asset, reporta lo que el sub debe SINTETIZAR porque el candidato no cubre
/// la variante observada: `synth_pal` = paleta AUTORADA del candidato elegido
/// cuando difiere de la observada (el motor aproxima el color como TONO desde
/// la CRAM viva; 0xFF = sin síntesis de paleta) y `synth_mirror` = espejos a
/// aplicar (bit0 H · bit1 V). Antes estos flags se DESCARTABAN acá y la
/// síntesis nunca ocurría.
struct VariantChoice {
    asset: String,
    synth_pal: u8,
    synth_mirror: u8,
}

/// Asset a usar para una instancia, según la variante observada del ancla. Sin
/// candidatos → el default (sin síntesis). Con candidatos → el más próximo (o
/// el default si ninguno), con sus flags de síntesis. `sig_latch` =
/// firmas de contenido latcheadas por (línea, slots) — el estado de la firma
/// de cada candidato se evalúa contra la línea OBSERVADA del ancla.
fn choose_variant_asset(
    pose: &PoseEntry,
    anchor: &SpriteOccurrence,
    sig_latch: &HashMap<(u8, u16), u64>,
) -> VariantChoice {
    let default = || VariantChoice {
        asset: pose.asset.clone(),
        synth_pal: 0xFF,
        synth_mirror: 0,
    };
    if pose.candidates.is_empty() {
        return default();
    }
    let tgt = VariantKey {
        palette: anchor.palette as i8,
        hflip: anchor.hflip as i8,
        vflip: anchor.vflip as i8,
        ..Default::default()
    };
    let keys: Vec<VariantKey> = pose.candidates.iter().map(|(k, _)| *k).collect();
    let sigs: Vec<SigState> = keys
        .iter()
        .map(|k| {
            if k.sig == 0 {
                return SigState::None;
            }
            match sig_latch.get(&(anchor.palette & 3, k.slots)) {
                Some(&live) if live == k.sig => SigState::Match,
                Some(_) => SigState::Mismatch,
                None => SigState::Unknown,
            }
        })
        .collect();
    let r = resolve_nearest_variant_sig(tgt, &keys, &sigs);
    if r.index < 0 {
        return default();
    }
    let (key, asset) = &pose.candidates[r.index as usize];
    // Espejos a sintetizar. Ejes EXPLÍCITOS: lo que el resolver no pudo
    // matchear (apply_*). Ejes COMODÍN:
    // el asset está dibujado en la cara BASE de la pose (base_mirror) y "no
    // discrimina" = se presenta en la cara OBSERVADA del ancla → sintetizar
    // el XOR que falte. Sin esto, una instancia espejada que resolvía al base
    // comodín salía con la cara SIN invertir — inconsistente con el
    // auto-espejo por occ del canal per-sprite y con el camino sin candidatos
    // (arr_bits ^ base_mirror).
    let mut mirror = (r.apply_hflip as u8) | ((r.apply_vflip as u8) << 1);
    if key.hflip < 0 {
        mirror ^= (anchor.hflip & 1) ^ (pose.base_mirror & 1);
    }
    if key.vflip < 0 {
        mirror ^= ((anchor.vflip & 1) << 1) ^ (pose.base_mirror & 2);
    }
    VariantChoice {
        asset: asset.clone(),
        synth_pal: if r.palette_synth && key.palette >= 0 {
            key.palette as u8
        } else {
            0xFF
        },
        synth_mirror: mirror,
    }
}

/// Resolves whole multi-sprite poses before per-sprite fallback.
pub struct PoseSetSubstitutor {
    catalog: Vec<PoseEntry>,
    /// Overrides EN VIVO (preview de Animar): poses inyectadas desde el Lab sin pack.
    /// Se resuelven ANTES que el catálogo (prioridad) y no se tocan al cargar un pack.
    overrides: Vec<PoseEntry>,
    /// Área VISIBLE del display según el modo de video vivo (H32=256/H40=320 ×
    /// V28=224/V30=240). Límite de la tolerancia off-screen: un miembro ausente se
    /// tolera sólo si su rect esperado queda TOTALMENTE fuera de [0,w)×[0,h) —
    /// las occurrences desaparecen en el borde visible (el VDP no lo dibuja o el
    /// juego lo parquea al salir de cámara), no en la holgura 336/240 del hasher.
    /// El host lo actualiza por frame (ayther_pose_sub_set_screen, im.snap.w/h).
    screen_w: i16,
    screen_h: i16,
    //  identidad por CONTENIDO de paleta. La CRAM viva (64 words
    // empaquetadas) entra por set_cram() cada frame; se trackea la estabilidad
    // POR LÍNEA (≥30 frames sin cambios, criterio ) y las firmas de los
    // (línea, slots) que piden los candidatos se LATCHEAN en estado estable —
    // durante un fade la identidad vigente es la última estable.
    cram: [u16; 64],
    cram_stable: [u32; 4],
    /// Firmas latcheadas: (línea, slots_mask) → xxh3 del último estado estable.
    sig_latch: HashMap<(u8, u16), u64>,
    /// Máscaras de slots que algún candidato con firma usa (qué latchear).
    sig_masks: Vec<u16>,
}

///  frames de estabilidad de línea exigidos para computar/latchear firmas.
const SIG_STABLE_FRAMES: u32 = 30;

impl PoseSetSubstitutor {
    /// Creates an empty pose catalog using a 320×240 visible area.
    pub fn new() -> Self {
        Self {
            catalog: Vec::new(),
            overrides: Vec::new(),
            screen_w: 320,
            screen_h: 240,
            cram: [0; 64],
            cram_stable: [0; 4],
            sig_latch: HashMap::new(),
            sig_masks: Vec::new(),
        }
    }

    /// Updates live CRAM and refreshes palette signatures after stable periods.
    ///
    /// Call this before [`Self::resolve`] each frame.
    pub fn set_cram(&mut self, words: &[u16]) {
        if words.len() < 64 {
            return;
        }
        for l in 0..4usize {
            let changed =
                (0..16).any(|s| (words[l * 16 + s] & 0x01FF) != (self.cram[l * 16 + s] & 0x01FF));
            if changed {
                self.cram_stable[l] = 0;
            } else {
                self.cram_stable[l] = self.cram_stable[l].saturating_add(1);
            }
        }
        self.cram[..64].copy_from_slice(&words[..64]);
        for l in 0..4u8 {
            if self.cram_stable[l as usize] < SIG_STABLE_FRAMES {
                continue;
            }
            for &m in &self.sig_masks {
                self.sig_latch
                    .insert((l, m), palette_signature(&self.cram, l, m));
            }
        }
    }

    /// Registra la máscara de slots de un candidato con firma (qué latchear).
    fn note_sig_mask(&mut self, slots: u16) {
        if slots != 0 && !self.sig_masks.contains(&slots) {
            self.sig_masks.push(slots);
        }
    }

    /// Updates the visible native frame dimensions; zero values are ignored.
    pub fn set_screen(&mut self, w: u16, h: u16) {
        if w > 0 && h > 0 {
            self.screen_w = w as i16;
            self.screen_h = h as i16;
        }
    }

    /// ¿Un sprite de w×h tiles en (x,y) quedaría TOTALMENTE fuera del área
    /// visible? Sólo ahí la ausencia de un miembro es provablemente "salió por
    /// el borde" (uno parcialmente visible se dibuja y genera occurrence).
    fn member_invisible(&self, x: i16, y: i16, w_tiles: u8, h_tiles: u8) -> bool {
        x >= self.screen_w
            || y >= self.screen_h
            || x + (w_tiles as i16) * 8 <= 0
            || y + (h_tiles as i16) * 8 <= 0
    }

    /// Loads pose substitutions from `pose_substitutions.toml` in a pack.
    pub fn load_from_pack(&mut self, pack: &crate::archive_vfs::AyArchive) {
        self.catalog.clear();
        if let Some(data) = pack.read("pose_substitutions.toml")
            && let Ok(s) = std::str::from_utf8(&data)
        {
            self.parse_toml(s);
        }
    }

    /// Returns the number of authored pose entries.
    pub fn catalog_len(&self) -> usize {
        self.catalog.len()
    }

    /// Adds a live pose override with priority over the pack catalog.
    ///
    /// When present, `rel` is parallel to `hashes` and enables instantiated
    /// matching at exact relative offsets.
    pub fn add_override(
        &mut self,
        hashes: Vec<u64>,
        rel: Option<Vec<(i16, i16)>>,
        max_w: u16,
        max_h: u16,
        asset: String,
    ) {
        self.add_override_variants(
            hashes,
            rel,
            None,
            None,
            max_w,
            max_h,
            0,
            [0, 0, 0],
            [[0; 3]; 4],
            asset,
            String::new(),
            Vec::new(),
        );
    }

    /// Adds a live pose override with variant candidates.
    ///
    /// The closest candidate to the observed anchor variant is selected, falling
    /// back to `default_asset`; an empty candidate list always uses the default.
    /// `dims` supplies per-member pixel sizes for off-screen tolerance,
    /// `base_mirror` describes the asset orientation relative to the capture,
    /// `ref_rgb` is the authored E1 tint reference, and `ref_line` supplies
    /// per-palette-line references.
    #[allow(clippy::too_many_arguments)]
    pub fn add_override_variants(
        &mut self,
        hashes: Vec<u64>,
        rel: Option<Vec<(i16, i16)>>,
        dims: Option<Vec<(i16, i16)>>,
        flips: Option<Vec<u8>>,
        max_w: u16,
        max_h: u16,
        base_mirror: u8,
        ref_rgb: [u8; 3],
        ref_line: [[u8; 3]; 4],
        default_asset: String,
        mask: String,
        candidates: Vec<(VariantKey, String)>,
    ) {
        if hashes.is_empty() {
            return;
        }
        let rel = match rel {
            Some(r) if r.len() == hashes.len() => Some(r),
            _ => None, // rel corrupto/ausente → legacy por set
        };
        let dims = match dims {
            Some(d) if d.len() == hashes.len() => Some(d),
            _ => None, // dims corruptas/ausentes → fallback del frame vivo
        };
        let flips = match flips {
            Some(f) if f.len() == hashes.len() => Some(f),
            _ => None, // flips corruptos/ausentes → sin desempate
        };
        for (k, _) in &candidates {
            if k.sig != 0 {
                self.note_sig_mask(k.slots);
            }
        }
        self.overrides.push(PoseEntry {
            hashes,
            rel,
            max_w,
            max_h,
            asset: default_asset,
            mask,
            candidates,
            dims,
            base_mirror,
            ref_rgb,
            ref_line,
            flips,
        });
    }
    /// Removes all live pose overrides.
    pub fn clear_overrides(&mut self) {
        self.overrides.clear();
    }
    /// Returns the number of live pose overrides.
    pub fn override_len(&self) -> usize {
        self.overrides.len()
    }

    /// Resolves whole-pose substitutions and claims their source occurrences.
    ///
    /// Instantiated poses are handled in two phases: enumerate all candidates
    /// whose members are present at exact offsets or provably off-screen, then
    /// greedily assign claims by observed evidence. Ties prefer more-specific
    /// poses and live overrides. Each instance emits one [`SpriteSub`] using its
    /// captured bounding box. Legacy set-only poses resolve afterward from the
    /// remaining unclaimed occurrences.
    pub fn resolve(&self, occs: &[SpriteOccurrence], claimed: &mut [bool]) -> Vec<SpriteSub> {
        let mut subs = Vec::new();
        // Dims por hash desde el frame vivo (mismo hash = mismo gráfico = mismas
        // dims): los espejos EXACTOS por miembro las necesitan (abajo).
        let mut dims: HashMap<u64, (u8, u8)> = HashMap::new();
        for o in occs {
            dims.entry(o.hash).or_insert((o.w_tiles, o.h_tiles));
        }
        // Orden base (= desempate de la fase 2): la pose MÁS ESPECÍFICA (más
        // miembros) primero — si una pose chica es sub-arreglo de una grande,
        // a IGUAL evidencia la grande no debe perder miembros. Sort estable: a
        // igual cantidad, overrides (preview en vivo) siguen resolviendo antes
        // que el catálogo del pack.
        let mut order: Vec<&PoseEntry> = self.overrides.iter().chain(self.catalog.iter()).collect();
        order.sort_by_key(|p| std::cmp::Reverse(p.hashes.len()));
        // ---- Fase 1: enumerar instancias candidatas (sin reclamar) ----
        struct Cand<'p> {
            pose: &'p PoseEntry,
            arr_bits: u8,
            inst: Vec<usize>,                   // occs con hit REAL
            missing: Vec<(i16, i16, i16, i16)>, // ausentes tolerados (bbox)
            ///  miembros cuyo flip SAT observado coincide con el autorado
            /// XOR el espejo del arreglo (0 si la pose no trae flips).
            agree: u32,
        }
        let mut cands: Vec<Cand> = Vec::new();
        for &pose in &order {
            if pose.hashes.is_empty() {
                continue;
            }
            if let Some(rel) = &pose.rel {
                // INSTANCIADA: cada occurrence del miembro 0 propone un origen; la
                // instancia matchea si TODOS los miembros están en origen+rel[i] con
                // su hash, sin reclamar. Puede haber varias instancias por frame.
                //
                // Flip-aware: la dirección es un ESTADO de la misma pose, no
                // una pose nueva (el hash ya es flip-normalizado h+v). Se prueban los
                // CUATRO arreglos: el capturado y sus espejos horizontal, vertical y
                // h+v. El espejo es EXACTO por miembro: x' = Wpx − x − w_i (dims
                // reales del gráfico, no la aprox. de tamaño uniforme — con miembros
                // MIXTOS la aprox. corría los descentrados y la cara espejada nunca
                // matcheaba completa: los pies de Tyris, 16px en un bbox de 32).
                // Dims de un miembro sin occ este frame → las AUTORADAS de la pose
                // (pose.dims); sin ésas, las del primer miembro visto (fallback
                // uniforme — adivina, y una cabeza 8×8 totalmente fuera juzgada
                // "parcialmente visible" con dims de 32px rompía el match en los
                // bordes). Sólo pesa en la tolerancia off-screen y el bbox.
                let fb: (u8, u8) = pose
                    .hashes
                    .iter()
                    .find_map(|h| dims.get(h))
                    .copied()
                    .unwrap_or((1, 1));
                let mpx: Vec<(i16, i16)> = pose
                    .hashes
                    .iter()
                    .enumerate()
                    .map(|(i, h)| match dims.get(h) {
                        Some(&(w, hh)) => ((w as i16) * 8, (hh as i16) * 8),
                        None => pose
                            .dims
                            .as_ref()
                            .map(|d| d[i])
                            .unwrap_or_else(|| ((fb.0 as i16) * 8, (fb.1 as i16) * 8)),
                    })
                    .collect();
                let wpx = rel
                    .iter()
                    .zip(&mpx)
                    .map(|(r, m)| r.0 + m.0)
                    .max()
                    .unwrap_or(0);
                let hpx = rel
                    .iter()
                    .zip(&mpx)
                    .map(|(r, m)| r.1 + m.1)
                    .max()
                    .unwrap_or(0);
                let mir_h: Vec<(i16, i16)> = rel
                    .iter()
                    .zip(&mpx)
                    .map(|(r, m)| (wpx - r.0 - m.0, r.1))
                    .collect();
                let mir_v: Vec<(i16, i16)> = rel
                    .iter()
                    .zip(&mpx)
                    .map(|(r, m)| (r.0, hpx - r.1 - m.1))
                    .collect();
                let mir_hv: Vec<(i16, i16)> = rel
                    .iter()
                    .zip(&mpx)
                    .map(|(r, m)| (wpx - r.0 - m.0, hpx - r.1 - m.1))
                    .collect();
                // Arreglos ÚNICOS (poses simétricas colapsan → sin pasadas
                // redundantes), cada uno con sus bits de espejo (0=capturado,
                // bit0=H, bit1=V) — viajan al sub para que el render dibuje la
                // cara canónica pre-volteada en las instancias espejadas.
                let mut arrangements: Vec<(&[(i16, i16)], u8)> = vec![(rel.as_slice(), 0)];
                for (m, bits) in [
                    (mir_h.as_slice(), 1u8),
                    (mir_v.as_slice(), 2u8),
                    (mir_hv.as_slice(), 3u8),
                ] {
                    if !arrangements.iter().any(|(a, _)| *a == m) {
                        arrangements.push((m, bits));
                    }
                }
                for (arrangement, arr_bits) in arrangements {
                    let mut tried: Vec<(i16, i16)> = Vec::new();
                    for a in 0..occs.len() {
                        if claimed[a] {
                            continue;
                        }
                        // cualquier miembro puede anclar (el 0 podría estar tapado en SAT raro)
                        let Some(k) = pose.hashes.iter().position(|&h| h == occs[a].hash) else {
                            continue;
                        };
                        let ox = occs[a].screen_x - arrangement[k].0;
                        let oy = occs[a].screen_y - arrangement[k].1;
                        if tried.contains(&(ox, oy)) {
                            continue;
                        }
                        tried.push((ox, oy));
                        // Tolerancia off-screen: un miembro AUSENTE se acepta sólo si
                        // su rect esperado (origen + rel, con SUS dims — mpx[i]) queda
                        // TOTALMENTE fuera del área visible (member_invisible →
                        // provablemente invisible; el personaje saliendo por un
                        // borde). Cualquier miembro visible ancla, así que la salida
                        // por cualquiera de los 4 bordes conserva un pivote. Los
                        // ausentes NO se reclaman pero SÍ engordan el bbox (el HD
                        // mantiene la geometría 1:1 de la captura y se extiende fuera
                        // de pantalla; el render recorta).
                        let mut inst: Vec<usize> = Vec::with_capacity(pose.hashes.len());
                        let mut missing: Vec<(i16, i16, i16, i16)> = Vec::new();
                        let mut ok = true;
                        let mut agree: u32 = 0;
                        for (i, &h) in pose.hashes.iter().enumerate() {
                            let (wx, wy) = (ox + arrangement[i].0, oy + arrangement[i].1);
                            let (mw, mh) = mpx[i];
                            match occs.iter().enumerate().find(|(q, o)| {
                                !claimed[*q]
                                    && !inst.contains(q)
                                    && o.hash == h
                                    && o.screen_x == wx
                                    && o.screen_y == wy
                            }) {
                                Some((q, o)) => {
                                    inst.push(q);
                                    //  agreement de flips (poses con
                                    // flips autorados).
                                    if let Some(fl) = &pose.flips {
                                        let ob = (o.hflip & 1) | ((o.vflip & 1) << 1);
                                        if ob == (fl[i] ^ arr_bits) {
                                            agree += 1;
                                        }
                                    }
                                }
                                None if self.member_invisible(
                                    wx,
                                    wy,
                                    (mw / 8).max(1) as u8,
                                    (mh / 8).max(1) as u8,
                                ) =>
                                {
                                    missing.push((wx, wy, mw, mh))
                                }
                                None => {
                                    ok = false;
                                    break;
                                }
                            }
                        }
                        if !ok || inst.is_empty() {
                            continue;
                        }
                        cands.push(Cand {
                            pose,
                            arr_bits,
                            inst,
                            missing,
                            agree,
                        });
                    }
                }
            }
        }
        // ---- Fase 2: asignación greedy por evidencia ----
        // Hits reales DESC: una instancia con 6 occs verificadas pesa más que
        // una de 16 miembros con 1 solo visible. Sort estable → a igual hits
        // queda el orden de la fase 1 (más miembros, overrides primero).
        //  a IGUAL evidencia gana el arreglo cuyo flip SAT observado
        // coincide con los flips autorados (occ.flip == member_flip^arr_bits)
        // -> resuelve la cara ambigua de instancias PARCIALES al borde. Poses
        // sin flips: agree 0 uniforme = orden estable previo.
        cands.sort_by_key(|c| (std::cmp::Reverse(c.inst.len()), std::cmp::Reverse(c.agree)));
        for c in cands {
            // Algún miembro ya reclamado por una instancia con más evidencia →
            // esta candidata cae (las anclas alternativas de su pose ya fueron
            // enumeradas como candidatas propias en la fase 1).
            if c.inst.iter().any(|&i| claimed[i]) {
                continue;
            }
            let Cand {
                pose,
                arr_bits,
                inst,
                missing,
                ..
            } = c;
            let (mut x0, mut y0, mut x1, mut y1) = (i16::MAX, i16::MAX, i16::MIN, i16::MIN);
            for &i in &inst {
                let o = &occs[i];
                x0 = x0.min(o.screen_x);
                y0 = y0.min(o.screen_y);
                x1 = x1.max(o.screen_x.saturating_add((o.w_tiles as i16) * 8));
                y1 = y1.max(o.screen_y.saturating_add((o.h_tiles as i16) * 8));
            }
            for &(wx, wy, mw, mh) in &missing {
                x0 = x0.min(wx);
                y0 = y0.min(wy);
                x1 = x1.max(wx.saturating_add(mw));
                y1 = y1.max(wy.saturating_add(mh));
            }
            // Ancla = miembro 0 (hash[0]); su variante observada (incluido
            // el hflip de la cara espejada) elige el candidato (paso 2).
            let choice = choose_variant_asset(pose, &occs[inst[0]], &self.sig_latch);
            // Vestuario: sólo acompaña al asset BASE (decisión de producto
            // 2026-08-18) — un candidato de variante ya es recolor autorado.
            // Cubre tanto el default() del resolver como el base compitiendo
            // de comodín -1/-1/-1 (mismo asset id en ambos casos).
            let mask_of_choice = if !pose.mask.is_empty() && choice.asset == pose.asset {
                pose.mask.clone()
            } else {
                String::new()
            };
            for &i in &inst {
                claimed[i] = true;
            }
            let w = ((x1 - x0) / 8).clamp(1, 255) as u8;
            let h = ((y1 - y0) / 8).clamp(1, 255) as u8;
            // Con CANDIDATOS la cara la elige el candidato por la
            // variante observada del ancla; lo que el candidato NO
            // cubre se SINTETIZA. Sin candidatos,
            // el XOR con base_mirror corrige assets autorados sobre
            // el máster espejado (flip de Posar).
            let mirror = if pose.candidates.is_empty() {
                arr_bits ^ pose.base_mirror
            } else {
                choice.synth_mirror
            };
            // opción A: miembros en LÍNEAS de paleta distintas →
            // un quad POR GRUPO, cada uno con su porción UV del asset,
            // su línea y su ref — el tinte E1 (por sub) sigue cada
            // línea por separado. Grupos con solape >15% del área del
            // menor (miembros intercalados) → quad único del ancla
            // (comportamiento previo). El flip viaja pre-volteado en
            // la textura → la proporción en espacio de pantalla vale
            // para cualquier cara.
            let mut groups: Vec<(u8, i16, i16, i16, i16)> = Vec::new();
            for &i in &inst {
                let o = &occs[i];
                let (gx0, gy0) = (o.screen_x, o.screen_y);
                let gx1 = o.screen_x.saturating_add((o.w_tiles as i16) * 8);
                let gy1 = o.screen_y.saturating_add((o.h_tiles as i16) * 8);
                match groups.iter_mut().find(|g| g.0 == o.palette) {
                    Some(g) => {
                        g.1 = g.1.min(gx0);
                        g.2 = g.2.min(gy0);
                        g.3 = g.3.max(gx1);
                        g.4 = g.4.max(gy1);
                    }
                    None => groups.push((o.palette, gx0, gy0, gx1, gy1)),
                }
            }
            let mut disjoint = groups.len() > 1;
            'ov: for a2 in 0..groups.len() {
                for b2 in a2 + 1..groups.len() {
                    let (ga, gb) = (&groups[a2], &groups[b2]);
                    let iw = (ga.3.min(gb.3) - ga.1.max(gb.1)).max(0) as i32;
                    let ih = (ga.4.min(gb.4) - ga.2.max(gb.2)).max(0) as i32;
                    let aa = ((ga.3 - ga.1) as i32) * ((ga.4 - ga.2) as i32);
                    let ab = ((gb.3 - gb.1) as i32) * ((gb.4 - gb.2) as i32);
                    if iw * ih * 100 > aa.min(ab).max(1) * 15 {
                        disjoint = false;
                        break 'ov;
                    }
                }
            }
            // Ref de un grupo: la autorada de SU línea; el grupo del
            // ancla cae a la ref clásica de la pose; sin nada → [0,0,0]
            // (peak-hold escalar de su línea, comportamiento previo).
            let anchor_pal = occs[inst[0]].palette;
            let ref_of = |pal: u8| -> [u8; 3] {
                let r = pose.ref_line[(pal & 3) as usize];
                if r[0] | r[1] | r[2] != 0 {
                    r
                } else if pal == anchor_pal {
                    pose.ref_rgb
                } else {
                    [0, 0, 0]
                }
            };
            if disjoint {
                let (fw, fh) = ((x1 - x0).max(1) as f32, (y1 - y0).max(1) as f32);
                for g in &groups {
                    subs.push(SpriteSub {
                        asset_path: choice.asset.clone(),
                        screen_x: g.1,
                        screen_y: g.2,
                        w_tiles: ((g.3 - g.1) / 8).clamp(1, 255) as u8,
                        h_tiles: ((g.4 - g.2) / 8).clamp(1, 255) as u8,
                        w_px: (g.3 - g.1).max(1) as u16,
                        h_px: (g.4 - g.2).max(1) as u16,
                        mirror,
                        palette: g.0,
                        pose_key: pose_key_of(&pose.hashes),
                        synth_pal: choice.synth_pal,
                        ref_rgb: ref_of(g.0),
                        u0: (g.1 - x0) as f32 / fw,
                        v0: (g.2 - y0) as f32 / fh,
                        uw: (g.3 - g.1) as f32 / fw,
                        vh: (g.4 - g.2) as f32 / fh,
                        mask_path: mask_of_choice.clone(),
                    });
                }
            } else {
                subs.push(SpriteSub {
                    asset_path: choice.asset,
                    screen_x: x0,
                    screen_y: y0,
                    w_tiles: w,
                    h_tiles: h,
                    w_px: (x1 - x0).max(1) as u16, // bbox EXACTO (no tile-múltiplo)
                    h_px: (y1 - y0).max(1) as u16,
                    mirror,
                    palette: anchor_pal, // ancla = miembro 0
                    pose_key: pose_key_of(&pose.hashes),
                    synth_pal: choice.synth_pal,
                    ref_rgb: pose.ref_rgb,
                    u0: 0.0,
                    v0: 0.0,
                    uw: 1.0,
                    vh: 1.0,
                    mask_path: mask_of_choice,
                });
            }
        }
        // ---- LEGACY por set de hashes (packs sin rel), sobre lo restante ----
        for &pose in &order {
            if pose.hashes.is_empty() || pose.rel.is_some() {
                continue;
            }
            let want: HashSet<u64> = pose.hashes.iter().copied().collect();
            let members: Vec<usize> = occs
                .iter()
                .enumerate()
                .filter(|(i, o)| !claimed[*i] && want.contains(&o.hash))
                .map(|(i, _)| i)
                .collect();
            // Requiere que TODOS los hashes distintos de la pose estén presentes.
            let present: HashSet<u64> = members.iter().map(|&i| occs[i].hash).collect();
            if present.len() != want.len() {
                continue;
            }
            let (mut x0, mut y0, mut x1, mut y1) = (i16::MAX, i16::MAX, i16::MIN, i16::MIN);
            for &i in &members {
                let o = &occs[i];
                x0 = x0.min(o.screen_x);
                y0 = y0.min(o.screen_y);
                x1 = x1.max(o.screen_x.saturating_add((o.w_tiles as i16) * 8));
                y1 = y1.max(o.screen_y.saturating_add((o.h_tiles as i16) * 8));
            }
            // GUARD: rechazar el "gigante". Si los hashes aparecen DISPERSOS (pose mal
            // mapeada, p.ej. hashes que son plumas cayendo), el bbox unión excede por
            // mucho el tamaño de captura → no es la pose, se salta (queda el original).
            if pose.max_w > 0 && pose.max_h > 0 {
                let uw = (x1 - x0).max(0) as u32;
                let uh = (y1 - y0).max(0) as u32;
                if uw > (pose.max_w as u32) * 2 || uh > (pose.max_h as u32) * 2 {
                    continue;
                }
            }
            // Ancla = primer miembro presente; su variante elige el candidato.
            let choice = choose_variant_asset(pose, &occs[members[0]], &self.sig_latch);
            let mask_of_choice = if !pose.mask.is_empty() && choice.asset == pose.asset {
                pose.mask.clone()
            } else {
                String::new()
            };
            for &i in &members {
                claimed[i] = true;
            }
            let w = ((x1 - x0) / 8).clamp(1, 255) as u8;
            let h = ((y1 - y0) / 8).clamp(1, 255) as u8;
            subs.push(SpriteSub {
                asset_path: choice.asset,
                screen_x: x0,
                screen_y: y0,
                w_tiles: w,
                h_tiles: h,
                w_px: (x1 - x0).max(1) as u16, // bbox EXACTO (no tile-múltiplo)
                h_px: (y1 - y0).max(1) as u16,
                // Legacy sin arreglo detectable: sólo el espejo de síntesis del
                // candidato; sin candidatos queda 0 como antes.
                mirror: choice.synth_mirror,
                palette: occs[members[0]].palette, // ancla = primer miembro
                pose_key: pose_key_of(&pose.hashes),
                synth_pal: choice.synth_pal,
                ref_rgb: pose.ref_rgb,
                u0: 0.0,
                v0: 0.0,
                uw: 1.0,
                vh: 1.0, // legacy: quad completo
                mask_path: mask_of_choice,
            });
        }
        subs
    }

    /// `pose_substitutions.toml` del pack — modelo COMPLETO de pose (paridad
    /// con el canal en vivo del Lab): `rel`/`dims` = pares "x,y|x,y|…"
    /// paralelos a hashes (presentes → matching INSTANCIADO), `max_w/max_h` =
    /// guard anti-gigante del legacy, `flip` = cara del asset ("h"/"v"/"hv"),
    /// `ref` = referencia autorada del tinte E1 ("r,g,b" 0-255), y
    /// `[[pose.variant]]` = candidatos por variante (palette/hflip/vflip, -1 =
    /// cualquiera + asset). Todos opcionales → un pack viejo (hashes + asset)
    /// carga igual con la semántica legacy por set.
    fn parse_toml(&mut self, s: &str) {
        #[derive(serde::Deserialize)]
        struct VariantT {
            palette: Option<i8>,
            hflip: Option<i8>,
            vflip: Option<i8>,
            ///  identidad por contenido — slots marcados ("9|10|11") +
            /// firma xxh3 del contenido estable de esos slots ("0x…").
            slots: Option<String>,
            sig: Option<String>,
            asset: String,
        }
        #[derive(serde::Deserialize)]
        struct PoseT {
            hashes: Vec<String>,
            asset: String,
            rel: Option<String>,
            dims: Option<String>,
            ///  flips SAT por miembro al capturar — "f|f|..." (bit0 H,
            /// bit1 V), paralelo a hashes. Ausente = pack viejo.
            flips: Option<String>,
            max_w: Option<u16>,
            max_h: Option<u16>,
            flip: Option<String>,
            #[serde(rename = "ref")]
            ref_rgb: Option<String>,
            ///  refs E1 por línea de paleta — "L:r,g,b|L:r,g,b" (L = 0-3).
            refs: Option<String>,
            /// Vestuario: asset id de la máscara de tinte del BASE.
            mask: Option<String>,
            #[serde(rename = "variant")]
            variants: Option<Vec<VariantT>>,
        }
        #[derive(serde::Deserialize)]
        struct PoseFile {
            #[serde(rename = "pose")]
            poses: Option<Vec<PoseT>>,
        }
        fn parse_pairs(v: &str) -> Vec<(i16, i16)> {
            v.split('|')
                .filter_map(|one| {
                    let mut it = one.split(',');
                    let a = it.next()?.trim().parse::<i16>().ok()?;
                    let b = it.next()?.trim().parse::<i16>().ok()?;
                    Some((a, b))
                })
                .collect()
        }
        if let Ok(file) = toml::from_str::<PoseFile>(s) {
            for e in file.poses.unwrap_or_default() {
                let hashes: Vec<u64> = e
                    .hashes
                    .iter()
                    .filter_map(|p| {
                        let hex = p.trim().trim_start_matches("0x").trim_start_matches("0X");
                        u64::from_str_radix(hex, 16).ok()
                    })
                    .collect();
                if hashes.is_empty() {
                    continue;
                }
                let n = hashes.len();
                let rel = e.rel.as_deref().map(parse_pairs).filter(|r| r.len() == n);
                let dims = e.dims.as_deref().map(parse_pairs).filter(|d| d.len() == n);
                let flips = e
                    .flips
                    .as_deref()
                    .map(|v| {
                        v.split('|')
                            .map(|one| one.trim().parse::<u8>().unwrap_or(0) & 3)
                            .collect::<Vec<u8>>()
                    })
                    .filter(|f| f.len() == n);
                let base_mirror = e
                    .flip
                    .map(|f| (f.contains('h') as u8) | ((f.contains('v') as u8) << 1))
                    .unwrap_or(0);
                let ref_rgb = e
                    .ref_rgb
                    .as_deref()
                    .map(|r| {
                        let mut it = r.split(',').map(|x| x.trim().parse::<u8>().unwrap_or(0));
                        let mut out = [0u8; 3];
                        for c in &mut out {
                            *c = it.next().unwrap_or(0);
                        }
                        out
                    })
                    .unwrap_or([0, 0, 0]);
                let mut ref_line = [[0u8; 3]; 4];
                if let Some(rs) = e.refs.as_deref() {
                    for one in rs.split('|') {
                        let Some((l, rgb)) = one.split_once(':') else {
                            continue;
                        };
                        let Ok(l) = l.trim().parse::<usize>() else {
                            continue;
                        };
                        if l > 3 {
                            continue;
                        }
                        let mut it = rgb.split(',').map(|x| x.trim().parse::<u8>().unwrap_or(0));
                        for c in &mut ref_line[l] {
                            *c = it.next().unwrap_or(0);
                        }
                    }
                }
                let candidates: Vec<(VariantKey, String)> = e
                    .variants
                    .unwrap_or_default()
                    .into_iter()
                    .map(|v| {
                        let slots = v
                            .slots
                            .as_deref()
                            .map(|sl| {
                                sl.split('|')
                                    .filter_map(|x| x.trim().parse::<u8>().ok())
                                    .filter(|&x| x < 16)
                                    .fold(0u16, |m, x| m | (1 << x))
                            })
                            .unwrap_or(0);
                        let sig = v
                            .sig
                            .as_deref()
                            .map(|x| {
                                let hex =
                                    x.trim().trim_start_matches("0x").trim_start_matches("0X");
                                u64::from_str_radix(hex, 16).unwrap_or(0)
                            })
                            .unwrap_or(0);
                        (
                            VariantKey {
                                palette: v.palette.unwrap_or(-1),
                                hflip: v.hflip.unwrap_or(-1),
                                vflip: v.vflip.unwrap_or(-1),
                                slots,
                                sig,
                            },
                            v.asset,
                        )
                    })
                    .collect();
                for (k, _) in &candidates {
                    if k.sig != 0 {
                        self.note_sig_mask(k.slots);
                    }
                }
                self.catalog.push(PoseEntry {
                    hashes,
                    rel,
                    dims,
                    flips,
                    max_w: e.max_w.unwrap_or(0),
                    max_h: e.max_h.unwrap_or(0),
                    asset: e.asset,
                    mask: e.mask.unwrap_or_default(),
                    candidates,
                    base_mirror,
                    ref_rgb,
                    ref_line,
                });
            }
        }
    }
}

impl Default for PoseSetSubstitutor {
    fn default() -> Self {
        Self::new()
    }
}

// ---------------------------------------------------------------------------
// TweenPlayer (CU-AN in-betweens) — playback POR TIEMPO de los dibujos intermedios,
// disparado por el cambio de config. El juego cambia de pose A→B instantáneo; el HD
// quiere mostrar los intermedios (B1…Bn) en el TIEMPO antes de sostener B. Opera
// como FILTRO sobre el HD ya resuelto: dado el `target` (lo que la sustitución eligió
// este frame), cuando el target CAMBIA arranca la secuencia de intermedios de ese
// target y los emite por `TWEEN_TICKS` ticks cada uno; al terminar, sostiene el target.
// Desacoplado de la detección de pose: trabaja al nivel de asset.
// ---------------------------------------------------------------------------
const TWEEN_TICKS: u32 = 3; // ticks por frame intermedio (default)
const TRACK_RADIUS2: i64 = 48 * 48; // matching sub→track por centro (px²)
const TRACK_TTL: u64 = 30; // frames sin ver → expira (parpadeos del VDP)
const MAX_TRACKS: usize = 64;

#[derive(Clone)]
struct TweenSeq {
    frames: Vec<String>,
    ticks: u32,
}

/// Una INSTANCIA trackeada (personaje en pantalla): identidad = pose_key +
/// posición. `last_target` = último asset RESUELTO (post-sustitución,
/// pre-tween) — el `from` del próximo lookup; un tween interrumpido usa el
/// target anterior, nunca el intermedio en curso.
struct TweenTrack {
    pose_key: u64,
    cx: i64,
    cy: i64,
    last_target: String,
    frames: Vec<String>, // secuencia activa ([] = sin tween)
    idx: usize,
    timer: u32,
    ticks: u32,
    last_seen: u64,
}

/// Plays authored in-between frames for asset transitions.
///
/// Sequences are cataloged by exact `(from, target)` pairs or target wildcards
/// and tracked independently per on-screen instance. Live overrides take priority
/// over pack data. In-between frames replace the beginning of the target hold and
/// are interrupted if the target changes again.
pub struct TweenPlayer {
    pairs: HashMap<(String, String), TweenSeq>, // (from, target) exactos
    wildcard: HashMap<String, TweenSeq>,        // target → comodín
    ov_pairs: HashMap<(String, String), TweenSeq>, // canal vivo del Lab
    ov_wildcard: HashMap<String, TweenSeq>,
    tracks: Vec<TweenTrack>,
    frame: u64,
}

impl TweenPlayer {
    /// Creates an empty in-between catalog with no active instance tracks.
    pub fn new() -> Self {
        Self {
            pairs: HashMap::new(),
            wildcard: HashMap::new(),
            ov_pairs: HashMap::new(),
            ov_wildcard: HashMap::new(),
            tracks: Vec::new(),
            frame: 0,
        }
    }

    /// Loads transition sequences from `tween_sequences.toml` in a pack.
    pub fn load_from_pack(&mut self, pack: &crate::archive_vfs::AyArchive) {
        self.pairs.clear();
        self.wildcard.clear();
        self.clear_state();
        if let Some(data) = pack.read("tween_sequences.toml")
            && let Ok(s) = std::str::from_utf8(&data)
        {
            self.parse_toml(s);
        }
    }

    /// Returns the number of exact-pair and wildcard transition sequences.
    pub fn catalog_len(&self) -> usize {
        self.pairs.len() + self.wildcard.len()
    }

    /// Registers a live transition override; `None` makes `from` a wildcard.
    pub fn set_override(
        &mut self,
        from: Option<&str>,
        target: &str,
        frames: Vec<String>,
        ticks: u32,
    ) {
        if frames.is_empty() {
            return;
        }
        let seq = TweenSeq {
            frames,
            ticks: ticks.max(1),
        };
        match from {
            Some(f) => {
                self.ov_pairs
                    .insert((f.to_string(), target.to_string()), seq);
            }
            None => {
                self.ov_wildcard.insert(target.to_string(), seq);
            }
        }
    }
    /// Removes all live transition overrides.
    pub fn clear_overrides(&mut self) {
        self.ov_pairs.clear();
        self.ov_wildcard.clear();
    }

    /// Discards all per-instance playback state after a seek or source change.
    pub fn clear_state(&mut self) {
        self.tracks.clear();
        self.frame = 0;
    }

    /// Escalera: par exacto (vivo > pack) > comodín (vivo > pack) > None.
    fn lookup(&self, from: &str, target: &str) -> Option<&TweenSeq> {
        let key = (from.to_string(), target.to_string());
        self.ov_pairs
            .get(&key)
            .or_else(|| self.pairs.get(&key))
            .or_else(|| self.ov_wildcard.get(target))
            .or_else(|| self.wildcard.get(target))
    }

    /// Advances active transition timers and expires stale instance tracks.
    ///
    /// Call once per frame before resolving instances.
    pub fn begin_frame(&mut self) {
        self.frame += 1;
        let now = self.frame;
        self.tracks
            .retain(|t| now.saturating_sub(t.last_seen) <= TRACK_TTL);
        for t in &mut self.tracks {
            if t.idx < t.frames.len() {
                t.timer += 1;
                if t.timer >= t.ticks {
                    t.timer = 0;
                    t.idx += 1;
                    if t.idx >= t.frames.len() {
                        t.frames.clear();
                        t.idx = 0;
                    }
                }
            }
        }
    }

    /// Track más cercano al centro (radio TRACK_RADIUS2; desempate por
    /// pose_key igual). Sin candidato → track nuevo (cap: recicla el más viejo).
    fn find_track(&mut self, pose_key: u64, cx: i64, cy: i64) -> usize {
        let mut best: Option<(usize, i64, bool)> = None; // (idx, dist², same_key)
        for (i, t) in self.tracks.iter().enumerate() {
            let d = (t.cx - cx) * (t.cx - cx) + (t.cy - cy) * (t.cy - cy);
            if d > TRACK_RADIUS2 {
                continue;
            }
            let same = t.pose_key == pose_key;
            let better = match best {
                None => true,
                Some((_, bd, bsame)) => (same && !bsame) || (same == bsame && d < bd),
            };
            if better {
                best = Some((i, d, same));
            }
        }
        if let Some((i, _, _)) = best {
            return i;
        }
        if self.tracks.len() >= MAX_TRACKS {
            let oldest = self
                .tracks
                .iter()
                .enumerate()
                .min_by_key(|(_, t)| t.last_seen)
                .map(|(i, _)| i)
                .unwrap_or(0);
            self.tracks.remove(oldest);
        }
        self.tracks.push(TweenTrack {
            pose_key,
            cx,
            cy,
            last_target: String::new(),
            frames: Vec::new(),
            idx: 0,
            timer: 0,
            ticks: TWEEN_TICKS,
            last_seen: self.frame,
        });
        self.tracks.len() - 1
    }

    /// Resolves the asset to render for one instance on the current frame.
    ///
    /// `target` is the already resolved HD asset; `pose_key` and the bounding-box
    /// center identify the instance. The result is the active in-between frame or
    /// `target`. A transition starts only when the tracked pose identity changes,
    /// so palette variants within one pose do not trigger false transitions.
    pub fn resolve(&mut self, target: &str, pose_key: u64, cx: i64, cy: i64) -> String {
        let ti = self.find_track(pose_key, cx, cy);
        let changed_pose =
            self.tracks[ti].pose_key != pose_key && !self.tracks[ti].last_target.is_empty();
        let changed_target = self.tracks[ti].last_target != target;
        if changed_target {
            let seq = if changed_pose {
                self.lookup(&self.tracks[ti].last_target.clone(), target)
                    .cloned()
            } else {
                None
            };
            let t = &mut self.tracks[ti];
            match seq {
                Some(s) => {
                    t.frames = s.frames;
                    t.ticks = s.ticks;
                }
                None => t.frames.clear(), // pop directo (o misma pose: corta)
            }
            t.idx = 0;
            t.timer = 0;
            t.last_target = target.to_string();
        }
        let now = self.frame;
        let t = &mut self.tracks[ti];
        t.pose_key = pose_key;
        t.cx = cx;
        t.cy = cy;
        t.last_seen = now;
        if t.idx < t.frames.len() {
            t.frames[t.idx].clone()
        } else {
            target.to_string()
        }
    }

    fn parse_toml(&mut self, s: &str) {
        // v2: `from` (opcional, ausente = comodín) + `ticks` (opcional, 3).
        // El shape v1 (target + frames) sigue parseando como comodín a 3 ticks.
        #[derive(serde::Deserialize)]
        struct TweenT {
            target: String,
            from: Option<String>,
            frames: Vec<String>,
            ticks: Option<u32>,
        }
        #[derive(serde::Deserialize)]
        struct TweenFile {
            #[serde(rename = "tween")]
            tweens: Option<Vec<TweenT>>,
        }
        if let Ok(file) = toml::from_str::<TweenFile>(s) {
            for t in file.tweens.unwrap_or_default() {
                if t.frames.is_empty() {
                    continue;
                }
                let seq = TweenSeq {
                    frames: t.frames,
                    ticks: t.ticks.unwrap_or(TWEEN_TICKS).max(1),
                };
                match t.from {
                    Some(f) => {
                        self.pairs.insert((f, t.target), seq);
                    }
                    None => {
                        self.wildcard.insert(t.target, seq);
                    }
                }
            }
        }
    }
}

impl Default for TweenPlayer {
    fn default() -> Self {
        Self::new()
    }
}

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    /// Escribe un word del 68k en el layout del BUFFER del fork: little-endian
    /// (el buffer viene word-swapped en hosts LE; leer LE reconstruye el word
    /// big-endian del bus — la convención de process_vram / rd16 / vrd16).
    fn wr16(vram: &mut [u8], off: usize, w: u16) {
        vram[off] = (w & 0xFF) as u8;
        vram[off + 1] = (w >> 8) as u8;
    }

    /// Build a minimal 64KB VRAM buffer with one sprite entry in the SAT
    /// and its tile data (fork-buffer word layout).
    fn make_vram(
        tile_idx: usize,
        x: i16,
        y: i16,
        w: u8,
        h: u8,
        pattern: u8, // fill the tile(s) with this byte
    ) -> Vec<u8> {
        let mut vram = vec![0u8; 65536];

        // Fill tile data.
        for t in 0..(w as usize * h as usize) {
            let off = (tile_idx + t) * VRAM_TILE_BYTES;
            for b in &mut vram[off..off + VRAM_TILE_BYTES] {
                *b = pattern;
            }
        }

        // Write SAT entry 0 at SAT_BASE_H40 (4 words: Y · size|link · attr · X).
        let e = SAT_BASE_H40;
        wr16(&mut vram, e, (y + SPRITE_COORD_OFFSET) as u16 & 0x3FF);
        wr16(
            &mut vram,
            e + 2,
            (((h - 1) as u16) << 10) | (((w - 1) as u16) << 8),
        ); // link = 0
        wr16(&mut vram, e + 4, tile_idx as u16 & 0x7FF); // sin flips
        wr16(&mut vram, e + 6, (x + SPRITE_COORD_OFFSET) as u16 & 0x1FF);

        vram
    }

    /// Write an `n`-sprite link chain at `base`: slot i links to i+1, the last
    /// links to 0 (end of chain). All sprites are 1×1, on-screen, with distinct
    /// tile data (tile `1+i`, kept clear of both the SAT regions used in tests).
    fn write_chain(vram: &mut [u8], base: usize, n: usize) {
        for i in 0..n {
            let tile_idx = 1 + i;
            let toff = tile_idx * VRAM_TILE_BYTES;
            for b in &mut vram[toff..toff + VRAM_TILE_BYTES] {
                *b = 0x10 + i as u8;
            }

            let e = base + i * SAT_ENTRY_SIZE;
            let x = 32 + (i as i16) * 8; // spread out, all on visible screen
            let y = 48 + (i as i16) * 4;
            let link = if i + 1 < n { (i as u16) + 1 } else { 0 };
            wr16(vram, e, (y + SPRITE_COORD_OFFSET) as u16);
            wr16(vram, e + 2, link); // 1×1 sprite (size = 0)
            wr16(vram, e + 4, tile_idx as u16 & 0x7FF);
            wr16(vram, e + 6, (x + SPRITE_COORD_OFFSET) as u16);
        }
    }

    fn make_vram_chain(base: usize, n: usize) -> Vec<u8> {
        let mut v = vec![0u8; 65536];
        write_chain(&mut v, base, n);
        v
    }

    #[test]
    fn detects_sprite_at_position() {
        let vram = make_vram(0x100, 80, 100, 1, 1, 0xAB);
        let mut hasher = SpriteHasher::new();
        let new = hasher.process_vram(&vram, SAT_BASE_H40);
        assert_eq!(new, 1, "should see 1 new sprite");
        let occs = hasher.last_occurrences();
        assert_eq!(occs.len(), 1);
        assert_eq!(occs[0].screen_x, 80);
        assert_eq!(occs[0].screen_y, 100);
        assert_eq!(occs[0].w_tiles, 1);
        assert_eq!(occs[0].h_tiles, 1);
    }

    /// Fija el layout de la entrada SAT del Mega Drive (4 words) tal como la lee
    /// el hasher del buffer del fork (words LE): cada campo en sus bits exactos.
    /// Caza un corrimiento de bits/offset que el decode "feliz" no notaría.
    /// Construye una entrada con valores distintivos en TODOS los campos y
    /// verifica el decode completo.
    #[test]
    fn sat_byte_layout_decodes_all_fields() {
        let mut vram = vec![0u8; 65536];
        let tile_idx = 5u16; // sprite de 3×2 tiles desde tile 5
        let toff = tile_idx as usize * VRAM_TILE_BYTES;
        for b in &mut vram[toff..toff + VRAM_TILE_BYTES * 6] {
            *b = 0x3C;
        }

        let e = SAT_BASE_H40;
        wr16(&mut vram, e, (100i16 + SPRITE_COORD_OFFSET) as u16); // Y = 100
        // Word 1: vsize (11:10) = 1 → h=2 · hsize (9:8) = 2 → w=3 · link = 0.
        wr16(&mut vram, e + 2, (1u16 << 10) | (2u16 << 8));
        // Word 2: priority (15) = 1 · palette (14:13) = 2 · hflip (11) = 1 · tile.
        wr16(
            &mut vram,
            e + 4,
            (1u16 << 15) | (2u16 << 13) | (1u16 << 11) | tile_idx,
        );
        wr16(&mut vram, e + 6, (120i16 + SPRITE_COORD_OFFSET) as u16); // X = 120

        let mut h = SpriteHasher::new();
        assert_eq!(h.process_vram(&vram, SAT_BASE_H40), 1);
        let o = &h.last_occurrences()[0];
        assert_eq!(o.screen_x, 120, "X (word 3, offset -128)");
        assert_eq!(o.screen_y, 100, "Y (word 0, offset -128)");
        assert_eq!(o.w_tiles, 3, "width  = word1[9:8] + 1");
        assert_eq!(o.h_tiles, 2, "height = word1[11:10] + 1");
        assert_eq!(o.palette, 2, "palette = word2[14:13]");
        assert_eq!(o.priority, 1, "priority = word2[15]");
        assert_eq!(o.hflip, 1, "hflip = word2[11]");
        assert_eq!(o.vflip, 0, "vflip = word2[12]");
    }

    /// Espejo por VDP-hflip = MISMOS tiles → MISMO hash (flip-invariante). La
    /// identidad de pose y los arreglos espejados del matching dependen
    /// de esto; la versión previa hasheaba la APARIENCIA (con flips aplicados) y
    /// cada cara producía otro hash. El patrón es asimétrico a propósito: si el
    /// flip volviera a mezclarse en el hash, el test falla.
    #[test]
    fn same_tiles_flipped_same_hash() {
        let mut vram = vec![0u8; 65536];
        let tile_idx = 0x100usize;
        let toff = tile_idx * VRAM_TILE_BYTES;
        for (i, b) in vram[toff..toff + VRAM_TILE_BYTES].iter_mut().enumerate() {
            *b = (i as u8).wrapping_mul(37).wrapping_add(1); // asimétrico h y v
        }
        let e = SAT_BASE_H40;
        wr16(&mut vram, e, (50i16 + SPRITE_COORD_OFFSET) as u16); // cara original
        wr16(&mut vram, e + 2, 0);
        wr16(&mut vram, e + 4, tile_idx as u16);
        wr16(&mut vram, e + 6, (60i16 + SPRITE_COORD_OFFSET) as u16);
        let e2 = e + SAT_ENTRY_SIZE; // espejo h+v
        wr16(&mut vram, e2, (50i16 + SPRITE_COORD_OFFSET) as u16);
        wr16(&mut vram, e2 + 2, 0);
        wr16(
            &mut vram,
            e2 + 4,
            (1u16 << 12) | (1u16 << 11) | tile_idx as u16,
        );
        wr16(&mut vram, e2 + 6, (120i16 + SPRITE_COORD_OFFSET) as u16);

        let mut h = SpriteHasher::new();
        assert_eq!(
            h.process_vram(&vram, SAT_BASE_H40),
            1,
            "misma identidad → 1 solo hash nuevo en el catálogo"
        );
        let occs = h.last_occurrences();
        assert_eq!(occs.len(), 2);
        assert_eq!(
            occs[0].hash, occs[1].hash,
            "la cara espejada comparte hash con la original"
        );
        assert_eq!(occs[1].hflip, 1, "la orientación viaja en la occurrence");
        assert_eq!(occs[1].vflip, 1);
    }

    #[test]
    fn same_sprite_different_position_same_hash() {
        let vram1 = make_vram(0x100, 50, 50, 1, 1, 0xCC);
        let vram2 = make_vram(0x100, 150, 200, 1, 1, 0xCC);
        let mut h = SpriteHasher::new();
        h.process_vram(&vram1, SAT_BASE_H40);
        let hash1 = h.last_occurrences()[0].hash;
        h.process_vram(&vram2, SAT_BASE_H40);
        let hash2 = h.last_occurrences()[0].hash;
        assert_eq!(
            hash1, hash2,
            "same pattern at different position must hash identically"
        );
        assert_eq!(
            h.unique_sprite_count(),
            1,
            "should still be 1 unique sprite"
        );
    }

    #[test]
    fn different_pattern_different_hash() {
        let vram1 = make_vram(0x100, 50, 50, 1, 1, 0xAA);
        let vram2 = make_vram(0x100, 50, 50, 1, 1, 0xBB);
        let mut h1 = SpriteHasher::new();
        let mut h2 = SpriteHasher::new();
        h1.process_vram(&vram1, SAT_BASE_H40);
        h2.process_vram(&vram2, SAT_BASE_H40);
        assert_ne!(
            h1.last_occurrences()[0].hash,
            h2.last_occurrences()[0].hash,
            "different patterns must have different hashes"
        );
    }

    #[test]
    fn sprite_substitutor_resolves() {
        let vram = make_vram(0x100, 80, 80, 1, 1, 0xDE);
        let mut hasher = SpriteHasher::new();
        hasher.process_vram(&vram, SAT_BASE_H40);
        let hash = hasher.last_occurrences()[0].hash;

        let mut sub = SpriteSubstitutor::new();
        sub.add_override(hash, "hd/sprites/sonic.png".into());
        let result = sub.resolve(hasher.last_occurrences());
        assert_eq!(result.len(), 1);
        assert_eq!(result[0].asset_path, "hd/sprites/sonic.png");
        assert_eq!(result[0].screen_x, 80);
        assert_eq!(result[0].screen_y, 80);
    }

    #[test]
    fn sprite_substitutor_palette_variants() {
        // CU-AN-10: comodín (cualquier paleta) + variante por paleta. Prioridad:
        // override > (hash,paleta) > comodín. Sin comodín y sin variante de esa
        // paleta → no sustituye.
        let mk = |hash: u64, palette: u8| SpriteOccurrence {
            hash,
            anim_group_id: 0,
            w_tiles: 1,
            h_tiles: 1,
            screen_x: 10,
            screen_y: 20,
            link: 0,
            palette,
            priority: 0,
            slot: 0,
            hflip: 0,
            vflip: 0,
        };
        let mut sub = SpriteSubstitutor::new();
        sub.parse_toml(concat!(
            "[[sub]]\nhash = \"0x00000000000000aa\"\nasset = \"wild.png\"\n\n",
            "[[sub]]\nhash = \"0x00000000000000aa\"\nasset = \"p1.png\"\npalette = 1\n\n",
            "[[sub]]\nhash = \"0x00000000000000bb\"\nasset = \"onlyp2.png\"\npalette = 2\n",
        ));
        // hash aa, paleta 1 → la variante gana sobre el comodín
        assert_eq!(sub.resolve(&[mk(0xaa, 1)])[0].asset_path, "p1.png");
        // hash aa, paleta 0 → cae al comodín
        assert_eq!(sub.resolve(&[mk(0xaa, 0)])[0].asset_path, "wild.png");
        // hash bb, paleta 2 → variante; paleta 0 → sin sub (no hay comodín)
        assert_eq!(sub.resolve(&[mk(0xbb, 2)])[0].asset_path, "onlyp2.png");
        assert_eq!(sub.resolve(&[mk(0xbb, 0)]).len(), 0);
    }

    #[test]
    fn sprite_substitutor_carries_ref_rgb() {
        //  la ref cromática E1 viaja por los TRES caminos del canal
        // per-sprite — [[sub]] comodín, [[sub]] con paleta, y override en vivo.
        let mk = |hash: u64, palette: u8| SpriteOccurrence {
            hash,
            anim_group_id: 0,
            w_tiles: 1,
            h_tiles: 1,
            screen_x: 10,
            screen_y: 20,
            link: 0,
            palette,
            priority: 0,
            slot: 0,
            hflip: 0,
            vflip: 0,
        };
        let mut sub = SpriteSubstitutor::new();
        sub.parse_toml(concat!(
            "[[sub]]\nhash = \"0x00000000000000aa\"\nasset = \"wild.png\"\nref = \"165,121,92\"\n\n",
            "[[sub]]\nhash = \"0x00000000000000aa\"\nasset = \"p1.png\"\npalette = 1\nref = \"40,80,120\"\n\n",
            "[[sub]]\nhash = \"0x00000000000000bb\"\nasset = \"noref.png\"\n",
        ));
        // Comodín con ref y variante por paleta con SU ref.
        assert_eq!(sub.resolve(&[mk(0xaa, 0)])[0].ref_rgb, [165, 121, 92]);
        assert_eq!(sub.resolve(&[mk(0xaa, 1)])[0].ref_rgb, [40, 80, 120]);
        // Sin campo ref → [0,0,0] (peak-hold gris legacy).
        assert_eq!(sub.resolve(&[mk(0xbb, 3)])[0].ref_rgb, [0, 0, 0]);
        // Override en vivo con ref gana y la transporta.
        sub.add_override_ref(0xbb, "live.png".into(), [10, 20, 30]);
        let r = sub.resolve(&[mk(0xbb, 3)]);
        assert_eq!(r[0].asset_path, "live.png");
        assert_eq!(r[0].ref_rgb, [10, 20, 30]);
        // Override legacy (sin ref) → [0,0,0].
        sub.add_override(0xaa, "legacy.png".into());
        assert_eq!(sub.resolve(&[mk(0xaa, 0)])[0].ref_rgb, [0, 0, 0]);
    }

    #[test]
    fn pose_set_substitutor_matches_full_set_and_claims() {
        // CU-AN multi-sprite: una pose = set de hashes; sólo sustituye cuando TODOS
        // están presentes, en el bbox, reclamando los miembros. Frame distinto (set
        // distinto) → no matchea esta pose (cada keyframe tiene su propia firma).
        let mk = |hash: u64, x: i16, y: i16| SpriteOccurrence {
            hash,
            anim_group_id: 0,
            w_tiles: 2,
            h_tiles: 2,
            screen_x: x,
            screen_y: y,
            link: 0,
            palette: 0,
            priority: 0,
            slot: 0,
            hflip: 0,
            vflip: 0,
        };
        let mut sub = PoseSetSubstitutor::new();
        sub.parse_toml("[[pose]]\nhashes = [\"0xaa\", \"0xbb\"]\nasset = \"pose.png\"\n");
        // Ambos presentes → 1 sub en el bbox + ambos reclamados.
        let occs = vec![mk(0xaa, 10, 20), mk(0xbb, 26, 20)];
        let mut claimed = vec![false; occs.len()];
        let subs = sub.resolve(&occs, &mut claimed);
        assert_eq!(subs.len(), 1);
        assert_eq!(subs[0].asset_path, "pose.png");
        assert_eq!(subs[0].screen_x, 10);
        assert!(claimed[0] && claimed[1]);
        // Sólo uno presente → no matchea (pose incompleta).
        let occs2 = vec![mk(0xaa, 10, 20)];
        let mut c2 = vec![false];
        assert_eq!(sub.resolve(&occs2, &mut c2).len(), 0);
    }

    #[test]
    fn pose_set_live_override_resolves_and_wins_over_catalog() {
        // Preview de Animar: una pose-override EN VIVO (sin pack) se resuelve como
        // pose-set (UN HD sobre el bbox combinado, reclama miembros) y tiene
        // prioridad sobre el catálogo del pack. clear_overrides no toca el catálogo.
        let mk = |hash: u64, x: i16, y: i16| SpriteOccurrence {
            hash,
            anim_group_id: 0,
            w_tiles: 2,
            h_tiles: 2,
            screen_x: x,
            screen_y: y,
            link: 0,
            palette: 0,
            priority: 0,
            slot: 0,
            hflip: 0,
            vflip: 0,
        };
        let mut sub = PoseSetSubstitutor::new();
        // Catálogo del pack: la misma firma apunta a otro asset.
        sub.parse_toml("[[pose]]\nhashes = [\"0xaa\", \"0xbb\"]\nasset = \"pack.png\"\n");
        // Override en vivo (multi-sprite): gana y cubre el bbox combinado (10..42).
        sub.add_override(vec![0xaa, 0xbb], None, 0, 0, "live.png".to_string());
        assert_eq!(sub.override_len(), 1);
        let occs = vec![mk(0xaa, 10, 20), mk(0xbb, 34, 20)];
        let mut claimed = vec![false; occs.len()];
        let subs = sub.resolve(&occs, &mut claimed);
        // Un solo sub (el override reclama ambos; el catálogo ya no los ve).
        assert_eq!(subs.len(), 1);
        assert_eq!(subs[0].asset_path, "live.png"); // prioridad sobre el pack
        assert_eq!(subs[0].screen_x, 10); // bbox combinado
        assert_eq!(subs[0].w_tiles, ((34 + 16 - 10) / 8) as u8);
        assert!(claimed[0] && claimed[1]); // ambos miembros reclamados
        // Al limpiar el override, el catálogo del pack vuelve a resolver.
        sub.clear_overrides();
        let mut c2 = vec![false; occs.len()];
        let subs2 = sub.resolve(&occs, &mut c2);
        assert_eq!(subs2.len(), 1);
        assert_eq!(subs2[0].asset_path, "pack.png");
    }

    #[test]
    fn pose_set_instanced_override_matches_exact_offsets_only() {
        // Preview instanciado (rel): la pose sólo matchea cuando los miembros están
        // en los offsets relativos EXACTOS de la captura (bbox 1:1). Si los sprites
        // derivan (plumas que flotan), NO matchea → el original queda intacto (nada
        // de snapshots estirados sobre un bbox unión deforme).
        let mk = |hash: u64, x: i16, y: i16| SpriteOccurrence {
            hash,
            anim_group_id: 0,
            w_tiles: 1,
            h_tiles: 1,
            screen_x: x,
            screen_y: y,
            link: 0,
            palette: 0,
            priority: 0,
            slot: 0,
            hflip: 0,
            vflip: 0,
        };
        let mut sub = PoseSetSubstitutor::new();
        // Pose de 2 miembros: 0xbb a (16,0) del origen de 0xaa.
        sub.add_override(
            vec![0xaa, 0xbb],
            Some(vec![(0, 0), (16, 0)]),
            0,
            0,
            "pose.png".to_string(),
        );
        // Instancia exacta en (100,50) → 1 sub, bbox = captura (24x8).
        let occs = vec![mk(0xaa, 100, 50), mk(0xbb, 116, 50)];
        let mut c = vec![false; 2];
        let subs = sub.resolve(&occs, &mut c);
        assert_eq!(subs.len(), 1);
        assert_eq!((subs[0].screen_x, subs[0].screen_y), (100, 50));
        assert_eq!((subs[0].w_tiles, subs[0].h_tiles), (3, 1)); // 24px = 3 tiles
        assert!(c[0] && c[1]);
        // Miembros DERIVADOS (0xbb se movió) → NO matchea, nada reclamado.
        let occs2 = vec![mk(0xaa, 100, 50), mk(0xbb, 130, 70)];
        let mut c2 = vec![false; 2];
        assert_eq!(sub.resolve(&occs2, &mut c2).len(), 0);
        assert!(!c2[0] && !c2[1]);
        // DOS instancias completas → 2 subs (una por instancia).
        let occs3 = vec![
            mk(0xaa, 10, 10),
            mk(0xbb, 26, 10),
            mk(0xaa, 200, 90),
            mk(0xbb, 216, 90),
        ];
        let mut c3 = vec![false; 4];
        let subs3 = sub.resolve(&occs3, &mut c3);
        assert_eq!(subs3.len(), 2);
    }

    #[test]
    fn pose_set_legacy_bbox_guard_rejects_scattered_giant() {
        // Pose LEGACY (sin rel) de 2 hashes, capturada en un bbox chico (max 24x16).
        // Si sus hashes aparecen DISPERSOS (pose mal mapeada), el bbox unión es enorme
        // → el guard rechaza el match (no se estira el snapshot a un gigante).
        let mk = |hash: u64, x: i16, y: i16| SpriteOccurrence {
            hash,
            anim_group_id: 0,
            w_tiles: 1,
            h_tiles: 1,
            screen_x: x,
            screen_y: y,
            link: 0,
            palette: 0,
            priority: 0,
            slot: 0,
            hflip: 0,
            vflip: 0,
        };
        let mut sub = PoseSetSubstitutor::new();
        sub.add_override(vec![0xaa, 0xbb], None, 24, 16, "pose.png".to_string());
        // JUNTOS (dentro de ~2× el bbox) → matchea.
        let near = vec![mk(0xaa, 100, 50), mk(0xbb, 108, 50)];
        let mut c1 = vec![false; 2];
        assert_eq!(sub.resolve(&near, &mut c1).len(), 1);
        // DISPERSOS (bbox unión gigante) → NO matchea, nada reclamado.
        let far = vec![mk(0xaa, 100, 50), mk(0xbb, 280, 200)];
        let mut c2 = vec![false; 2];
        assert_eq!(sub.resolve(&far, &mut c2).len(), 0);
        assert!(!c2[0] && !c2[1]);
    }

    #[test]
    fn sprite_sub_carries_anchor_palette() {
        // E1 (fundido): el sub lleva la paleta del MIEMBRO ancla, para que el
        // motor no adivine con "primera occ del bbox" — un ajeno solapado
        // (Tyris montada, paleta 0, dentro del bbox del Dragón paleta 2) daba
        // la paleta equivocada y el flash nunca modulaba el HD (f1478).
        let mk = |hash: u64, x: i16, y: i16, pal: u8| SpriteOccurrence {
            hash,
            anim_group_id: 0,
            w_tiles: 1,
            h_tiles: 1,
            screen_x: x,
            screen_y: y,
            link: 0,
            palette: pal,
            priority: 0,
            slot: 0,
            hflip: 0,
            vflip: 0,
        };
        // Pose INSTANCIADA: el ajeno (paleta 0) va primero en el orden de occs
        // y su centro cae dentro del bbox de la pose (miembros paleta 2).
        let mut sub = PoseSetSubstitutor::new();
        sub.add_override(
            vec![0xaa, 0xbb],
            Some(vec![(0, 0), (16, 0)]),
            0,
            0,
            "pose.png".to_string(),
        );
        let occs = vec![
            mk(0x99, 104, 50, 0),
            mk(0xaa, 100, 50, 2),
            mk(0xbb, 116, 50, 2),
        ];
        let mut c = vec![false; 3];
        let subs = sub.resolve(&occs, &mut c);
        assert_eq!(subs.len(), 1);
        assert_eq!(subs[0].palette, 2);
        // Pose LEGACY (sin rel): ancla = primer miembro presente.
        let mut leg = PoseSetSubstitutor::new();
        leg.add_override(vec![0xcc], None, 0, 0, "l.png".to_string());
        let occs2 = vec![mk(0xcc, 10, 10, 3)];
        let mut c2 = vec![false; 1];
        let s2 = leg.resolve(&occs2, &mut c2);
        assert_eq!(s2.len(), 1);
        assert_eq!(s2[0].palette, 3);
        // Per-sprite: la paleta de SU occ.
        let mut ps = SpriteSubstitutor::new();
        ps.add_override(0xdd, "d.png".to_string());
        let s3 = ps.resolve(&[mk(0xdd, 5, 5, 1)]);
        assert_eq!(s3.len(), 1);
        assert_eq!(s3[0].palette, 1);
    }

    #[test]
    fn pose_pack_toml_carries_full_model() {
        // El pack de Entregar transporta el modelo COMPLETO de pose: rel/dims
        // (matching instanciado), guard, flip (cara del asset), ref (tinte E1)
        // y candidatos por variante — paridad con el canal en vivo del Lab.
        let mk = |hash: u64, x: i16, y: i16, pal: u8, hf: u8| SpriteOccurrence {
            hash,
            anim_group_id: 0,
            w_tiles: 1,
            h_tiles: 1,
            screen_x: x,
            screen_y: y,
            link: 0,
            palette: pal,
            priority: 0,
            slot: 0,
            hflip: hf,
            vflip: 0,
        };
        let mut sub = PoseSetSubstitutor::new();
        sub.parse_toml(concat!(
            "[[pose]]\n",
            "hashes = [\"0xaa\", \"0xbb\"]\n",
            "rel = \"0,0|16,0\"\n",
            "dims = \"8,8|8,8\"\n",
            "max_w = 24\nmax_h = 8\n",
            "flip = \"h\"\n",
            "ref = \"165,121,92\"\n",
            "asset = \"base.png\"\n",
            "[[pose.variant]]\n",
            "palette = 1\nhflip = -1\nvflip = -1\n",
            "asset = \"p1.png\"\n\n",
            // Entrada LEGACY (pack viejo): hashes + asset, sin nada más.
            "[[pose]]\nhashes = [\"0xcc\"]\nasset = \"old.png\"\n"
        ));
        assert_eq!(sub.catalog_len(), 2);
        // Instancia exacta (arreglo capturado, ancla paleta 1) → gana el
        // candidato de paleta 1 (exacto en paleta, flips -1 = cualquiera).
        let occs = vec![mk(0xaa, 100, 50, 1, 0), mk(0xbb, 116, 50, 1, 0)];
        let mut c = vec![false; 2];
        let subs = sub.resolve(&occs, &mut c);
        assert_eq!(subs.len(), 1);
        assert_eq!(subs[0].asset_path, "p1.png");
        assert_eq!(subs[0].palette, 1);
        assert_eq!(subs[0].ref_rgb, [165, 121, 92]);
        assert_eq!(subs[0].synth_pal, 0xFF); // paleta matcheó → sin tono
        // Miembros DERIVADOS → el rel del pack activa el matching instanciado
        // (el legacy por set habría matcheado igual → sería un gigante).
        let far = vec![mk(0xaa, 100, 50, 1, 0), mk(0xbb, 180, 90, 1, 0)];
        let mut c2 = vec![false; 2];
        assert_eq!(sub.resolve(&far, &mut c2).len(), 0);
        // Legacy del mismo pack: sigue resolviendo por set.
        let leg = vec![mk(0xcc, 10, 10, 2, 0)];
        let mut c3 = vec![false; 1];
        let s3 = sub.resolve(&leg, &mut c3);
        assert_eq!(s3.len(), 1);
        assert_eq!(s3[0].asset_path, "old.png");
        assert_eq!(s3[0].ref_rgb, [0, 0, 0]); // sin ref → peak-hold gris
    }

    #[test]
    fn pose_sub_reports_synthesis_and_ref() {
        //  cuando el candidato elegido NO cubre la variante observada, el
        // sub reporta la síntesis pendiente — synth_pal (paleta AUTORADA del
        // candidato, para el tinte) y mirror (flips a aplicar) — y lleva la
        // referencia autorada ref_rgb. Antes los flags se descartaban.
        let mk = |hash: u64, x: i16, y: i16, pal: u8, hf: u8| SpriteOccurrence {
            hash,
            anim_group_id: 0,
            w_tiles: 1,
            h_tiles: 1,
            screen_x: x,
            screen_y: y,
            link: 0,
            palette: pal,
            priority: 0,
            slot: 0,
            hflip: hf,
            vflip: 0,
        };
        let mut sub = PoseSetSubstitutor::new();
        // Único candidato: paleta 1, cara base. Observado: paleta 2, hflip → se
        // sintetizan TONO (synth_pal=1) y espejo H (mirror bit0).
        sub.add_override_variants(
            vec![0xaa],
            Some(vec![(0, 0)]),
            None,
            None,
            0,
            0,
            0,
            [120, 90, 60],
            [[0; 3]; 4],
            "default.png".to_string(),
            String::new(),
            vec![(
                VariantKey {
                    palette: 1,
                    hflip: 0,
                    vflip: 0,
                    ..Default::default()
                },
                "cand.png".to_string(),
            )],
        );
        let occs = vec![mk(0xaa, 50, 50, 2, 1)];
        let mut c = vec![false; 1];
        let subs = sub.resolve(&occs, &mut c);
        assert_eq!(subs.len(), 1);
        assert_eq!(subs[0].asset_path, "cand.png");
        assert_eq!(subs[0].synth_pal, 1);
        assert_eq!(subs[0].mirror, 1); // espejo H sintetizado
        assert_eq!(subs[0].ref_rgb, [120, 90, 60]); // referencia autorada
        // Candidato EXACTO (paleta 2, hflip 1) → sin síntesis.
        let mut ex = PoseSetSubstitutor::new();
        ex.add_override_variants(
            vec![0xbb],
            Some(vec![(0, 0)]),
            None,
            None,
            0,
            0,
            0,
            [0, 0, 0],
            [[0; 3]; 4],
            "default.png".to_string(),
            String::new(),
            vec![(
                VariantKey {
                    palette: 2,
                    hflip: 1,
                    vflip: 0,
                    ..Default::default()
                },
                "exact.png".to_string(),
            )],
        );
        let occs2 = vec![mk(0xbb, 50, 50, 2, 1)];
        let mut c2 = vec![false; 1];
        let s2 = ex.resolve(&occs2, &mut c2);
        assert_eq!(s2[0].asset_path, "exact.png");
        assert_eq!(s2[0].synth_pal, 0xFF);
        assert_eq!(s2[0].mirror, 0);
    }

    #[test]
    fn nearest_variant_matches_palette_synthesizes_flip() {
        // Paso 2: la PALETA se matchea, el FLIP se sintetiza. Espeja el oráculo
        // C++ lab_pose_variant_tests.
        let vk = |p: i8, h: i8, v: i8| VariantKey {
            palette: p,
            hflip: h,
            vflip: v,
            ..Default::default()
        };
        // flip+paleta, dos candidatos → gana el de la paleta, se sintetiza el flip.
        let r = resolve_nearest_variant(vk(2, 1, 0), &[vk(2, 0, 0), vk(1, 1, 0)]);
        assert_eq!(r.index, 0);
        assert!(r.apply_hflip && !r.palette_synth && !r.exact);
        // único con flip pero no paleta → cambio de tono (palette_synth).
        let r = resolve_nearest_variant(vk(2, 1, 0), &[vk(1, 1, 0)]);
        assert_eq!(r.index, 0);
        assert!(r.palette_synth && !r.apply_hflip);
        // ESCALERA de especificidad: la variante EXACTA gana al base comodín
        // aunque el base vaya primero (antes empataban en 0 y el base tapaba
        // todas las variantes — nunca se elegían).
        let r = resolve_nearest_variant(vk(2, 1, 0), &[vk(-1, -1, -1), vk(2, 1, 0)]);
        assert_eq!(r.index, 1);
        assert!(r.exact);
        // Paleta equivocada: el base comodín gana a la síntesis (se dibuja el
        // base tal cual + tinte ref — no arte de otra paleta tintado).
        let r = resolve_nearest_variant(vk(3, 0, 0), &[vk(-1, -1, -1), vk(1, 0, 0)]);
        assert_eq!(r.index, 0);
        assert!(!r.palette_synth && !r.apply_hflip);
        // exacto → sin síntesis.
        let r = resolve_nearest_variant(vk(3, 0, 0), &[vk(1, 0, 0), vk(3, 0, 0)]);
        assert_eq!(r.index, 1);
        assert!(r.exact);
        // -1 = cualquiera cubre el target sin sintetizar.
        let r = resolve_nearest_variant(vk(2, 1, 1), &[vk(-1, -1, -1)]);
        assert!(r.index == 0 && r.exact);
        // sin candidatos → -1.
        assert_eq!(resolve_nearest_variant(vk(0, 0, 0), &[]).index, -1);
    }

    #[test]
    fn pose_set_candidate_picks_asset_by_observed_palette() {
        // Integración: una pose instanciada con DOS candidatos por paleta. La
        // variante OBSERVADA del ancla elige el asset. Sin candidatos → el default.
        let mk = |hash: u64, x: i16, y: i16, pal: u8| SpriteOccurrence {
            hash,
            anim_group_id: 0,
            w_tiles: 1,
            h_tiles: 1,
            screen_x: x,
            screen_y: y,
            link: 0,
            palette: pal,
            priority: 0,
            slot: 0,
            hflip: 0,
            vflip: 0,
        };
        let vk = |p: i8| VariantKey {
            palette: p,
            hflip: -1,
            vflip: -1,
            ..Default::default()
        };
        let mut sub = PoseSetSubstitutor::new();
        sub.add_override_variants(
            vec![0xaa],
            Some(vec![(0, 0)]),
            None,
            None,
            0,
            0,
            0,
            [0, 0, 0],
            [[0; 3]; 4],
            "default.png".to_string(),
            String::new(),
            vec![(vk(1), "p1.png".to_string()), (vk(2), "p2.png".to_string())],
        );
        // Occurrence con paleta 2 → candidato p2.png.
        let occ2 = vec![mk(0xaa, 40, 40, 2)];
        let mut c = vec![false; 1];
        let s = sub.resolve(&occ2, &mut c);
        assert_eq!(s.len(), 1);
        assert_eq!(s[0].asset_path, "p2.png");
        // Occurrence con paleta 1 → candidato p1.png.
        let occ1 = vec![mk(0xaa, 40, 40, 1)];
        let mut c = vec![false; 1];
        assert_eq!(sub.resolve(&occ1, &mut c)[0].asset_path, "p1.png");
        // Paleta 3 (sin candidato que matchee) → gana el de menor costo/índice;
        // ambos son mismatch de paleta (costo 100) → el primero: p1.png.
        let occ3 = vec![mk(0xaa, 40, 40, 3)];
        let mut c = vec![false; 1];
        assert_eq!(sub.resolve(&occ3, &mut c)[0].asset_path, "p1.png");
    }

    #[test]
    fn pose_set_instanced_matches_mirrored_facing() {
        // Flip-aware: una pose captada mirando a la derecha (rel [(0,0),
        // (16,0)]) también matchea la instancia ESPEJADA (mirando a la izquierda),
        // porque la dirección es un estado de la misma pose, no una pose nueva.
        let mk = |hash: u64, x: i16, y: i16, hf: u8| SpriteOccurrence {
            hash,
            anim_group_id: 0,
            w_tiles: 2,
            h_tiles: 2,
            screen_x: x,
            screen_y: y,
            link: 0,
            palette: 0,
            priority: 0,
            slot: 0,
            hflip: hf,
            vflip: 0,
        };
        let mut sub = PoseSetSubstitutor::new();
        sub.add_override(
            vec![0xaa, 0xbb],
            Some(vec![(0, 0), (16, 0)]),
            0,
            0,
            "pose.png".to_string(),
        );
        // Cara derecha (arreglo capturado): 0xaa a la izq, 0xbb a la der.
        let right = vec![mk(0xaa, 100, 50, 0), mk(0xbb, 116, 50, 0)];
        let mut c = vec![false; 2];
        assert_eq!(sub.resolve(&right, &mut c).len(), 1);
        // Cara izquierda (ESPEJO H): 0xbb a la izq, 0xaa a la der, con hflip=1.
        let left = vec![mk(0xbb, 100, 50, 1), mk(0xaa, 116, 50, 1)];
        let mut c2 = vec![false; 2];
        let subs = sub.resolve(&left, &mut c2);
        assert_eq!(
            subs.len(),
            1,
            "la instancia espejada H matchea la misma pose"
        );
        assert_eq!(subs[0].screen_x, 100);
        assert!(c2[0] && c2[1]);
        // Arreglo que NO es ni el capturado ni ningún espejo → no matchea.
        let broken_input = vec![mk(0xaa, 100, 50, 0), mk(0xbb, 140, 80, 0)];
        let mut c3 = vec![false; 2];
        assert_eq!(sub.resolve(&broken_input, &mut c3).len(), 0);

        // Espejo VERTICAL: pose vertical (rel [(0,0),(0,16)]) matchea la instancia
        // con los miembros invertidos en Y.
        let mut subv = PoseSetSubstitutor::new();
        subv.add_override(
            vec![0xaa, 0xbb],
            Some(vec![(0, 0), (0, 16)]),
            0,
            0,
            "v.png".to_string(),
        );
        // Capturado: 0xaa arriba, 0xbb abajo.
        let top = vec![mk(0xaa, 50, 100, 0), mk(0xbb, 50, 116, 0)];
        let mut cv = vec![false; 2];
        assert_eq!(subv.resolve(&top, &mut cv).len(), 1);
        // Espejo V: 0xbb arriba, 0xaa abajo (vflip=... da igual, matchea por posición).
        let invertido = vec![mk(0xbb, 50, 100, 0), mk(0xaa, 50, 116, 0)];
        let mut cv2 = vec![false; 2];
        assert_eq!(
            subv.resolve(&invertido, &mut cv2).len(),
            1,
            "la instancia espejada V matchea la misma pose"
        );
    }

    #[test]
    fn tween_player_plays_then_holds() {
        // In-betweens v2: al cambiar la POSE (pose_key distinto) hacia un target
        // con tween, reproduce B1,B2 por TWEEN_TICKS cada uno DENTRO del hold de
        // B y después sostiene B. Shape v1 del pack (sin from) = comodín.
        let (ka, kb) = (pose_key_of(&[0xA]), pose_key_of(&[0xB]));
        let mut p = TweenPlayer::new();
        p.parse_toml("[[tween]]\ntarget = \"B.png\"\nframes = [\"B1.png\", \"B2.png\"]\n");
        p.begin_frame();
        assert_eq!(p.resolve("A.png", ka, 100, 100), "A.png");
        p.begin_frame();
        assert_eq!(p.resolve("B.png", kb, 102, 100), "B1.png");
        for _ in 0..(TWEEN_TICKS - 1) {
            p.begin_frame();
            assert_eq!(p.resolve("B.png", kb, 102, 100), "B1.png");
        }
        p.begin_frame();
        assert_eq!(p.resolve("B.png", kb, 102, 100), "B2.png");
        for _ in 0..(TWEEN_TICKS - 1) {
            p.begin_frame();
            assert_eq!(p.resolve("B.png", kb, 102, 100), "B2.png");
        }
        p.begin_frame();
        assert_eq!(p.resolve("B.png", kb, 102, 100), "B.png");
        p.begin_frame();
        assert_eq!(p.resolve("B.png", kb, 102, 100), "B.png");
    }

    #[test]
    fn tween_pair_exact_beats_wildcard() {
        // Escalera: el par exacto (from=A) gana sobre el comodín del mismo target.
        let (ka, kb) = (pose_key_of(&[0xA]), pose_key_of(&[0xB]));
        let mut p = TweenPlayer::new();
        p.parse_toml(concat!(
            "[[tween]]\ntarget = \"B.png\"\nfrom = \"A.png\"\nframes = [\"AB.png\"]\n",
            "[[tween]]\ntarget = \"B.png\"\nframes = [\"any.png\"]\n"
        ));
        p.begin_frame();
        assert_eq!(p.resolve("A.png", ka, 10, 10), "A.png");
        p.begin_frame();
        assert_eq!(p.resolve("B.png", kb, 10, 10), "AB.png");
        // Desde OTRO origen (C) cae al comodín.
        let (kc, kb2) = (pose_key_of(&[0xC]), pose_key_of(&[0xB]));
        let mut q = TweenPlayer::new();
        q.parse_toml(concat!(
            "[[tween]]\ntarget = \"B.png\"\nfrom = \"A.png\"\nframes = [\"AB.png\"]\n",
            "[[tween]]\ntarget = \"B.png\"\nframes = [\"any.png\"]\n"
        ));
        q.begin_frame();
        assert_eq!(q.resolve("C.png", kc, 10, 10), "C.png");
        q.begin_frame();
        assert_eq!(q.resolve("B.png", kb2, 10, 10), "any.png");
    }

    #[test]
    fn tween_same_pose_key_never_fires() {
        // Anti-falso-positivo: un cambio de ASSET dentro de la MISMA pose (flash
        // de paleta que elige otro candidato) NO dispara la transición.
        let kb = pose_key_of(&[0xB]);
        let mut p = TweenPlayer::new();
        p.parse_toml("[[tween]]\ntarget = \"B_rojo.png\"\nframes = [\"ib.png\"]\n");
        p.begin_frame();
        assert_eq!(p.resolve("B_azul.png", kb, 10, 10), "B_azul.png");
        p.begin_frame();
        assert_eq!(p.resolve("B_rojo.png", kb, 10, 10), "B_rojo.png");
    }

    #[test]
    fn tween_two_instances_independent() {
        // Dos personajes lejos uno del otro: tracks separados, tweens propios.
        let (ka, kb) = (pose_key_of(&[0xA]), pose_key_of(&[0xB]));
        let mut p = TweenPlayer::new();
        p.parse_toml("[[tween]]\ntarget = \"B.png\"\nframes = [\"B1.png\", \"B2.png\"]\n");
        p.begin_frame();
        assert_eq!(p.resolve("A.png", ka, 50, 100), "A.png"); // instancia 1
        assert_eq!(p.resolve("A.png", ka, 250, 100), "A.png"); // instancia 2
        p.begin_frame();
        assert_eq!(p.resolve("B.png", kb, 52, 100), "B1.png"); // 1 transiciona
        assert_eq!(p.resolve("A.png", ka, 250, 100), "A.png"); // 2 sigue en A
        for _ in 0..TWEEN_TICKS {
            p.begin_frame();
            assert_eq!(p.resolve("A.png", ka, 250, 100), "A.png");
        }
        // La instancia 2 recién ahora cambia: su tween arranca desde el dibujo 0
        // aunque el de la instancia 1 ya avanzó.
        p.begin_frame();
        assert_eq!(p.resolve("B.png", kb, 252, 100), "B1.png");
    }

    #[test]
    fn tween_interrupted_uses_previous_target() {
        // El juego corta a mitad de tween (B→C antes de terminar los intermedios
        // de B): el `from` del próximo lookup es el TARGET anterior (B.png),
        // nunca el intermedio en curso.
        let (ka, kb, kc) = (
            pose_key_of(&[0xA]),
            pose_key_of(&[0xB]),
            pose_key_of(&[0xC]),
        );
        let mut p = TweenPlayer::new();
        p.parse_toml(concat!(
            "[[tween]]\ntarget = \"B.png\"\nframes = [\"B1.png\", \"B2.png\"]\n",
            "[[tween]]\ntarget = \"C.png\"\nfrom = \"B.png\"\nframes = [\"BC.png\"]\n"
        ));
        p.begin_frame();
        assert_eq!(p.resolve("A.png", ka, 10, 10), "A.png");
        p.begin_frame();
        assert_eq!(p.resolve("B.png", kb, 10, 10), "B1.png");
        // Interrupción: cambia a C con el tween de B a mitad → par (B→C).
        p.begin_frame();
        assert_eq!(p.resolve("C.png", kc, 10, 10), "BC.png");
    }

    #[test]
    fn tween_track_expires_after_ttl() {
        // Instancia fuera de pantalla más de TRACK_TTL frames → track nuevo al
        // volver: sin `from`, la reaparición no dispara transición.
        let (ka, kb) = (pose_key_of(&[0xA]), pose_key_of(&[0xB]));
        let mut p = TweenPlayer::new();
        p.parse_toml("[[tween]]\ntarget = \"B.png\"\nframes = [\"B1.png\"]\n");
        p.begin_frame();
        assert_eq!(p.resolve("A.png", ka, 10, 10), "A.png");
        for _ in 0..(TRACK_TTL + 2) {
            p.begin_frame();
        } // desaparece
        p.begin_frame();
        assert_eq!(
            p.resolve("B.png", kb, 10, 10),
            "B.png",
            "track expirado: reaparecer no es una transición"
        );
    }

    #[test]
    fn tween_v2_ticks_and_overrides() {
        // ticks custom del pack v2 + overrides en vivo con prioridad.
        let (ka, kb) = (pose_key_of(&[0xA]), pose_key_of(&[0xB]));
        let mut p = TweenPlayer::new();
        p.parse_toml("[[tween]]\ntarget = \"B.png\"\nframes = [\"pk.png\"]\nticks = 1\n");
        p.set_override(None, "B.png", vec!["live.png".into()], 1);
        p.begin_frame();
        assert_eq!(p.resolve("A.png", ka, 10, 10), "A.png");
        p.begin_frame();
        assert_eq!(
            p.resolve("B.png", kb, 10, 10),
            "live.png",
            "el override vivo pisa el pack"
        );
        p.clear_overrides();
        p.clear_state();
        p.begin_frame();
        assert_eq!(p.resolve("A.png", ka, 10, 10), "A.png");
        p.begin_frame();
        assert_eq!(p.resolve("B.png", kb, 10, 10), "pk.png");
        p.begin_frame();
        assert_eq!(
            p.resolve("B.png", kb, 10, 10),
            "B.png",
            "ticks=1: un frame por dibujo"
        );
    }

    #[test]
    fn animation_grouper_groups_cycling_patterns() {
        // Two alternating patterns at the same SAT slot → same group_id after warm-up.
        let vram_a = make_vram(0x100, 80, 80, 1, 1, 0xAA);
        let vram_b = make_vram(0x100, 80, 80, 1, 1, 0xBB);
        let mut h = SpriteHasher::new();

        // Drive enough frames to pass the first ANIM_RECOMPUTE_PERIOD (16) and
        // accumulate enough occurrences (>= ANIM_MIN_OCCURRENCES = 2 each).
        for i in 0..40u32 {
            if i % 2 == 0 {
                h.process_vram(&vram_a, SAT_BASE_H40);
            } else {
                h.process_vram(&vram_b, SAT_BASE_H40);
            }
        }

        // Check final group assignments.
        h.process_vram(&vram_a, SAT_BASE_H40);
        let group_a = h.last_occurrences()[0].anim_group_id;

        h.process_vram(&vram_b, SAT_BASE_H40);
        let group_b = h.last_occurrences()[0].anim_group_id;

        assert_ne!(group_a, 0, "hash A should belong to an animation group");
        assert_ne!(group_b, 0, "hash B should belong to an animation group");
        assert_eq!(
            group_a, group_b,
            "both hashes should share the same animation group_id"
        );
    }

    #[test]
    fn animation_clips_orders_cycling_poses() {
        // Same cycling setup → the grouper should surface one ordered clip. (C-S1)
        let vram_a = make_vram(0x100, 80, 80, 1, 1, 0xAA);
        let vram_b = make_vram(0x100, 80, 80, 1, 1, 0xBB);
        let mut h = SpriteHasher::new();
        for i in 0..40u32 {
            if i % 2 == 0 {
                h.process_vram(&vram_a, SAT_BASE_H40);
            } else {
                h.process_vram(&vram_b, SAT_BASE_H40);
            }
        }
        let hash_a = {
            h.process_vram(&vram_a, SAT_BASE_H40);
            h.last_occurrences()[0].hash
        };
        let hash_b = {
            h.process_vram(&vram_b, SAT_BASE_H40);
            h.last_occurrences()[0].hash
        };

        let clips = h.animation_clips();
        assert_eq!(clips.len(), 1, "one animated slot → one clip");
        let clip = &clips[0];
        assert!(
            clip.looping,
            "an alternating cycle should be detected as looping"
        );
        assert_ne!(clip.id, 0);
        let poses: std::collections::HashSet<u64> = clip.frames.iter().map(|f| f.pose).collect();
        assert!(
            poses.contains(&hash_a) && poses.contains(&hash_b),
            "the clip should contain both cycling poses"
        );
        assert!(clip.frames.iter().all(|f| f.duration >= 1));
    }

    #[test]
    fn animation_grouper_no_group_for_static_sprite() {
        // Same pattern every frame → only one distinct hash → no group.
        let vram = make_vram(0x100, 80, 80, 1, 1, 0xCC);
        let mut h = SpriteHasher::new();

        for _ in 0..40 {
            h.process_vram(&vram, SAT_BASE_H40);
        }

        let group = h.last_occurrences()[0].anim_group_id;
        assert_eq!(
            group, 0,
            "a static sprite (one distinct hash) must not be grouped"
        );
    }

    // ---- AnimationClip consolidation (C-S1) ----

    fn vd(hashes: &[u64]) -> std::collections::VecDeque<u64> {
        hashes.iter().copied().collect()
    }

    #[test]
    fn clip_consolidates_cycle_with_order_and_timing() {
        // A held 3 frames, B held 2, C held 3 — three full cycles in the window.
        let (a, b, c) = (0x30u64, 0x10u64, 0x20u64); // min hash = b → order starts at b
        let mut hist = Vec::new();
        for _ in 0..3 {
            hist.extend(std::iter::repeat_n(a, 3));
            hist.extend(std::iter::repeat_n(b, 2));
            hist.extend(std::iter::repeat_n(c, 3));
        }
        let clip = consolidate_clip(0xABCD, &vd(&hist)).expect("should detect a clip");
        assert_eq!(clip.id, 0xABCD);
        assert!(clip.looping, "a repeating cycle loops");
        // Rotated to start at the smallest pose hash (b); durations = run lengths.
        assert_eq!(
            clip.frames,
            vec![
                AnimFrame {
                    pose: b,
                    duration: 2
                },
                AnimFrame {
                    pose: c,
                    duration: 3
                },
                AnimFrame {
                    pose: a,
                    duration: 3
                },
            ]
        );
    }

    #[test]
    fn clip_none_for_static_history() {
        assert!(consolidate_clip(1, &vd(&[0xAA, 0xAA, 0xAA, 0xAA])).is_none());
    }

    #[test]
    fn clip_rejects_near_static() {
        // Una pose dominante (50 frames) + poses transitorias (1t) = sprite casi
        // quieto con flicker, no una animación → rechazado (>70% una pose).
        let mut h = vec![0xAAu64; 50];
        h.extend_from_slice(&[0xBB, 0xCC, 0xAA]);
        assert!(consolidate_clip(9, &vd(&h)).is_none());
    }

    #[test]
    fn clip_loops_with_partial_second_cycle() {
        // 1.5 ciclos de A:2,B:2 (período hallado aunque no haya 2 ciclos completos)
        // → debe marcarse looping (antes exigía n>=2*period y daba false).
        let clip = consolidate_clip(3, &vd(&[0xA, 0xA, 0xB, 0xB, 0xA, 0xA, 0xB])).expect("clip");
        assert!(clip.looping);
    }

    #[test]
    fn clip_fallback_for_noisy_history() {
        // No clean period but ≥2 distinct poses → distinct-in-first-seen-order.
        let clip = consolidate_clip(7, &vd(&[0xA, 0xA, 0xB, 0xB, 0xC, 0xA, 0xB]))
            .expect("fallback should still yield a clip");
        assert_eq!(clip.frames.len(), 3);
        assert!(
            !clip.looping,
            "a non-periodic history is not marked looping"
        );
        let poses: std::collections::HashSet<u64> = clip.frames.iter().map(|f| f.pose).collect();
        assert_eq!(poses, [0xA, 0xB, 0xC].into_iter().collect());
    }

    #[test]
    fn sprite_hasher_exposes_clips() {
        // Two patterns alternating at one slot → one clip with both poses.
        let vram_a = make_vram(0x100, 80, 80, 1, 1, 0xAA);
        let vram_b = make_vram(0x100, 80, 80, 1, 1, 0xBB);
        let mut h = SpriteHasher::new();
        for i in 0..48u32 {
            if i % 2 == 0 {
                h.process_vram(&vram_a, SAT_BASE_H40);
            } else {
                h.process_vram(&vram_b, SAT_BASE_H40);
            }
        }
        assert_eq!(
            h.animation_clips().len(),
            1,
            "one animation group → one clip"
        );
        h.process_vram(&vram_a, SAT_BASE_H40);
        let group = h.last_occurrences()[0].anim_group_id;
        assert_ne!(group, 0);
        let clips = h.animation_clips();
        assert_eq!(clips[0].id, group, "clip id == anim_group_id");
        assert_eq!(
            clips[0].frames.len(),
            2,
            "two alternating poses → two frames"
        );
    }

    #[test]
    fn hidden_sprite_skipped() {
        // Sprite with Y = 0 (stored as 128 + (-128) = 0) is off-screen/hidden.
        let vram = make_vram(0x100, 0, -128, 1, 1, 0xFF);
        let mut h = SpriteHasher::new();
        h.process_vram(&vram, SAT_BASE_H40);
        // Y=-128 means top pixel is at -128, below the SPRITE_COORD_OFFSET cutoff.
        // It should be skipped.
        assert_eq!(
            h.last_occurrences().len(),
            0,
            "hidden sprite should be skipped"
        );
    }

    /// Regresión del caso Sonic 2 EHZ: la copia en VRAM de la SAT deja una
    /// cadena de links CÍCLICA (la lista buena vive en el cache interno del
    /// VDP). Un chain-walk queda atrapado en el ciclo (17→68→49→17… en la ROM
    /// real) y nunca llega a los sprites del jugador, que quedan FUERA de la
    /// cadena. El escaneo lineal de los 80 slots debe verlos a todos.
    #[test]
    fn cyclic_link_chain_still_finds_all_sprites() {
        let mut vram = vec![0u8; 65536];
        write_chain(&mut vram, SAT_BASE_H40, 6); // slots 0..5, en pantalla
        // Cerrar el ciclo: slot 3 → 1 (0→1→2→3→1…). Los slots 4 y 5 quedan
        // fuera de la cadena — como el jugador en EHZ.
        wr16(&mut vram, SAT_BASE_H40 + 3 * SAT_ENTRY_SIZE + 2, 1); // word size|link

        let mut h = SpriteHasher::new();
        h.process_vram(&vram, SAT_BASE_H40);
        let occs = h.last_occurrences();
        assert_eq!(occs.len(), 6, "los 6 slots en pantalla, links aparte");
        assert!(
            occs.iter().any(|o| o.slot == 4) && occs.iter().any(|o| o.slot == 5),
            "los sprites fuera de la cadena (el jugador) deben aparecer"
        );
        // Sin duplicados: cada slot en pantalla aparece exactamente una vez.
        let mut slots: Vec<u8> = occs.iter().map(|o| o.slot).collect();
        slots.sort_unstable();
        slots.dedup();
        assert_eq!(
            slots.len(),
            6,
            "un occurrence por slot (sin repetir el ciclo)"
        );
    }

    // ---- SAT auto-detection (SAT_AUTODETECT) ----

    #[test]
    fn autodetect_finds_sat_at_nonstandard_base() {
        // SAT deliberately NOT at SAT_BASE_H40 — a hardcoded 0xD800 would miss it.
        let base = 0xC000;
        let vram = make_vram_chain(base, 6);
        assert_eq!(
            SpriteHasher::detect_sat_base(&vram),
            Some(base),
            "detector must recover the real SAT base from the link chain"
        );

        let mut h = SpriteHasher::new();
        let new = h.process_vram(&vram, SAT_AUTODETECT);
        assert_eq!(new, 6, "autodetect should find all 6 chained sprites");
        assert_eq!(h.last_occurrences().len(), 6);
    }

    #[test]
    fn autodetect_none_on_empty_vram() {
        let vram = vec![0u8; 65536];
        assert_eq!(
            SpriteHasher::detect_sat_base(&vram),
            None,
            "all-zero VRAM has no plausible SAT"
        );
        let mut h = SpriteHasher::new();
        assert_eq!(
            h.process_vram(&vram, SAT_AUTODETECT),
            0,
            "no SAT → no sprites (must not false-positive)"
        );
    }

    #[test]
    fn autodetect_prefers_longer_chain() {
        // A short decoy chain and the real (longer) SAT in the same VRAM:
        // the detector must pick the one with more on-screen sprites.
        let mut vram = vec![0u8; 65536];
        write_chain(&mut vram, 0x2000, 3); // decoy (exactly the minimum)
        write_chain(&mut vram, 0xB800, 12); // real SAT (longer chain)
        assert_eq!(
            SpriteHasher::detect_sat_base(&vram),
            Some(0xB800),
            "detector must prefer the longer, more-populated chain"
        );
    }

    // ---- PoseSetSubstitutor (claim por especificidad) ----

    fn occ_at(hash: u64, x: i16, y: i16) -> SpriteOccurrence {
        occ_sized(hash, x, y, 1, 1)
    }

    fn occ_sized(hash: u64, x: i16, y: i16, w: u8, h: u8) -> SpriteOccurrence {
        SpriteOccurrence {
            hash,
            anim_group_id: 0,
            w_tiles: w,
            h_tiles: h,
            screen_x: x,
            screen_y: y,
            link: 0,
            palette: 0,
            priority: 0,
            slot: 0,
            hflip: 0,
            vflip: 0,
        }
    }

    /// Una pose chica cuyo arreglo es SUBCONJUNTO de una grande (poses vecinas
    /// con distinta cantidad de sprites) NO debe robarle miembros aunque esté
    /// antes en el catálogo: la más específica (más miembros) reclama primero.
    /// Antes (orden de inserción) la chica reclamaba 2 de los 3 occs, la grande
    /// nunca completaba y sus frames mostraban el sub chico + un sprite suelto.
    #[test]
    fn pose_set_larger_pose_claims_before_subset() {
        let mut ps = PoseSetSubstitutor::new();
        // Chica primero (orden de inserción desfavorable a propósito).
        ps.add_override(
            vec![1, 2],
            Some(vec![(0, 0), (8, 0)]),
            16,
            8,
            "small.png".into(),
        );
        ps.add_override(
            vec![1, 2, 3],
            Some(vec![(0, 0), (8, 0), (0, 8)]),
            16,
            16,
            "big.png".into(),
        );

        // Frame con la instancia GRANDE completa (la chica coincide como subconjunto).
        let occs = vec![occ_at(1, 100, 50), occ_at(2, 108, 50), occ_at(3, 100, 58)];
        let mut claimed = vec![false; occs.len()];
        let subs = ps.resolve(&occs, &mut claimed);

        assert_eq!(
            subs.len(),
            1,
            "una sola instancia: la grande; la chica no roba"
        );
        assert_eq!(subs[0].asset_path, "big.png");
        assert!(
            claimed.iter().all(|&c| c),
            "los 3 miembros quedan reclamados"
        );
    }

    /// Robo de ancla en el borde (defecto, Longmoan «Demo Amazona» f443-459):
    /// una pose GRANDE casi toda fuera de pantalla NO debe completar con un
    /// único miembro visible robándole la cabeza compartida a la pose que tiene
    /// MÁS occs reales. Walk 02 (16 miembros) anclaba la cabeza ~23px a la
    /// izquierda del origen real, completaba con 1 hit + 15 tolerados
    /// off-screen y reclamaba antes (más miembros), dejando a Walk back 03
    /// (5-6 occs exactas) sin completar. Gana la EVIDENCIA, no el tamaño.
    #[test]
    fn pose_set_edge_anchor_steal_prefers_real_evidence() {
        let mut ps = PoseSetSubstitutor::new();
        ps.set_screen(320, 224);
        // GRANDE (4 miembros): comparte la "cabeza" (hashes 1 y 2, separados
        // (7,5) igual que en la chica) en rel (27,0)/(34,5); el resto queda a
        // la izquierda → off-screen tolerado al anclar en el borde.
        ps.add_override(
            vec![1, 2, 3, 4],
            Some(vec![(27, 0), (34, 5), (0, 20), (10, 30)]),
            48,
            40,
            "grande.png".into(),
        );
        // CHICA (3 miembros): cabeza en (4,0)/(11,5) + cuerpo visible.
        ps.add_override(
            vec![1, 2, 5],
            Some(vec![(4, 0), (11, 5), (6, 20)]),
            24,
            30,
            "chica.png".into(),
        );

        // Personaje saliendo por la izquierda (origen real de la chica
        // (-10,27)): las dos cabezas y el cuerpo tienen occ. La grande ancla
        // el hash 2 en (1,32) → origen (-33,27): el hash 1 esperado en (-6,27)
        // COINCIDE con la occ real y los miembros 3/4 quedan provablemente
        // invisibles → completa con 2 hits. La chica completa con 3.
        let occs = vec![
            occ_sized(1, -6, 27, 1, 2), // cabeza 8×16
            occ_at(2, 1, 32),           // cabeza 8×8
            occ_sized(5, -4, 47, 2, 1), // cuerpo 16×8
        ];
        let mut claimed = vec![false; occs.len()];
        let subs = ps.resolve(&occs, &mut claimed);
        assert_eq!(subs.len(), 1, "una sola pose gana los 3 occs");
        assert_eq!(
            subs[0].asset_path, "chica.png",
            "gana la instancia con MÁS occs reales (3 vs 2), no la de más miembros"
        );
        assert!(
            claimed.iter().all(|&c| c),
            "los 3 miembros quedan reclamados"
        );
    }

    /// opción A: pose de paletas MIXTAS (Tyris cuerpo p0 + partes p1) →
    /// un quad POR GRUPO de línea, cada uno con su porción UV proporcional del
    /// MISMO asset, su paleta y la ref E1 de SU línea (el grupo del ancla cae
    /// a la ref clásica).
    #[test]
    fn pose_mixed_palettes_split_into_group_quads() {
        let occp = |hash: u64, x: i16, y: i16, pal: u8| {
            let mut o = occ_sized(hash, x, y, 2, 2); // miembros de 16×16
            o.palette = pal;
            o
        };
        let mut ps = PoseSetSubstitutor::new();
        // 3 miembros: fila superior p0 (cuerpo) + uno debajo p1 (partes).
        ps.add_override_variants(
            vec![1, 2, 3],
            Some(vec![(0, 0), (16, 0), (8, 16)]),
            None,
            None,
            32,
            32,
            0,
            [200, 100, 50],                          // ref clásica (línea del ancla)
            [[0; 3], [40, 80, 120], [0; 3], [0; 3]], // ref por línea: p1 autorada
            "tyris.png".into(),
            String::new(),
            Vec::new(),
        );

        let occs = vec![
            occp(1, 100, 50, 0),
            occp(2, 116, 50, 0),
            occp(3, 108, 66, 1),
        ];
        let mut claimed = vec![false; occs.len()];
        let subs = ps.resolve(&occs, &mut claimed);

        assert_eq!(subs.len(), 2, "dos grupos de paleta → dos quads");
        assert!(
            claimed.iter().all(|&c| c),
            "los 3 miembros quedan reclamados"
        );
        let g0 = subs.iter().find(|s| s.palette == 0).expect("grupo p0");
        let g1 = subs.iter().find(|s| s.palette == 1).expect("grupo p1");
        assert_eq!(g0.asset_path, "tyris.png");
        assert_eq!(g1.asset_path, "tyris.png");
        assert_eq!(g0.ref_rgb, [200, 100, 50], "grupo del ancla → ref clásica");
        assert_eq!(g1.ref_rgb, [40, 80, 120], "grupo p1 → ref de SU línea");
        // Bbox pose = (100,50)-(132,82): p0 arriba (32×16), p1 abajo (16×16).
        assert_eq!(
            (g0.screen_x, g0.screen_y, g0.w_px, g0.h_px),
            (100, 50, 32, 16)
        );
        assert_eq!(
            (g1.screen_x, g1.screen_y, g1.w_px, g1.h_px),
            (108, 66, 16, 16)
        );
        let close = |a: f32, b: f32| (a - b).abs() < 1e-6;
        assert!(
            close(g0.u0, 0.0) && close(g0.v0, 0.0) && close(g0.uw, 1.0) && close(g0.vh, 0.5),
            "UV del grupo p0"
        );
        assert!(
            close(g1.u0, 0.25) && close(g1.v0, 0.5) && close(g1.uw, 0.5) && close(g1.vh, 0.5),
            "UV del grupo p1"
        );
    }

    ///  variante por CONTENIDO de paleta — dos candidatos con el MISMO
    /// índice se distinguen por la firma de los slots marcados; la firma se
    /// latchea con la línea ESTABLE (≥30 frames) y sobrevive un fade.
    #[test]
    fn pose_variant_by_palette_content_signature() {
        // CRAM sintética: línea 1 con "ropa roja" en los slots 9-10.
        let mut cram = [0u16; 64];
        cram[16 + 9] = 0x0007; // rojo puro (R=7, empaquetado 3-3-3)
        cram[16 + 10] = 0x0005;
        let slots: u16 = (1 << 9) | (1 << 10);
        let sig_red = palette_signature(&cram, 1, slots);
        let mut cram_blue = cram;
        cram_blue[16 + 9] = 0x01C0; // azul puro (B=7)
        cram_blue[16 + 10] = 0x0140;
        let sig_blue = palette_signature(&cram_blue, 1, slots);
        assert_ne!(sig_red, sig_blue, "contenido distinto → firma distinta");

        // Pose con DOS candidatos del mismo índice de paleta (1), firmas rojo/azul.
        let mk_cand = |sig: u64, asset: &str| {
            (
                VariantKey {
                    palette: 1,
                    hflip: 0,
                    vflip: 0,
                    slots,
                    sig,
                },
                asset.to_string(),
            )
        };
        let mut ps = PoseSetSubstitutor::new();
        ps.add_override_variants(
            vec![1, 2],
            Some(vec![(0, 0), (8, 0)]),
            None,
            None,
            16,
            8,
            0,
            [0, 0, 0],
            [[0; 3]; 4],
            "default.png".into(),
            String::new(),
            vec![mk_cand(sig_red, "rojo.png"), mk_cand(sig_blue, "azul.png")],
        );

        let occp = |hash: u64, x: i16| {
            let mut o = occ_sized(hash, x, 50, 1, 1);
            o.palette = 1;
            o
        };
        let occs = vec![occp(1, 100), occp(2, 108)];

        // Sin CRAM estable todavía → firmas Unknown → empate al menor índice.
        let mut claimed = vec![false; 2];
        let subs = ps.resolve(&occs, &mut claimed);
        assert_eq!(subs[0].asset_path, "rojo.png", "sin latch: menor índice");

        // 30+ frames con la CRAM AZUL estable → el latch identifica "azul".
        for _ in 0..=SIG_STABLE_FRAMES {
            ps.set_cram(&cram_blue);
        }
        let mut claimed = vec![false; 2];
        let subs = ps.resolve(&occs, &mut claimed);
        assert_eq!(
            subs[0].asset_path, "azul.png",
            "firma del contenido vivo → gana el candidato azul"
        );

        // Un fade (CRAM cambiando cada frame) NO rompe el latch: la identidad
        // vigente sigue siendo la última estable (azul).
        let mut fade = cram_blue;
        for f in 0..10u16 {
            fade[16 + 9] = f; // línea 1 inestable
            ps.set_cram(&fade);
        }
        let mut claimed = vec![false; 2];
        let subs = ps.resolve(&occs, &mut claimed);
        assert_eq!(
            subs[0].asset_path, "azul.png",
            "durante el fade la firma queda LATCHEADA (azul)"
        );

        // La CRAM ROJA estable ≥30 frames → la identidad cambia a "rojo".
        for _ in 0..=SIG_STABLE_FRAMES {
            ps.set_cram(&cram);
        }
        let mut claimed = vec![false; 2];
        let subs = ps.resolve(&occs, &mut claimed);
        assert_eq!(
            subs[0].asset_path, "rojo.png",
            "nuevo estado estable → la firma re-identifica (rojo)"
        );
    }

    ///  pose de 1 SPRITE con candidatos — el flip del target sale de
    /// occ.hflip del ANCLA (la geometría de un solo miembro no distingue
    /// caras: los 4 arreglos colapsan) y un candidato con eje de flip COMODÍN
    /// se PRESENTA en la cara observada (espejo sintetizado — paridad con el
    /// auto-espejo por occ del canal per-sprite). El arte propio de una cara
    /// explícita gana exacto, sin síntesis.
    #[test]
    fn single_sprite_flip_candidate_and_wildcard_base_presentation() {
        let mut ps = PoseSetSubstitutor::new();
        ps.add_override_variants(
            vec![0xAA],
            Some(vec![(0, 0)]),
            None,
            None,
            8,
            8,
            0,
            [0, 0, 0],
            [[0; 3]; 4],
            "base.png".into(),
            String::new(),
            vec![
                (
                    VariantKey {
                        palette: -1,
                        hflip: -1,
                        vflip: -1,
                        ..Default::default()
                    },
                    "base.png".to_string(),
                ),
                (
                    VariantKey {
                        palette: 1,
                        hflip: 1,
                        vflip: 0,
                        ..Default::default()
                    },
                    "izq.png".to_string(),
                ),
            ],
        );
        // Cara izquierda con paleta del candidato → arte PROPIO, sin espejo.
        let mut o = occ_sized(0xAA, 60, 60, 1, 1);
        o.palette = 1;
        o.hflip = 1;
        let mut claimed = vec![false; 1];
        let subs = ps.resolve(&[o], &mut claimed);
        assert_eq!(subs[0].asset_path, "izq.png", "arte propio de la cara");
        assert_eq!(subs[0].mirror, 0, "cara explicita exacta -> sin espejo");
        // Paleta SIN candidato + cara izquierda → gana el base comodín y se
        // presenta ESPEJADO a la cara observada.
        let mut o2 = occ_sized(0xAA, 60, 60, 1, 1);
        o2.palette = 2;
        o2.hflip = 1;
        let mut claimed = vec![false; 1];
        let subs = ps.resolve(&[o2], &mut claimed);
        assert_eq!(
            subs[0].asset_path, "base.png",
            "sin candidato de esa paleta"
        );
        assert_eq!(
            subs[0].mirror, 1,
            "base comodin presentado en la cara observada (espejo H)"
        );
    }

    ///  la escalera con firmas — firma > índice de paleta > comodín; una
    /// firma que NO matchea pierde contra el base comodín.
    #[test]
    fn variant_signature_precedence() {
        let t = VariantKey {
            palette: 1,
            hflip: 0,
            vflip: 0,
            ..Default::default()
        };
        let k = |p: i8, slots: u16, sig: u64| VariantKey {
            palette: p,
            hflip: -1,
            vflip: -1,
            slots,
            sig,
        };
        // firma-match con paleta comodín GANA al índice exacto sin firma.
        let cands = [k(1, 0, 0), k(-1, 3, 77)];
        let r = resolve_nearest_variant_sig(t, &cands, &[SigState::None, SigState::Match]);
        assert_eq!(r.index, 1, "firma > índice");
        // firma-mismatch pierde contra el base comodín sin firma.
        let cands = [k(-1, 0, 0), k(1, 3, 77)];
        let r = resolve_nearest_variant_sig(t, &cands, &[SigState::None, SigState::Mismatch]);
        assert_eq!(r.index, 0, "mismatch de firma descalifica");
        // Sin firmas en juego, el +50 uniforme no altera el orden previo.
        let cands = [k(-1, 0, 0), k(1, 0, 0)];
        let r = resolve_nearest_variant_sig(t, &cands, &[SigState::None, SigState::None]);
        assert_eq!(r.index, 1, "sin firmas: índice exacto gana al comodín");
    }

    ///  grupos con solape >15% del área del menor (miembros intercalados)
    /// → quad ÚNICO con el tinte del ancla (comportamiento previo, sin doble
    /// dibujo de la zona compartida).
    #[test]
    fn pose_mixed_palettes_overlapping_groups_fall_back_to_single_quad() {
        let occp = |hash: u64, x: i16, pal: u8| {
            let mut o = occ_sized(hash, x, 50, 2, 2);
            o.palette = pal;
            o
        };
        let mut ps = PoseSetSubstitutor::new();
        // p1 (miembro 3) queda ADENTRO del bbox del grupo p0 (solape total).
        ps.add_override(
            vec![1, 2, 3],
            Some(vec![(0, 0), (16, 0), (8, 0)]),
            32,
            16,
            "mix.png".into(),
        );
        let occs = vec![occp(1, 100, 0), occp(2, 116, 0), occp(3, 108, 1)];
        let mut claimed = vec![false; occs.len()];
        let subs = ps.resolve(&occs, &mut claimed);
        assert_eq!(subs.len(), 1, "solape >15% → quad único");
        assert_eq!(subs[0].palette, 0, "tinte del ancla");
        assert_eq!(
            (subs[0].u0, subs[0].v0, subs[0].uw, subs[0].vh),
            (0.0, 0.0, 1.0, 1.0),
            "quad completo (identidad UV)"
        );
    }

    /// Salida por la DERECHA: un miembro cuyo rect esperado queda totalmente
    /// fuera del área visible (x >= W) se tolera — la pose matchea con los
    /// visibles, el bbox conserva la geometría 1:1 de la captura (incluye el
    /// rect esperado del ausente) y sólo los visibles quedan reclamados.
    #[test]
    fn pose_set_tolerates_member_off_right_edge() {
        let mut ps = PoseSetSubstitutor::new();
        ps.set_screen(320, 224);
        ps.add_override(
            vec![1, 2, 3],
            Some(vec![(0, 0), (8, 0), (16, 0)]),
            24,
            8,
            "pose.png".into(),
        );
        // Origen en x=304 → miembro 3 esperado en x=320 (borde visible: invisible).
        let occs = vec![occ_at(1, 304, 100), occ_at(2, 312, 100)];
        let mut claimed = vec![false; occs.len()];
        let subs = ps.resolve(&occs, &mut claimed);

        assert_eq!(
            subs.len(),
            1,
            "la pose se detecta con el miembro fuera de pantalla"
        );
        assert_eq!(subs[0].asset_path, "pose.png");
        assert_eq!(subs[0].screen_x, 304);
        assert_eq!(
            subs[0].w_tiles, 3,
            "el bbox incluye el rect esperado del ausente"
        );
        assert!(
            claimed.iter().all(|&c| c),
            "sólo los visibles se reclaman (ambos)"
        );
    }

    /// El límite es el MODO de video vivo: en H32 (256 de ancho, p.ej. Aladdin)
    /// un ausente esperado en x=260 es invisible y se tolera; con el default
    /// H40 (320) esa misma posición está en pantalla y NO se tolera.
    #[test]
    fn pose_set_tolerance_boundary_follows_video_mode() {
        let mut ps = PoseSetSubstitutor::new();
        ps.add_override(
            vec![1, 2],
            Some(vec![(0, 0), (8, 0)]),
            16,
            8,
            "pose.png".into(),
        );
        let occs = vec![occ_at(1, 252, 100)]; // miembro 2 esperado en x=260

        // Default 320 de ancho: 260 está en pantalla → ausencia no tolerada.
        let mut claimed = vec![false; occs.len()];
        assert!(ps.resolve(&occs, &mut claimed).is_empty());

        // Modo H32 (256): 260 queda fuera del área visible → tolerada.
        ps.set_screen(256, 224);
        let mut claimed = vec![false; occs.len()];
        let subs = ps.resolve(&occs, &mut claimed);
        assert_eq!(subs.len(), 1);
        assert_eq!(subs[0].screen_x, 252);
    }

    /// Salida por ARRIBA (cabeza/torso fuera): el ausente esperado en y <= -(h*8)
    /// se tolera anclando desde el miembro inferior; el bbox arranca en el y
    /// esperado del ausente (negativo).
    #[test]
    fn pose_set_tolerates_member_off_top_edge() {
        let mut ps = PoseSetSubstitutor::new();
        ps.add_override(
            vec![1, 2],
            Some(vec![(0, 0), (0, 16)]),
            8,
            24,
            "pose.png".into(),
        );
        // Sólo el miembro de abajo visible en y=0 → el de arriba esperado en
        // y=-16 (1×1 tile → totalmente fuera, tolerado).
        let occs = vec![occ_at(2, 100, 0)];
        let mut claimed = vec![false; occs.len()];
        let subs = ps.resolve(&occs, &mut claimed);

        assert_eq!(subs.len(), 1);
        assert_eq!(
            subs[0].screen_y, -16,
            "el bbox conserva el origen de captura"
        );
        assert!(claimed[0]);
    }

    /// Anti falso-positivo: un miembro ausente cuya posición esperada cae DENTRO
    /// de pantalla NO se tolera — la pose no matchea.
    #[test]
    fn pose_set_missing_member_on_screen_does_not_match() {
        let mut ps = PoseSetSubstitutor::new();
        ps.add_override(
            vec![1, 2],
            Some(vec![(0, 0), (8, 0)]),
            16,
            8,
            "pose.png".into(),
        );
        let occs = vec![occ_at(1, 100, 50)]; // el 2 esperado en (108,50): visible
        let mut claimed = vec![false; occs.len()];
        let subs = ps.resolve(&occs, &mut claimed);

        assert!(
            subs.is_empty(),
            "ausente en posición visible ⇒ no es la pose"
        );
        assert!(!claimed[0]);
    }

    /// Dims AUTORADAS por miembro (fix «Demo Amazona»): una cabeza 8×8 que salió
    /// TOTALMENTE de pantalla se tolera aunque no haya ninguna occ de su hash de
    /// la que copiar dims — sin pose.dims, el fallback (las dims del primer
    /// miembro visible, 32px) la juzgaba "parcialmente visible" y el match caía
    /// en los bordes; además el bbox del sub se engordaba con el rect mal medido.
    #[test]
    fn pose_set_authored_dims_tolerate_offscreen_small_member() {
        let mk = |dims: Option<Vec<(i16, i16)>>| {
            let mut ps = PoseSetSubstitutor::new();
            ps.add_override_variants(
                vec![1, 2],
                Some(vec![(0, 0), (0, 8)]),
                dims,
                None,
                32,
                40,
                0,
                [0, 0, 0],
                [[0; 3]; 4],
                "pose.png".into(),
                String::new(),
                Vec::new(),
            );
            ps
        };
        // Cuerpo (32×32) visible en (10,0); cabeza esperada en (10,-8): con su h
        // REAL (8) queda totalmente fuera (-8+8=0 ≤ 0); con el fallback (32 del
        // cuerpo) "asomaría" y el match se descartaba.
        let occs = vec![occ_sized(2, 10, 0, 4, 4)];
        let mut claimed = vec![false; occs.len()];
        let subs = mk(Some(vec![(8, 8), (32, 32)])).resolve(&occs, &mut claimed);
        assert_eq!(
            subs.len(),
            1,
            "con dims autoradas la cabeza fuera se tolera"
        );
        assert_eq!(subs[0].screen_y, -8, "el bbox conserva la geometría 1:1");
        assert_eq!(subs[0].w_px, 32);
        assert_eq!(subs[0].h_px, 40);
        // Sin dims (pose pre-migración): el fallback la juzga visible → no matchea
        // (comportamiento previo intacto).
        let mut claimed = vec![false; occs.len()];
        assert!(mk(None).resolve(&occs, &mut claimed).is_empty());
    }

    /// base_mirror (flip de presentación de Posar): el asset se autoró sobre el
    /// máster ESPEJADO → al matchear la cara capturada el sub sale con mirror=1
    /// (el render lo voltea a la dirección del juego); en la cara espejada el
    /// doble espejo se anula (mirror=0). Caso real: Run 02 de Tyris (flip="h").
    #[test]
    fn pose_set_base_mirror_xors_into_sub() {
        let mut ps = PoseSetSubstitutor::new();
        ps.add_override_variants(
            vec![1, 2],
            Some(vec![(0, 0), (12, 16)]),
            None,
            None,
            32,
            24,
            1,
            [0, 0, 0],
            [[0; 3]; 4],
            "pose.png".into(),
            String::new(),
            Vec::new(),
        );
        // Cara CAPTURADA (rel tal cual): cuerpo 4×1 en (100,50), pies 2×1 en (112,66).
        let occs = vec![occ_sized(1, 100, 50, 4, 1), occ_sized(2, 112, 66, 2, 1)];
        let mut claimed = vec![false; occs.len()];
        let subs = ps.resolve(&occs, &mut claimed);
        assert_eq!(subs.len(), 1);
        assert_eq!(
            subs[0].mirror, 1,
            "cara capturada + asset espejado → volteo"
        );
        // Cara ESPEJADA (espejo exacto por miembro: cuerpo→0, pies→32−12−16=4).
        let occs = vec![occ_sized(1, 100, 50, 4, 1), occ_sized(2, 104, 66, 2, 1)];
        let mut claimed = vec![false; occs.len()];
        let subs = ps.resolve(&occs, &mut claimed);
        assert_eq!(subs.len(), 1);
        assert_eq!(
            subs[0].mirror, 0,
            "cara espejada + asset espejado → tal cual"
        );
    }

    ///  instancia PARCIAL al borde con geometría AMBIGUA (miembros de
    /// hash repetido en fila; solo uno visible, el resto tolerado off-screen):
    /// captura y espejo completan por igual — el AGREEMENT de flips SAT
    /// (occ.flip == member_flip ^ arr_bits) desempata la cara. Sin flips
    /// autorados el orden estable previo se conserva.
    #[test]
    fn pose_set_partial_edge_flip_tiebreak() {
        // Pose: ancla (hash 7) centrada VERTICALMENTE + dos miembros gemelos
        // (hash 0xB) arriba/abajo que INTERCAMBIAN posición bajo el espejo V.
        // Con solo el ancla visible al borde derecho (los gemelos esperados a
        // x>=320 quedan fuera en AMBOS arreglos), captura y espejo V forman
        // candidatas de 1 hit geométricamente equivalentes.
        let mk = |flips: Option<Vec<u8>>| {
            let mut ps = PoseSetSubstitutor::new();
            ps.add_override_variants(
                vec![7, 0xB, 0xB],
                Some(vec![(0, 12), (8, 0), (8, 24)]),
                Some(vec![(8, 8), (8, 8), (8, 8)]),
                flips,
                16,
                32,
                0,
                [0, 0, 0],
                [[0; 3]; 4],
                "pose.png".into(),
                String::new(),
                Vec::new(),
            );
            ps
        };
        let mut occ = occ_sized(7, 312, 100, 1, 1);
        occ.vflip = 1; // el juego dibuja el ancla ESPEJADA en V
        let mut claimed = vec![false; 1];
        let subs = mk(Some(vec![0, 0, 0])).resolve(&[occ], &mut claimed);
        assert_eq!(subs.len(), 1);
        assert_eq!(
            subs[0].mirror, 2,
            "flip SAT observado desempata a la cara espejada (V)"
        );
        // occ SIN flip → gana la cara de captura.
        let occ2 = occ_sized(7, 312, 100, 1, 1);
        let mut claimed = vec![false; 1];
        let subs = mk(Some(vec![0, 0, 0])).resolve(&[occ2], &mut claimed);
        assert_eq!(subs.len(), 1);
        assert_eq!(subs[0].mirror, 0, "sin flip observado, cara de captura");
        // Pose LEGACY sin flips → orden estable previo (cara de captura),
        // aunque la occ venga espejada.
        let mut occ3 = occ_sized(7, 312, 100, 1, 1);
        occ3.vflip = 1;
        let mut claimed = vec![false; 1];
        let subs = mk(None).resolve(&[occ3], &mut claimed);
        assert_eq!(subs.len(), 1);
        assert_eq!(subs[0].mirror, 0, "sin flips autorados no hay desempate");
    }

    /// Las poses LEGACY (sin rel) no ganan tolerancia: sin offsets no hay
    /// posición esperada computable → siguen exigiendo el set completo.
    #[test]
    fn pose_set_legacy_still_requires_full_set() {
        let mut ps = PoseSetSubstitutor::new();
        ps.add_override(vec![1, 2], None, 0, 0, "pose.png".into());
        let occs = vec![occ_at(1, 330, 100)]; // cerca del borde, hash 2 ausente
        let mut claimed = vec![false; occs.len()];
        let subs = ps.resolve(&occs, &mut claimed);
        assert!(subs.is_empty());
    }

    /// Espejo con miembros de TAMAÑOS MIXTOS (el caso Tyris): el espejo exacto
    /// por miembro es x' = Wpx − x − w_i. Un cuerpo de 32px centrado no se mueve
    /// al espejar, pero los pies de 16px descentrados (rel 12) pasan a rel 4 —
    /// la aproximación uniforme (max_rel − x) los corría mal y la cara espejada
    /// nunca matcheaba completa.
    #[test]
    fn pose_set_mirrored_match_with_mixed_member_sizes() {
        let mut ps = PoseSetSubstitutor::new();
        ps.add_override(
            vec![1, 2],
            Some(vec![(0, 0), (12, 16)]),
            32,
            24,
            "pose.png".into(),
        );
        // Cara espejada: cuerpo (4×1, 32px) queda en 0; pies (2×1) en 32−12−16=4.
        let occs = vec![occ_sized(1, 100, 50, 4, 1), occ_sized(2, 104, 66, 2, 1)];
        let mut claimed = vec![false; occs.len()];
        let subs = ps.resolve(&occs, &mut claimed);

        assert_eq!(subs.len(), 1, "la cara espejada matchea con dims reales");
        assert_eq!(subs[0].screen_x, 100);
        assert!(claimed.iter().all(|&c| c));
    }

    /// Regresión con los DATOS REALES de «Tyris Flare - Walk 02» (Golden Axe,
    /// Demo Amazona): capturada en el frame 204 (cara hflip, rel de abajo) y
    /// vista espejada en el frame 551 en esas posiciones absolutas. El espejo
    /// exacto por miembro debe matchear los 5 — incluidos los pies (2×1 en
    /// rel 12,56 → espejados a 4,56).
    #[test]
    fn pose_set_tyris_walk02_matches_frame_551() {
        let mut ps = PoseSetSubstitutor::new();
        ps.add_override(
            vec![1, 2, 3, 4, 5],
            Some(vec![(8, 0), (4, 8), (0, 24), (8, 40), (12, 56)]),
            32,
            64,
            "tyris.png".into(),
        );
        let occs = vec![
            occ_sized(1, 190, 66, 2, 1),
            occ_sized(2, 186, 74, 3, 2),
            occ_sized(3, 182, 90, 4, 2),
            occ_sized(4, 190, 106, 2, 2),
            occ_sized(5, 186, 122, 2, 1), // los pies, espejados en su lugar
        ];
        let mut claimed = vec![false; occs.len()];
        let subs = ps.resolve(&occs, &mut claimed);

        assert_eq!(
            subs.len(),
            1,
            "la cara espejada del frame 551 matchea completa"
        );
        assert!(
            claimed.iter().all(|&c| c),
            "los 5 miembros reclamados (pies incluidos)"
        );
        assert_eq!(subs[0].screen_x, 182);
        assert_eq!(subs[0].screen_y, 66);
        assert_eq!(subs[0].w_px, 32);
        assert_eq!(subs[0].h_px, 64);
        assert_eq!(
            subs[0].mirror, 1,
            "arreglo espejo-H → el render voltea el asset canónico a SU dirección"
        );
    }

    /// El sub de pose lleva el bbox EXACTO en píxeles (p.ej. 29×8): truncarlo a
    /// tiles (24 px) achataba el HD/snapshot al dibujarlo (deformación visible
    /// al armar poses cuyo bbox no es múltiplo de 8).
    #[test]
    fn pose_set_sub_carries_exact_pixel_bbox() {
        let mut ps = PoseSetSubstitutor::new();
        ps.add_override(
            vec![1, 2],
            Some(vec![(0, 0), (21, 0)]),
            29,
            8,
            "pose.png".into(),
        );
        let occs = vec![occ_at(1, 100, 50), occ_at(2, 121, 50)];
        let mut claimed = vec![false; occs.len()];
        let subs = ps.resolve(&occs, &mut claimed);
        assert_eq!(subs.len(), 1);
        assert_eq!(subs[0].w_px, 29, "bbox exacto, no tile-múltiplo");
        assert_eq!(subs[0].h_px, 8);
        assert_eq!(
            subs[0].w_tiles, 3,
            "los tiles truncados se conservan (compat)"
        );
    }

    /// La chica sigue matcheando sola cuando la grande NO está completa en el
    /// frame (su propio frame de la animación).
    #[test]
    fn pose_set_subset_still_matches_alone() {
        let mut ps = PoseSetSubstitutor::new();
        ps.add_override(
            vec![1, 2],
            Some(vec![(0, 0), (8, 0)]),
            16,
            8,
            "small.png".into(),
        );
        ps.add_override(
            vec![1, 2, 3],
            Some(vec![(0, 0), (8, 0), (0, 8)]),
            16,
            16,
            "big.png".into(),
        );

        let occs = vec![occ_at(1, 100, 50), occ_at(2, 108, 50)]; // sin el hash 3
        let mut claimed = vec![false; occs.len()];
        let subs = ps.resolve(&occs, &mut claimed);

        assert_eq!(subs.len(), 1);
        assert_eq!(subs[0].asset_path, "small.png");
    }

    /// Vestuario: la máscara acompaña SOLO al asset BASE (decisión de producto
    /// 2026-08-18) — un candidato de variante es recolor autorado a mano y su
    /// quad sale sin máscara; el base compitiendo de comodín (mismo asset id)
    /// sí la lleva.
    #[test]
    fn pose_set_mask_only_on_base_asset() {
        // Sin candidatos: el sub del base lleva la máscara.
        let mut ps = PoseSetSubstitutor::new();
        ps.add_override_variants(
            vec![1, 2],
            Some(vec![(0, 0), (8, 0)]),
            None,
            None,
            16,
            8,
            0,
            [0, 0, 0],
            [[0; 3]; 4],
            "base.png".into(),
            "vestuario.png".into(),
            Vec::new(),
        );
        let occs = vec![occ_at(1, 100, 50), occ_at(2, 108, 50)];
        let mut claimed = vec![false; occs.len()];
        let subs = ps.resolve(&occs, &mut claimed);
        assert_eq!(subs.len(), 1);
        assert_eq!(subs[0].mask_path, "vestuario.png");

        // Candidato EXACTO para la paleta observada (0) → gana el recolor
        // autorado y la máscara NO viaja.
        let mut ps = PoseSetSubstitutor::new();
        ps.add_override_variants(
            vec![1, 2],
            Some(vec![(0, 0), (8, 0)]),
            None,
            None,
            16,
            8,
            0,
            [0, 0, 0],
            [[0; 3]; 4],
            "base.png".into(),
            "vestuario.png".into(),
            vec![(
                VariantKey {
                    palette: 0,
                    hflip: 0,
                    vflip: 0,
                    ..Default::default()
                },
                "variante_p0.png".into(),
            )],
        );
        let mut claimed = vec![false; occs.len()];
        let subs = ps.resolve(&occs, &mut claimed);
        assert_eq!(subs.len(), 1);
        assert_eq!(subs[0].asset_path, "variante_p0.png");
        assert_eq!(subs[0].mask_path, "", "recolor autorado: sin Vestuario");

        // El base compitiendo de COMODÍN -1/-1/-1 (forma real del pack) más
        // una variante de OTRA paleta: gana el comodín → mask presente.
        let mut ps = PoseSetSubstitutor::new();
        ps.add_override_variants(
            vec![1, 2],
            Some(vec![(0, 0), (8, 0)]),
            None,
            None,
            16,
            8,
            0,
            [0, 0, 0],
            [[0; 3]; 4],
            "base.png".into(),
            "vestuario.png".into(),
            vec![
                (
                    VariantKey {
                        palette: -1,
                        hflip: -1,
                        vflip: -1,
                        ..Default::default()
                    },
                    "base.png".into(),
                ),
                (
                    VariantKey {
                        palette: 2,
                        hflip: 0,
                        vflip: 0,
                        ..Default::default()
                    },
                    "variante_p2.png".into(),
                ),
            ],
        );
        let mut claimed = vec![false; occs.len()];
        let subs = ps.resolve(&occs, &mut claimed);
        assert_eq!(subs.len(), 1);
        assert_eq!(subs[0].asset_path, "base.png");
        assert_eq!(
            subs[0].mask_path, "vestuario.png",
            "el comodín ES el base (mismo id) → la máscara viaja"
        );
    }

    /// Vestuario en el pack: `mask` se parsea de `[[pose]]` y llega al sub; un
    /// pack viejo (sin el campo) emite mask_path vacío — retrocompatible.
    #[test]
    fn pose_set_parses_mask_from_pack_toml() {
        let mut ps = PoseSetSubstitutor::new();
        ps.parse_toml(concat!(
            "[[pose]]\n",
            "hashes = [\"0x1\", \"0x2\"]\n",
            "rel = \"0,0|8,0\"\n",
            "max_w = 16\n",
            "max_h = 8\n",
            "asset = \"aaaa1111\"\n",
            "mask = \"mmmm2222\"\n",
            "\n",
            "[[pose]]\n",
            "hashes = [\"0x7\"]\n",
            "rel = \"0,0\"\n",
            "max_w = 8\n",
            "max_h = 8\n",
            "asset = \"bbbb3333\"\n"
        ));
        let occs = vec![occ_at(1, 100, 50), occ_at(2, 108, 50), occ_at(7, 200, 80)];
        let mut claimed = vec![false; occs.len()];
        let subs = ps.resolve(&occs, &mut claimed);
        assert_eq!(subs.len(), 2);
        let with = subs.iter().find(|s| s.asset_path == "aaaa1111").unwrap();
        let without = subs.iter().find(|s| s.asset_path == "bbbb3333").unwrap();
        assert_eq!(with.mask_path, "mmmm2222");
        assert_eq!(without.mask_path, "", "pack sin `mask` → sin máscara");
    }
}
