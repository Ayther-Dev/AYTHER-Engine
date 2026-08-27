//! Reconstruction of scrolling background planes from frame observations.
//!
//! [`BackgroundStitcher`] accumulates wrapping VDP cells in level space so
//! authoring tools can export complete, deterministic layer images.

// ---------------------------------------------------------------------------
// background.rs — Fondos: stitch a scrolling plane into a full level strip.
//
// A Mega Drive plane nametable is small (typically 64×32 cells) and *wraps*: as
// the camera scrolls, the game streams new level columns into it, overwriting
// the ones that scrolled off. So no single frame holds a whole level — to
// reconstruct it (to export a per-layer PNG the artist repaints in HD) we must
// accumulate the cells seen over a whole `.arp` take, keyed by their position in
// **level space** rather than in the wrapping nametable (componentes-plan §3.3).
//
// `BackgroundStitcher` is that accumulator, and it is the pure core: it takes
// per-frame observations `(plane, level_x, level_y, cell)` — where the caller
// has already turned the on-screen nametable cells into absolute level-tile
// coordinates using the VDP scroll — and no VDP/RAM of its own. Deterministic
// and unit-testable; the same code serves live capture and offline replay.
//
// `cell` is an opaque per-cell code (the caller packs pattern|palette|flips): the
// stitcher only needs equality to detect a cell that changed between visits
// (animated tiles / a genuinely different level cell). Level coordinates are in
// **tiles** (1 cell = 8 px).
// ---------------------------------------------------------------------------

use std::collections::HashMap;

/// The three Mega Drive background planes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Plane {
    /// Scrolling plane A.
    A = 0,
    /// Scrolling plane B.
    B = 1,
    /// Fixed window plane.
    Window = 2,
}

impl Plane {
    /// Converts the hardware plane index `0..=2` into a plane.
    pub fn from_u8(v: u8) -> Option<Plane> {
        match v {
            0 => Some(Plane::A),
            1 => Some(Plane::B),
            2 => Some(Plane::Window),
            _ => None,
        }
    }
}

/// How a reconstructed layer scrolls (componentes-plan §3.5). Authoring output;
/// the stitcher produces the pixels, this describes how the runtime moves them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BgKind {
    /// A screen-fixed layer.
    Fixed,
    /// A layer that follows the game camera.
    Scrolling,
    /// A layer composited with an independent parallax ratio.
    Layered,
}

/// One reconstructed background layer, ready to assign an HD asset to.
#[derive(Clone, Debug)]
pub struct BackgroundLayer {
    /// Hardware plane represented by this layer.
    pub plane: Plane,
    /// Runtime scrolling behavior.
    pub kind: BgKind,
    /// Path to the full-layer image in the pack.
    pub asset: String, // full-layer PNG in the.ay pack
    /// Horizontal origin in level-tile coordinates.
    pub origin_x: i32, // anchor in level space (tiles)
    /// Vertical origin in level-tile coordinates.
    pub origin_y: i32,
    /// Camera-motion ratio; `1.0` follows the game and smaller values recede.
    pub scroll_ratio: f32, // 1.0 = moves with the game; <1 = distant parallax
}

/// What accumulated at one level cell: the first code seen (a stable
/// representative for export), the latest, and whether it ever changed (the cell
/// animates / the game streamed a different tile there).
#[derive(Clone, Copy)]
struct Cell {
    first: u32,
    last: u32,
    changed: bool,
}

/// Per-plane accumulation state.
#[derive(Default)]
struct PlaneMap {
    cells: HashMap<(i32, i32), Cell>,
    conflicts: usize, // total observations that differed from the previous
    animated: usize,  // distinct cells that ever changed (animated tiles)
    min_x: i32,
    min_y: i32,
    max_x: i32,
    max_y: i32,
    seen: bool,
}

/// Accumulates the visible cells of each plane into level space across a take.
#[derive(Default)]
pub struct BackgroundStitcher {
    planes: [PlaneMap; 3],
}

impl BackgroundStitcher {
    /// Creates an empty stitcher for all three hardware planes.
    pub fn new() -> Self {
        Self::default()
    }

    /// Record one visible cell. `plane` is 0/1/2 (A/B/Window); `level_x/level_y`
    /// are its absolute level-tile coordinates; `cell` is the opaque code. A cell
    /// re-seen with a different code counts as a conflict and keeps the latest
    /// (the game either animated it or streamed a different tile there).
    pub fn observe(&mut self, plane: u8, level_x: i32, level_y: i32, cell: u32) {
        let p = match plane {
            0..=2 => &mut self.planes[plane as usize],
            _ => return,
        };
        match p.cells.get_mut(&(level_x, level_y)) {
            Some(c) => {
                if c.last != cell {
                    p.conflicts += 1;
                }
                if cell != c.first && !c.changed {
                    c.changed = true;
                    p.animated += 1; // first time this cell diverges
                }
                c.last = cell;
            }
            None => {
                p.cells.insert(
                    (level_x, level_y),
                    Cell {
                        first: cell,
                        last: cell,
                        changed: false,
                    },
                );
            }
        }
        if !p.seen {
            p.seen = true;
            p.min_x = level_x;
            p.max_x = level_x;
            p.min_y = level_y;
            p.max_y = level_y;
        } else {
            if level_x < p.min_x {
                p.min_x = level_x;
            }
            if level_x > p.max_x {
                p.max_x = level_x;
            }
            if level_y < p.min_y {
                p.min_y = level_y;
            }
            if level_y > p.max_y {
                p.max_y = level_y;
            }
        }
    }

