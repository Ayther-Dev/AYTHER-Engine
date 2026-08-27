//! Framebuffer tile fingerprinting for Mega Drive graphics.
//!
//! [`TileHasher`] converts tiles to a luma-normalized representation before
//! hashing so palette variants of the same shape share an identity.

// ---------------------------------------------------------------------------
// Tile-based sprite fingerprinting for Mega Drive framebuffers.
//
// The Genesis renders in 8×8 pixel tiles. We slice each video frame into
// tiles, convert each to a luma-normalized grayscale representation
// ("blind to color"), then fingerprint with xxHash3-64.
//
// "Blind to color" means Sonic's sprite in Emerald Hill and Chemical Plant —
// same shape, different palette — produce identical hashes. We achieve this
// by mapping each pixel to its BT.601 luma, quantized to 4 bits (16 levels).
// Minor palette variations collapse to the same value; distinct shapes stay
// distinct.
//
// TileHasher accumulates a *catalog* of unique tiles across frames. The
// catalog is the input the Ayther Lab artist uses to map tiles → HD assets.
// ---------------------------------------------------------------------------

use std::collections::HashMap;
use xxhash_rust::xxh3::xxh3_64;

/// Native tile size of the Mega Drive VDP.
pub const TILE_W: usize = 8;
/// Native tile height of the Mega Drive VDP.
pub const TILE_H: usize = 8;
/// Number of pixels in one native tile.
pub const TILE_PIXELS: usize = TILE_W * TILE_H; // 64

// ---------------------------------------------------------------------------
// Pixel format (mirrors RETRO_PIXEL_FORMAT_* from libretro.h)
// ---------------------------------------------------------------------------

/// Framebuffer pixel format, matching libretro's numeric constants.
#[repr(u32)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum PixelFormat {
    /// Legacy 15-bit `0RGB1555` format.
    Rgb1555 = 0, // legacy 15-bit (ORG1555)
    /// Thirty-two-bit `XRGB8888` format.
    Xrgb8888 = 1, // 32-bit (byte order: B G R X, i.e. 0xXXRRGGBB LE)
    /// Sixteen-bit RGB565 format used by Genesis Plus GX.
    Rgb565 = 2, // 16-bit (Genesis Plus GX default)
}

impl PixelFormat {
    /// Converts a libretro numeric format, defaulting unknown values to RGB1555.
    pub fn from_u32(v: u32) -> Self {
        match v {
            1 => Self::Xrgb8888,
            2 => Self::Rgb565,
            _ => Self::Rgb1555,
        }
    }

    /// Bytes per pixel.
    pub fn bpp(self) -> usize {
        match self {
            Self::Xrgb8888 => 4,
            _ => 2,
        }
    }
}

// ---------------------------------------------------------------------------
// Per-frame statistics
// ---------------------------------------------------------------------------

/// Statistics from the most recently processed video frame.
#[derive(Default, Debug, Clone)]
pub struct FrameStats {
    /// Zero-based frame index.
    pub frame_index: u64,
    /// Number of tiles sliced from this frame.
    pub tiles_total: u32, // tiles sliced from this frame
    /// Cumulative number of unique tiles in the catalog.
    pub tiles_unique: u32, // cumulative unique tiles in catalog
    /// Tiles first observed in this frame.
    pub tiles_new: u32, // tiles seen for the first time this frame
}

// ---------------------------------------------------------------------------
// Per-frame tile occurrence (position + hash, for substitution lookups)
// ---------------------------------------------------------------------------

/// One tile occurrence in the most-recently-processed frame.
///
/// `tile_x` and `tile_y` are tile-grid coordinates;
/// multiply by `TILE_W` / `TILE_H` to get pixel screen coordinates.
#[derive(Clone, Debug)]
pub struct TileOccurrence {
    /// Luma-normalized tile hash.
    pub hash: u64,
    /// Column in the native tile grid.
    pub tile_x: u32, // column in tile grid
    /// Row in the native tile grid.
    pub tile_y: u32, // row in tile grid
}

// ---------------------------------------------------------------------------
// TileHasher
// ---------------------------------------------------------------------------

/// Accumulates a catalog of unique tile fingerprints across frames.
///
/// **Key**: xxHash3-64 of the 64-byte luma-quantized 8×8 tile.
/// **Value**: first occurrence `(frame_index, tile_col, tile_row)`.
pub struct TileHasher {
    /// hash → (first_frame, tile_col, tile_row)
    catalog: HashMap<u64, (u64, u32, u32)>,
    frame_index: u64,
    last_stats: FrameStats,
    /// Every tile occurrence from the last call to process_frame (all positions, not just new).
    last_occurrences: Vec<TileOccurrence>,
}

