//! Pure geometry for assigning hardware sprites to RAM-anchored entities.
//!
//! The matcher combines world positions, camera scroll, pivots, and match boxes
//! without reading emulator state itself, making results deterministic.

// ---------------------------------------------------------------------------
// ram_anchor.rs — Mode 3 geometry: match SAT sprites to RAM-anchored entities.
//
// Modes 1/2 group hardware sprites by pattern/anim-group and, for repeats, by
// spatial clustering. Clustering can't tell two *identical* on-screen entities
// apart (two of the same enemy, co-op players): their sprites share hashes and
// anim-groups, so a purely visual split guesses. Mode 3 removes the guess by
// reading each entity's **world position** from game RAM — every instance has a
// distinct world_pos, so identity is exact.
//
// This module is the pure geometric core: given the entities' world positions
// (read from RAM elsewhere), the camera scroll (read from the VDP elsewhere) and
// this frame's SAT sprite screen positions, it assigns each sprite to an entity.
// It reads no RAM and no VDP — those are fed in — so it is deterministic and unit
// testable, and the same code serves the live engine and offline `.arp` replay.
//
// ## World ↔ screen  (metasprites-study §8.1)
//
// The SAT gives *screen* coordinates; RAM gives *world* coordinates. They meet at
//
//     screen_pos(sprite) ≈ world_pos(entity) − scroll_camera + pivot   (± box)
//
// `pivot` absorbs the offset between the entity's RAM origin (often its hotspot)
// and the top-left of its visual bounding box; the box half-extents are the match
// tolerance. A sprite inside an entity's box belongs to it; when boxes overlap,
// the nearest center wins (deterministic, lowest-index on ties).
// ---------------------------------------------------------------------------

/// One entity located in world space this frame (read from game RAM elsewhere).
#[derive(Clone, Copy, Debug)]
pub struct AnchoredEntity {
    /// Stable per-instance id (e.g. the RAM struct address) — this is what makes
    /// two identical entities distinguishable.
    pub id: u64,
    /// Horizontal position of the entity in world space.
    pub world_x: i32,
    /// Vertical position of the entity in world space.
    pub world_y: i32,
}

/// The match box shared by an entity kind: the pivot from world origin to the
/// bbox anchor, and the half-extents used as the match tolerance.
#[derive(Clone, Copy, Debug)]
pub struct AnchorBox {
    /// Horizontal offset from the entity origin to its visual anchor.
    pub pivot_x: i32,
    /// Vertical offset from the entity origin to its visual anchor.
    pub pivot_y: i32,
    /// Horizontal matching tolerance on either side of the anchor.
    pub half_w: i32,
    /// Vertical matching tolerance on either side of the anchor.
    pub half_h: i32,
}

/// The SAT sprites claimed by one entity (indices into the input positions).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AnchorAssignment {
    /// Stable identifier of the matched entity instance.
    pub id: u64,
    /// Indices of the input sprites assigned to the entity.
    pub members: Vec<usize>,
}