    /// Distinct level cells accumulated for a plane.
    pub fn cell_count(&self, plane: u8) -> usize {
        if plane > 2 {
            return 0;
        }
        self.planes[plane as usize].cells.len()
    }

    /// Total observations that differed from the previous code at their cell
    /// (frame-to-frame churn — animation + any streaming noise).
    pub fn conflicts(&self, plane: u8) -> usize {
        if plane > 2 {
            return 0;
        }
        self.planes[plane as usize].conflicts
    }

    /// Distinct level cells that ever changed code = **animated tiles** (the
    /// game cycles them in place). The export marks these so the artist knows a
    /// cell is not a single static tile; `representative` gives a stable frame.
    pub fn animated_cells(&self, plane: u8) -> usize {
        if plane > 2 {
            return 0;
        }
        self.planes[plane as usize].animated
    }

    /// Whether a specific level cell animated over the take.
    pub fn is_animated(&self, plane: u8, level_x: i32, level_y: i32) -> bool {
        if plane > 2 {
            return false;
        }
        self.planes[plane as usize]
            .cells
            .get(&(level_x, level_y))
            .is_some_and(|c| c.changed)
    }

    /// The first code seen at a cell — a stable representative for PNG export
    /// (independent of which frame the take happened to end on).
    pub fn representative(&self, plane: u8, level_x: i32, level_y: i32) -> Option<u32> {
        if plane > 2 {
            return None;
        }
        self.planes[plane as usize]
            .cells
            .get(&(level_x, level_y))
            .map(|c| c.first)
    }

    /// Bounding box in level tiles: `(min_x, min_y, max_x, max_y)`, or None if
    /// nothing was seen for the plane.
    pub fn bounds(&self, plane: u8) -> Option<(i32, i32, i32, i32)> {
        if plane > 2 {
            return None;
        }
        let p = &self.planes[plane as usize];
        if p.seen {
            Some((p.min_x, p.min_y, p.max_x, p.max_y))
        } else {
            None
        }
    }

    /// Width / height of the reconstructed strip in tiles (inclusive bounds).
    pub fn width_tiles(&self, plane: u8) -> i32 {
        self.bounds(plane).map_or(0, |(a, _, b, _)| b - a + 1)
    }
    /// Returns the reconstructed height of a plane in tiles.
    pub fn height_tiles(&self, plane: u8) -> i32 {
        self.bounds(plane).map_or(0, |(_, a, _, b)| b - a + 1)
    }

    /// The most recent code at a level cell, if seen.
    pub fn get(&self, plane: u8, level_x: i32, level_y: i32) -> Option<u32> {
        if plane > 2 {
            return None;
        }
        self.planes[plane as usize]
            .cells
            .get(&(level_x, level_y))
            .map(|c| c.last)
    }
}

// ---------------------------------------------------------------------------
// Unwrap a wrapped scroll into an absolute, monotone camera position.
//
// The VDP Hscroll is a 10-bit value that wraps at the plane width, so on its own
// it only gives the camera modulo the plane. Accumulating the *shortest signed
// step* between consecutive frames recovers the absolute camera (game-agnostic —
// no per-game RAM read) as long as the per-frame move is < half a plane, which
// always holds (a camera can't jump half a screen-plane in one frame).
// ---------------------------------------------------------------------------

/// Running unwrap of a value known modulo `period`.
#[derive(Clone, Copy, Debug)]
pub struct ScrollUnwrapper {
    period: i32,
    absolute: i64,
    last_mod: i32,
    ///  delta del ÚLTIMO push (0 en el primero) — el caller detecta un
    /// salto NO FÍSICO (corte de escena: la cámara no se mueve >N px/frame).
    last_step: i32,
    started: bool,
}

impl ScrollUnwrapper {
    /// Creates an unwrapper for values modulo `period`.
    ///
    /// Non-positive periods are normalized to one.
    pub fn new(period: i32) -> Self {
        Self {
            period: period.max(1),
            absolute: 0,
            last_mod: 0,
            last_step: 0,
            started: false,
        }
    }

    /// Feed this frame's wrapped value (in `[0, period)`); returns the absolute.
    pub fn push(&mut self, wrapped: i32) -> i64 {
        let w = ((wrapped % self.period) + self.period) % self.period;
        if !self.started {
            self.started = true;
            self.last_mod = w;
            self.absolute = w as i64;
            self.last_step = 0;
            return self.absolute;
        }
        // Shortest signed delta across the wrap boundary.
        let mut d = w - self.last_mod;
        if d > self.period / 2 {
            d -= self.period;
        }
        if d < -self.period / 2 {
            d += self.period;
        }
        self.absolute += d as i64;
        self.last_mod = w;
        self.last_step = d;
        self.absolute
    }