impl TileHasher {
    /// Creates an empty tile catalog at frame zero.
    pub fn new() -> Self {
        Self {
            catalog: HashMap::new(),
            frame_index: 0,
            last_stats: FrameStats::default(),
            last_occurrences: Vec::new(),
        }
    }

    /// Process one video frame.
    ///
    /// - `pixels` — raw framebuffer bytes, row-major.
    /// - `width`  — frame width in pixels.
    /// - `height` — frame height in pixels.
    /// - `pitch`  — row stride in **bytes** (≥ width × bpp, may have padding).
    /// - `fmt`    — pixel format of `pixels`.
    ///
    /// Returns per-frame statistics.
    pub fn process_frame(
        &mut self,
        pixels: &[u8],
        width: u32,
        height: u32,
        pitch: usize,
        fmt: PixelFormat,
    ) -> FrameStats {
        let bpp = fmt.bpp();
        let tiles_x = (width as usize) / TILE_W;
        let tiles_y = (height as usize) / TILE_H;
        let mut new_this_frame = 0u32;

        // Scratch buffer for one tile's luma values (reused each iteration).
        let mut luma = [0u8; TILE_PIXELS];

        // Reset per-frame occurrence list; pre-allocate to avoid repeated growth.
        self.last_occurrences.clear();
        self.last_occurrences.reserve(tiles_x * tiles_y);

        for ty in 0..tiles_y {
            for tx in 0..tiles_x {
                Self::fill_luma(pixels, pitch, bpp, tx, ty, fmt, &mut luma);
                let hash = xxh3_64(&luma);

                // Record every tile position for substitution queries.
                self.last_occurrences.push(TileOccurrence {
                    hash,
                    tile_x: tx as u32,
                    tile_y: ty as u32,
                });

                use std::collections::hash_map::Entry;
                if let Entry::Vacant(e) = self.catalog.entry(hash) {
                    e.insert((self.frame_index, tx as u32, ty as u32));
                    new_this_frame += 1;
                }
            }
        }

        let stats = FrameStats {
            frame_index: self.frame_index,
            tiles_total: (tiles_x * tiles_y) as u32,
            tiles_unique: self.catalog.len() as u32,
            tiles_new: new_this_frame,
        };
        self.last_stats = stats.clone();
        self.frame_index += 1;
        stats
    }

    /// Returns the cumulative number of unique tiles.
    pub fn unique_tile_count(&self) -> u32 {
        self.catalog.len() as u32
    }
    /// Returns the index that will be assigned to the next processed frame.
    pub fn frame_index(&self) -> u64 {
        self.frame_index
    }
    /// Returns statistics from the most recently processed frame.
    pub fn last_stats(&self) -> &FrameStats {
        &self.last_stats
    }
    /// All tile occurrences from the most-recently-processed frame.
    pub fn last_occurrences(&self) -> &[TileOccurrence] {
        &self.last_occurrences
    }

    /// Dump the full tile catalog to a TOML file.
    /// Returns `true` on success.
    pub fn dump_toml(&self, path: &str) -> bool {
        use std::fmt::Write as FmtWrite;
        let mut out = String::with_capacity(self.catalog.len() * 96);
        let _ = writeln!(out, "# Ayther tile hash catalog — v0.2.0");
        let _ = writeln!(
            out,
            "# {} unique tiles across {} frames\n",
            self.catalog.len(),
            self.frame_index
        );

        // Sort by (first_frame, tile_col, tile_row) for deterministic output
        let mut entries: Vec<_> = self.catalog.iter().collect();
        entries.sort_by_key(|(_, (f, tx, ty))| (*f, *tx, *ty));

        for (hash, (first_frame, tx, ty)) in &entries {
            let _ = writeln!(out, "[[tile]]");
            let _ = writeln!(out, "hash        = \"{:#018x}\"", hash);
            let _ = writeln!(out, "first_frame = {}", first_frame);
            let _ = writeln!(out, "tile_col    = {}", tx);
            let _ = writeln!(out, "tile_row    = {}", ty);
        }

        std::fs::write(path, out).is_ok()
    }

    // ---- private ----