/// Assign this frame's sprites to the anchored entities.
///
/// - `entities`   — world positions this frame (keep a stable order, e.g. by id,
///   for deterministic tie-breaking).
/// - `scroll_x/y` — camera scroll of the plane the entities live on.
/// - `b`          — the shared match box (pivot + tolerance).
/// - `sprite_pos` — top-left screen positions of the SAT sprites this frame.
///
/// Returns one `AnchorAssignment` per entity (same order), each listing the
/// sprite indices inside its box. A sprite matching no box is left unassigned;
/// a sprite inside several boxes goes to the nearest center.
pub fn assign_sprites(
    entities: &[AnchoredEntity],
    scroll_x: i32,
    scroll_y: i32,
    b: &AnchorBox,
    sprite_pos: &[(i16, i16)],
) -> Vec<AnchorAssignment> {
    // Expected screen center of each entity: world − scroll + pivot.
    let centers: Vec<(i32, i32)> = entities
        .iter()
        .map(|e| {
            (
                e.world_x - scroll_x + b.pivot_x,
                e.world_y - scroll_y + b.pivot_y,
            )
        })
        .collect();

    let mut members: Vec<Vec<usize>> = vec![Vec::new(); entities.len()];

    for (si, &(sx, sy)) in sprite_pos.iter().enumerate() {
        let (sx, sy) = (sx as i32, sy as i32);
        let mut best: Option<(usize, i64)> = None;
        for (ei, &(cx, cy)) in centers.iter().enumerate() {
            let dx = (sx - cx).abs();
            let dy = (sy - cy).abs();
            if dx <= b.half_w && dy <= b.half_h {
                let dist = (dx as i64) * (dx as i64) + (dy as i64) * (dy as i64);
                // Strictly-closer replaces → keeps the lowest index on ties.
                if best.is_none_or(|(_, bd)| dist < bd) {
                    best = Some((ei, dist));
                }
            }
        }
        if let Some((ei, _)) = best {
            members[ei].push(si);
        }
    }

    entities
        .iter()
        .zip(members)
        .map(|(e, mem)| AnchorAssignment {
            id: e.id,
            members: mem,
        })
        .collect()
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    const BOX16: AnchorBox = AnchorBox {
        pivot_x: 0,
        pivot_y: 0,
        half_w: 16,
        half_h: 16,
    };

    fn ent(id: u64, x: i32, y: i32) -> AnchoredEntity {
        AnchoredEntity {
            id,
            world_x: x,
            world_y: y,
        }
    }

    #[test]
    fn assigns_nearby_sprites_and_skips_far_ones() {
        let entities = [ent(1, 100, 50)];
        let sprites = [(100i16, 50i16), (110, 55), (200, 200)];
        let out = assign_sprites(&entities, 0, 0, &BOX16, &sprites);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].id, 1);
        assert_eq!(out[0].members, vec![0, 1]); // (200,200) is out of the box
    }

    #[test]
    fn scroll_shifts_the_expected_position() {
        let entities = [ent(1, 100, 50)];
        // With scroll (80,0) the entity is expected at screen (20,50).
        let sprites = [(20i16, 50i16), (100, 50)];
        let out = assign_sprites(&entities, 80, 0, &BOX16, &sprites);
        assert_eq!(
            out[0].members,
            vec![0],
            "only the scroll-corrected sprite matches"
        );
    }

    #[test]
    fn two_identical_instances_disambiguated_by_world_pos() {
        // The Mode 2 blind spot: two of the same entity, far apart.
        let entities = [ent(0xA, 100, 50), ent(0xB, 300, 50)];
        let sprites = [(102i16, 50i16), (298, 52), (100, 48), (300, 50)];
        let out = assign_sprites(&entities, 0, 0, &BOX16, &sprites);
        assert_eq!(out[0].id, 0xA);
        assert_eq!(out[0].members, vec![0, 2]);
        assert_eq!(out[1].id, 0xB);
        assert_eq!(out[1].members, vec![1, 3]);
    }

    #[test]
    fn overlapping_boxes_nearest_center_wins() {
        let entities = [ent(1, 100, 50), ent(2, 120, 50)];
        // A sprite at 108 is closer to entity 1 (100) than entity 2 (120).
        let sprites = [(108i16, 50i16), (118, 50)];
        let out = assign_sprites(&entities, 0, 0, &BOX16, &sprites);
        assert_eq!(out[0].members, vec![0], "108 → nearest is entity 1");
        assert_eq!(out[1].members, vec![1], "118 → nearest is entity 2");
    }

    #[test]
    fn pivot_offset_is_applied() {
        let entities = [ent(1, 100, 50)];
        // pivot moves the expected anchor to (100+8, 50-4) = (108, 46).
        let b = AnchorBox {
            pivot_x: 8,
            pivot_y: -4,
            half_w: 4,
            half_h: 4,
        };
        let sprites = [(108i16, 46i16), (100, 50)];
        let out = assign_sprites(&entities, 0, 0, &b, &sprites);
        assert_eq!(out[0].members, vec![0]);
    }

    #[test]
    fn sprite_in_no_box_is_unassigned() {
        let entities = [ent(1, 0, 0)];
        let sprites = [(500i16, 400i16)];
        let out = assign_sprites(&entities, 0, 0, &BOX16, &sprites);
        assert!(out[0].members.is_empty());
    }
}