    /// Returns the current unwrapped absolute position.
    pub fn absolute(&self) -> i64 {
        self.absolute
    }

    /// Returns the signed delta produced by the most recent [`Self::push`].
    ///
    /// A large absolute delta usually indicates a scene cut.
    pub fn last_step(&self) -> i32 {
        self.last_step
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn accumulates_cells_and_bounds() {
        let mut s = BackgroundStitcher::new();
        s.observe(0, 10, 5, 0xAA);
        s.observe(0, 11, 5, 0xBB);
        s.observe(0, 10, 6, 0xCC);
        assert_eq!(s.cell_count(0), 3);
        assert_eq!(s.bounds(0), Some((10, 5, 11, 6)));
        assert_eq!(s.width_tiles(0), 2);
        assert_eq!(s.height_tiles(0), 2);
        assert_eq!(s.get(0, 11, 5), Some(0xBB));
        assert_eq!(s.get(0, 99, 99), None);
    }

    #[test]
    fn revisiting_same_cell_same_code_is_not_a_conflict() {
        let mut s = BackgroundStitcher::new();
        s.observe(0, 3, 3, 0x1234);
        s.observe(0, 3, 3, 0x1234); // same → no conflict, no growth
        assert_eq!(s.cell_count(0), 1);
        assert_eq!(s.conflicts(0), 0);
    }

    #[test]
    fn revisiting_with_different_code_counts_a_conflict_and_keeps_latest() {
        let mut s = BackgroundStitcher::new();
        s.observe(0, 3, 3, 0x1);
        s.observe(0, 3, 3, 0x2); // animated / changed
        assert_eq!(s.conflicts(0), 1);
        assert_eq!(s.get(0, 3, 3), Some(0x2)); // latest
        assert_eq!(s.representative(0, 3, 3), Some(0x1)); // stable first-seen
    }

    #[test]
    fn classifies_animated_cells() {
        let mut s = BackgroundStitcher::new();
        // (0,0): a static tile seen twice.  (1,0): cycles A→B→A (animated, once).
        s.observe(0, 0, 0, 0x5);
        s.observe(0, 0, 0, 0x5);
        s.observe(0, 1, 0, 0xA);
        s.observe(0, 1, 0, 0xB);
        s.observe(0, 1, 0, 0xA);
        assert_eq!(s.animated_cells(0), 1, "only the cycling cell is animated");
        assert!(!s.is_animated(0, 0, 0));
        assert!(s.is_animated(0, 1, 0));
        assert_eq!(
            s.conflicts(0),
            2,
            "two frame-to-frame changes on the animated cell"
        );
    }

    #[test]
    fn planes_are_independent() {
        let mut s = BackgroundStitcher::new();
        s.observe(0, 0, 0, 1);
        s.observe(1, 0, 0, 2);
        assert_eq!(s.cell_count(0), 1);
        assert_eq!(s.cell_count(1), 1);
        assert_eq!(s.cell_count(2), 0);
        assert_eq!(s.get(0, 0, 0), Some(1));
        assert_eq!(s.get(1, 0, 0), Some(2));
    }

    #[test]
    fn out_of_range_plane_is_ignored() {
        let mut s = BackgroundStitcher::new();
        s.observe(7, 0, 0, 1); // no plane 7
        assert_eq!(s.cell_count(7), 0);
        assert_eq!(s.bounds(7), None);
    }

    ///  last_step reporta el delta de cada push — un salto NO FÍSICO
    /// (corte de escena: título/intro resetean el scroll) es detectable.
    #[test]
    fn unwrapper_reports_last_step() {
        let mut u = ScrollUnwrapper::new(512);
        assert_eq!(u.push(100), 100);
        assert_eq!(u.last_step(), 0, "primer push = sin delta");
        u.push(108);
        assert_eq!(u.last_step(), 8, "paseo fisico");
        u.push(0);
        assert_eq!(u.last_step(), -108, "salto de corte de escena, detectable");
    }

    #[test]
    fn unwrap_recovers_absolute_across_the_wrap() {
        // Camera drifts +6 px/frame; plane = 512. Crosses the wrap boundary.
        let period = 512;
        let mut u = ScrollUnwrapper::new(period);
        let mut real = 500i64;
        u.push(((real % period as i64) + period as i64) as i32 % period); // seed
        for _ in 0..40 {
            real += 6;
            let got = u.push((real % period as i64) as i32);
            assert_eq!(got, real, "unwrapped must track the absolute camera");
        }
        assert!(u.absolute() > period as i64, "crossed the wrap boundary");
    }

    #[test]
    fn unwrap_handles_negative_drift() {
        let period = 512;
        let mut u = ScrollUnwrapper::new(period);
        let mut real = 40i64;
        u.push((((real % period as i64) + period as i64) % period as i64) as i32);
        for _ in 0..20 {
            real -= 5; // scroll left, wraps below 0
            let m = (((real % period as i64) + period as i64) % period as i64) as i32;
            assert_eq!(u.push(m), real);
        }
    }
}