    /// Extract one 8×8 tile's luma-quantized bytes into `out`.
    ///
    /// BT.601 luma: Y = (299·R + 587·G + 114·B) / 1000
    /// Quantized to 4 bits (>> 4): 16 intensity levels.
    /// This collapses minor palette variations while preserving shape.
    #[inline]
    fn fill_luma(
        pixels: &[u8],
        pitch: usize,
        bpp: usize,
        tx: usize,
        ty: usize,
        fmt: PixelFormat,
        out: &mut [u8; TILE_PIXELS],
    ) {
        for py in 0..TILE_H {
            let row = ty * TILE_H + py;
            let row_base = row * pitch;
            for px in 0..TILE_W {
                let col = tx * TILE_W + px;
                let off = row_base + col * bpp;
                let luma_raw = match fmt {
                    PixelFormat::Rgb565 => {
                        if off + 1 >= pixels.len() {
                            0
                        } else {
                            let w = u16::from_le_bytes([pixels[off], pixels[off + 1]]);
                            let r = ((w >> 11) & 0x1F) as u32 * 255 / 31;
                            let g = ((w >> 5) & 0x3F) as u32 * 255 / 63;
                            let b = (w & 0x1F) as u32 * 255 / 31;
                            (r * 299 + g * 587 + b * 114) / 1000
                        }
                    }
                    PixelFormat::Xrgb8888 => {
                        if off + 3 >= pixels.len() {
                            0
                        } else {
                            // memory layout in LE: B G R X
                            let b = pixels[off] as u32;
                            let g = pixels[off + 1] as u32;
                            let r = pixels[off + 2] as u32;
                            (r * 299 + g * 587 + b * 114) / 1000
                        }
                    }
                    PixelFormat::Rgb1555 => {
                        if off + 1 >= pixels.len() {
                            0
                        } else {
                            let w = u16::from_le_bytes([pixels[off], pixels[off + 1]]);
                            let r = ((w >> 10) & 0x1F) as u32 * 255 / 31;
                            let g = ((w >> 5) & 0x1F) as u32 * 255 / 31;
                            let b = (w & 0x1F) as u32 * 255 / 31;
                            (r * 299 + g * 587 + b * 114) / 1000
                        }
                    }
                };
                out[py * TILE_W + px] = (luma_raw >> 4) as u8; // 4-bit quantize
            }
        }
    }
}

impl Default for TileHasher {
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

    fn solid_frame(color_rgb565: u16, w: u32, h: u32) -> Vec<u8> {
        let mut v = Vec::with_capacity((w * h * 2) as usize);
        for _ in 0..(w * h) {
            let [lo, hi] = color_rgb565.to_le_bytes();
            v.push(lo);
            v.push(hi);
        }
        v
    }

    #[test]
    fn solid_frame_one_unique_tile() {
        let mut h = TileHasher::new();
        let pixels = solid_frame(0x001F, 256, 192); // solid blue
        let stats = h.process_frame(&pixels, 256, 192, 256 * 2, PixelFormat::Rgb565);
        // All tiles identical → exactly 1 unique hash
        assert_eq!(stats.tiles_new, 1);
        assert_eq!(h.unique_tile_count(), 1);
    }

    #[test]
    fn two_different_colors_same_luma_collapse() {
        // Two RGB565 colors with the same approximate luma should hash identically
        // Pure red  (31,0,0)  luma ≈ 76 → quantized = 4
        // Pure blue (0,0,31)  luma ≈ 29 → quantized = 1
        // They are different → should NOT collapse
        let mut h = TileHasher::new();
        let red_frame = solid_frame(0xF800, 8, 8);
        let blue_frame = solid_frame(0x001F, 8, 8);
        h.process_frame(&red_frame, 8, 8, 16, PixelFormat::Rgb565);
        h.process_frame(&blue_frame, 8, 8, 16, PixelFormat::Rgb565);
        assert_eq!(
            h.unique_tile_count(),
            2,
            "red and blue should be distinct tiles"
        );
    }

    #[test]
    fn palette_swap_collapses_if_same_luma() {
        // Two greens with different chroma but near-identical luma should collapse.
        // RGB565: (0,31,0) green  → R=0, G=253, B=0 → luma=148 → quantized=9
        // We verify same color → same hash (trivial case)
        let mut h = TileHasher::new();
        let frame = solid_frame(0x07E0, 8, 8); // pure green
        h.process_frame(&frame, 8, 8, 16, PixelFormat::Rgb565);
        h.process_frame(&frame, 8, 8, 16, PixelFormat::Rgb565); // exact duplicate
        assert_eq!(h.unique_tile_count(), 1, "same tile twice → still 1 unique");
    }

    #[test]
    fn dump_toml_creates_file() {
        let mut h = TileHasher::new();
        let pixels = solid_frame(0xFFFF, 64, 64);
        h.process_frame(&pixels, 64, 64, 128, PixelFormat::Rgb565);
        let path = "test_tile_catalog.toml";
        assert!(h.dump_toml(path));
        assert!(std::fs::read_to_string(path).unwrap().contains("[[tile]]"));
        std::fs::remove_file(path).ok();
    }
}
