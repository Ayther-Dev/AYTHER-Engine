//! Core services for the AYTHER real-time remastering engine.
//!
//! `ayther_core` provides deterministic identity generation, pack loading and
//! validation, asset substitution, audio-event detection, scripting, and the
//! Rust/C++ integration layer used by the engine and authoring tools.
//!
//! Most consumers should use the safe Rust modules or the typed [`ffi`] bridge.
//! The flat `ayther_*` symbols in this crate are the legacy C ABI and operate on
//! caller-owned buffers and opaque handles.
//!
//! # Main subsystems
//!
//! - [`archive_vfs`] and [`pack_builder`] implement the `.ay` pack format.
//! - [`sprite_hasher`], [`vram_sprite`], and [`audio_event`] derive stable game
//!   identities from video, VDP, and sound-chip observations.
//! - [`tile_substitutor`] and [`audio_hasher`] resolve identities to authored
//!   replacement assets.
//! - [`script_env`] runs sandboxed pack scripts.
//! - [`pack_validate`] checks whether a pack can run in the current session.
//!
//! # FFI safety contract
//!
//! Unsafe C-ABI functions require every non-null pointer to reference a valid,
//! correctly aligned allocation for the size stated by its companion length or
//! capacity argument. Opaque pointers must come from the matching AYTHER
//! constructor, remain alive for the call, and be released exactly once by the
//! matching free function. C strings must be NUL-terminated. Unless a function
//! explicitly states otherwise, input and output regions must not overlap.
#![deny(missing_docs)]
#![deny(rustdoc::broken_intra_doc_links)]
#![deny(unsafe_op_in_unsafe_fn)]

// Raw C ABI wrappers remain for operations whose pointer and zero-copy semantics
// are not represented by the typed `ffi` bridge.

pub mod animation;
pub mod archive_vfs;
pub mod audio_event;
pub mod audio_hasher;
pub mod background;
pub mod cheat_code; // Player-facing Game Genie and PAR codes.
pub mod conditions;
pub mod ffi;
pub mod file_watcher;
pub mod game_profile;
pub mod identity_kat; // Pure identities and known-answer tests.
pub mod instrument_map;
pub mod memory_aob;
pub mod pack_builder;
pub mod pack_credits; // Pack provenance and credits for Play and Hub.
pub mod pack_security;
pub mod pack_trust;
pub mod pack_validate; // Compatibility with the current session.
pub mod ram_anchor;
pub mod rom_patch; // User-supplied IPS/BPS patches applied in memory.
pub mod script_env;
pub mod sf2; // SoundFont-based resynthesis.
pub mod sf2_bake; // Retain only the presets used by a pack.
pub mod sf3; // Convert SF3 Vorbis samples to SF2 at the boundary.
pub mod sfz; // Convert loose SFZ text and samples to SF2 at the boundary.
pub mod shape_hash; // Brightness-independent tile-shape families.
pub mod sprite_hasher;
pub mod tile_substitutor;
pub mod vram_sprite;
pub mod widescreen_gate; // Map game timbres to presets from instruments.toml.

/// AYTHER product release shared by Cargo, CMake, the native SDK, pack
/// compatibility checks, and the Lua API.
pub const RELEASE_VERSION: &str = env!("CARGO_PKG_VERSION");

/// Revision of the legacy flat C ABI.
///
/// This counter is independent from [`RELEASE_VERSION`] and changes only when
/// that ABI contract changes.
pub const CORE_C_ABI_REVISION: u32 = 7;

use audio_hasher::{AudioHasher, AudioOccurrence, AudioSubstitutor};
use vram_sprite::{SpriteHasher, SpriteSubstitutor};

use archive_vfs::AyArchive;
use script_env::ScriptEnv;
use sprite_hasher::{PixelFormat, TileHasher};
use tile_substitutor::TileSubstitutor;

// ===========================================================================
// Version probe
// ===========================================================================

#[unsafe(no_mangle)]
/// Returns the revision of the exported flat C ABI.
pub extern "C" fn ayther_core_version() -> u32 {
    CORE_C_ABI_REVISION
}

// ===========================================================================
// Sonic 2 — RAM reads (68000 work RAM, Big-Endian integers)
// ===========================================================================

/// Read Sonic 2 player X / Y position.
///
/// Offsets (within the 64 KB work-RAM block):
///   0xB008 — player X  (subpixel word, Big-Endian i16)
///   0xB00C — player Y  (subpixel word, Big-Endian i16)
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sonic_read_xy(
    ram: *const u8,
    size: usize,
    out_x: *mut i16,
    out_y: *mut i16,
) -> bool {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        const X_OFF: usize = 0xB008;
        const Y_OFF: usize = 0xB00C;
        if ram.is_null() || size < Y_OFF + 2 {
            return false;
        }
        let buf = std::slice::from_raw_parts(ram, size);
        let x = i16::from_be_bytes([buf[X_OFF], buf[X_OFF + 1]]);
        let y = i16::from_be_bytes([buf[Y_OFF], buf[Y_OFF + 1]]);
        if !out_x.is_null() {
            *out_x = x;
        }
        if !out_y.is_null() {
            *out_y = y;
        }
        true
    }
}

/// Read Sonic 2 player X / Y velocity (fixed-point subpixels per frame).
///
///   0xB014 — X velocity (Big-Endian i16, signed)
///   0xB018 — Y velocity (Big-Endian i16, signed; positive = downward)
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sonic_read_velocity(
    ram: *const u8,
    size: usize,
    out_vx: *mut i16,
    out_vy: *mut i16,
) -> bool {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        const VX_OFF: usize = 0xB014;
        const VY_OFF: usize = 0xB018;
        if ram.is_null() || size < VY_OFF + 2 {
            return false;
        }
        let buf = std::slice::from_raw_parts(ram, size);
        let vx = i16::from_be_bytes([buf[VX_OFF], buf[VX_OFF + 1]]);
        let vy = i16::from_be_bytes([buf[VY_OFF], buf[VY_OFF + 1]]);
        if !out_vx.is_null() {
            *out_vx = vx;
        }
        if !out_vy.is_null() {
            *out_vy = vy;
        }
        true
    }
}

// ===========================================================================
// Mode 3 (RAM anchoring) — C entry point for the mode3_spike harness
// ===========================================================================
//
// Thin raw-pointer wrapper over `ram_anchor::assign_sprites` (kept out of the
// cxx bridge because the geometry is a one-shot pure function taking SoA
// arrays — a hand-written extern "C" is lighter than modelling Vec<T> in the
// bridge, and this is currently the only consumer: the spike that validates
// the world↔screen conversion against a real ROM).
//
// Given the entities' world positions, the camera scroll (read from the VDP by
// the caller) and this frame's SAT sprite top-left screen positions, it fills
// `out_assign[j]` with the index of the entity that claims sprite `j`, or -1 if
// the sprite matches no entity's box. Returns the number of sprites assigned
// (i.e. out_assign[j] >= 0). No-op returning 0 if any required pointer is null.

/// # Safety
/// `ent_x`/`ent_y`/`ent_ids` must point to `n_ent` valid i32/i32/u64 elements;
/// `spr_x`/`spr_y` to `n_spr` valid i16 elements; `out_assign` to `n_spr`
/// writable i32 elements. All slices must outlive the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ayther_mode3_assign_sprites(
    ent_ids: *const u64,
    ent_x: *const i32,
    ent_y: *const i32,
    n_ent: usize,
    scroll_x: i32,
    scroll_y: i32,
    pivot_x: i32,
    pivot_y: i32,
    half_w: i32,
    half_h: i32,
    spr_x: *const i16,
    spr_y: *const i16,
    n_spr: usize,
    out_assign: *mut i32,
) -> usize {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        use crate::ram_anchor::{AnchorBox, AnchoredEntity, assign_sprites};

        if ent_ids.is_null()
            || ent_x.is_null()
            || ent_y.is_null()
            || spr_x.is_null()
            || spr_y.is_null()
            || out_assign.is_null()
        {
            return 0;
        }

        let ids = std::slice::from_raw_parts(ent_ids, n_ent);
        let xs = std::slice::from_raw_parts(ent_x, n_ent);
        let ys = std::slice::from_raw_parts(ent_y, n_ent);
        let sx = std::slice::from_raw_parts(spr_x, n_spr);
        let sy = std::slice::from_raw_parts(spr_y, n_spr);
        let out = std::slice::from_raw_parts_mut(out_assign, n_spr);

        let entities: Vec<AnchoredEntity> = (0..n_ent)
            .map(|i| AnchoredEntity {
                id: ids[i],
                world_x: xs[i],
                world_y: ys[i],
            })
            .collect();
        let sprite_pos: Vec<(i16, i16)> = (0..n_spr).map(|j| (sx[j], sy[j])).collect();
        let b = AnchorBox {
            pivot_x,
            pivot_y,
            half_w,
            half_h,
        };

        let assigns = assign_sprites(&entities, scroll_x, scroll_y, &b, &sprite_pos);

        for slot in out.iter_mut() {
            *slot = -1;
        }
        let mut n_assigned = 0usize;
        for (ei, a) in assigns.iter().enumerate() {
            for &si in &a.members {
                if si < n_spr {
                    out[si] = ei as i32;
                    n_assigned += 1;
                }
            }
        }
        n_assigned
    }
}

// ===========================================================================
// Game profile (Mode 3 input plumbing) — C entry points
// ===========================================================================
//
// Load a TOML anchor profile (game_profile.rs), read entities from work RAM and
// assign this frame's SAT sprites to them. Opaque handle owned by the caller.

use crate::game_profile::GameProfile;

/// Load a game profile from a TOML file. Returns null on read/parse error.
/// # Safety
/// `path` must be a valid NUL-terminated C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ayther_game_profile_load(
    path: *const std::os::raw::c_char,
) -> *mut GameProfile {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if path.is_null() {
            return std::ptr::null_mut();
        }
        let p = std::ffi::CStr::from_ptr(path)
            .to_string_lossy()
            .into_owned();
        match std::fs::read_to_string(&p) {
            Ok(s) => match GameProfile::from_toml_str(&s) {
                Ok(gp) => Box::into_raw(Box::new(gp)),
                Err(_) => std::ptr::null_mut(),
            },
            Err(_) => std::ptr::null_mut(),
        }
    }
}

///  load a game profile from an in-memory TOML string — the pack case,
/// where game_profile.toml lives INSIDE the.ay and never touches disk.
/// Returns null on parse error.
/// # Safety
/// `toml` must be a valid NUL-terminated C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ayther_game_profile_load_str(
    toml: *const std::os::raw::c_char,
) -> *mut GameProfile {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if toml.is_null() {
            return std::ptr::null_mut();
        }
        let s = std::ffi::CStr::from_ptr(toml).to_string_lossy();
        match GameProfile::from_toml_str(&s) {
            Ok(gp) => Box::into_raw(Box::new(gp)),
            Err(_) => std::ptr::null_mut(),
        }
    }
}

/// Free a profile from `ayther_game_profile_load`.
/// # Safety
/// `p` must be a pointer from `ayther_game_profile_load` (or null).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ayther_game_profile_free(p: *mut GameProfile) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !p.is_null() {
            drop(Box::from_raw(p));
        }
    }
}

/// Read the profile's active entities from `ram`, filling parallel output arrays
/// (id / world_x / world_y) up to `cap`. Returns the number written.
/// # Safety
/// Pointers must be valid; `out_*` must hold `cap` elements.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ayther_game_profile_entities(
    p: *const GameProfile,
    ram: *const u8,
    ramsz: usize,
    out_id: *mut u64,
    out_wx: *mut i32,
    out_wy: *mut i32,
    cap: usize,
) -> usize {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if p.is_null() || ram.is_null() || out_id.is_null() {
            return 0;
        }
        let prof = &*p;
        let buf = std::slice::from_raw_parts(ram, ramsz);
        let ids = std::slice::from_raw_parts_mut(out_id, cap);
        let xs = std::slice::from_raw_parts_mut(out_wx, cap);
        let ys = std::slice::from_raw_parts_mut(out_wy, cap);
        let ents = prof.entities(buf);
        let n = ents.len().min(cap);
        for (i, (_, e)) in ents.iter().take(n).enumerate() {
            ids[i] = e.id;
            xs[i] = e.world_x;
            ys[i] = e.world_y;
        }
        n
    }
}

/// Returns the number of anchor kinds in the profile, or zero for null `p`.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_game_profile_kind_count(p: *const GameProfile) -> usize {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe { p.as_ref().map_or(0, |gp| gp.kind_count()) }
}

/// Returns the anchor-kind index for an entity ID, or -1 if none is available.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_game_profile_kind_of_id(p: *const GameProfile, id: u64) -> i32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        p.as_ref()
            .and_then(|gp| gp.kind_of_id(id))
            .map_or(-1, |k| k as i32)
    }
}

/// Copy anchor-kind `idx`'s nombre into `buf` (NUL-terminated, up to `cap`).
/// Returns the nombre's byte length (may exceed cap-1 → grow and retry), or 0.
/// Copies an anchor nombre into `buf` and returns its full byte length.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_game_profile_kind_name(
    p: *const GameProfile,
    idx: usize,
    buf: *mut std::os::raw::c_char,
    cap: usize,
) -> usize {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        let name = match p.as_ref().and_then(|gp| gp.kind_name(idx)) {
            Some(n) => n,
            None => return 0,
        };
        let bytes = name.as_bytes();
        if !buf.is_null() && cap > 0 {
            let n = bytes.len().min(cap - 1);
            std::ptr::copy_nonoverlapping(bytes.as_ptr() as *const std::os::raw::c_char, buf, n);
            *buf.add(n) = 0;
        }
        bytes.len()
    }
}

/// Assign this frame's SAT sprites to the profile's entities. `out_ent_id[j]`
/// receives the id of the entity claiming sprite `j`, or 0 if unassigned.
/// Returns the number of active entities considered.
/// # Safety
/// Pointers must be valid; `spr_*`/`out_ent_id` must hold `n_spr` elements.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ayther_game_profile_assign(
    p: *const GameProfile,
    ram: *const u8,
    ramsz: usize,
    scroll_x: i32,
    scroll_y: i32,
    plane_w: i32,
    plane_h: i32,
    spr_x: *const i16,
    spr_y: *const i16,
    n_spr: usize,
    out_ent_id: *mut u64,
) -> usize {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if p.is_null()
            || ram.is_null()
            || spr_x.is_null()
            || spr_y.is_null()
            || out_ent_id.is_null()
        {
            return 0;
        }
        let prof = &*p;
        let buf = std::slice::from_raw_parts(ram, ramsz);
        let sx = std::slice::from_raw_parts(spr_x, n_spr);
        let sy = std::slice::from_raw_parts(spr_y, n_spr);
        let out = std::slice::from_raw_parts_mut(out_ent_id, n_spr);

        let sprite_pos: Vec<(i16, i16)> = (0..n_spr).map(|j| (sx[j], sy[j])).collect();
        let assigns = prof.assign(buf, scroll_x, scroll_y, plane_w, plane_h, &sprite_pos);

        for slot in out.iter_mut() {
            *slot = 0;
        }
        for a in &assigns {
            for &si in &a.members {
                if si < n_spr {
                    out[si] = a.id;
                }
            }
        }
        assigns.len()
    }
}

// ===========================================================================
// Background stitching and scroll unwrapping C entry points.
// ===========================================================================
//
// Accumulate the visible cells of a scrolling plane into a full level strip
// (background.rs), and unwrap the wrapped VDP Hscroll into an absolute camera.

use crate::background::{BackgroundStitcher, ScrollUnwrapper};

/// Allocate a stitcher. Free with `ayther_bg_stitcher_free`.
#[unsafe(no_mangle)]
pub extern "C" fn ayther_bg_stitcher_new() -> *mut BackgroundStitcher {
    Box::into_raw(Box::new(BackgroundStitcher::new()))
}
/// Frees a background stitcher. Null is accepted as a no-op.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_bg_stitcher_free(p: *mut BackgroundStitcher) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !p.is_null() {
            drop(Box::from_raw(p));
        }
    }
}
/// Records one visible cell at absolute level-tile `(lx, ly)` on `plane` (0/1/2).
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_bg_stitcher_observe(
    p: *mut BackgroundStitcher,
    plane: u8,
    lx: i32,
    ly: i32,
    cell: u32,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if let Some(s) = p.as_mut() {
            s.observe(plane, lx, ly, cell);
        }
    }
}
/// Returns the number of cells observed on `plane`.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_bg_stitcher_cell_count(
    p: *const BackgroundStitcher,
    plane: u8,
) -> usize {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe { p.as_ref().map_or(0, |s| s.cell_count(plane)) }
}
/// Returns the number of conflicting observations on `plane`.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_bg_stitcher_conflicts(
    p: *const BackgroundStitcher,
    plane: u8,
) -> usize {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe { p.as_ref().map_or(0, |s| s.conflicts(plane)) }
}
/// Returns the number of distinct cells classified as animated in place.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_bg_stitcher_animated_cells(
    p: *const BackgroundStitcher,
    plane: u8,
) -> usize {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe { p.as_ref().map_or(0, |s| s.animated_cells(plane)) }
}
/// Fill `out[4]` = {min_x, min_y, max_x, max_y}. Returns false if the plane is
/// empty.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_bg_stitcher_bounds(
    p: *const BackgroundStitcher,
    plane: u8,
    out: *mut i32,
) -> bool {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        match (p.as_ref(), out.is_null()) {
            (Some(s), false) => match s.bounds(plane) {
                Some((a, b, c, d)) => {
                    let o = std::slice::from_raw_parts_mut(out, 4);
                    o[0] = a;
                    o[1] = b;
                    o[2] = c;
                    o[3] = d;
                    true
                }
                None => false,
            },
            _ => false,
        }
    }
}
/// Read the (latest) code at level cell `(x, y)` of `plane` into `out_code`.
/// Returns false if the cell was never seen.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_bg_stitcher_get(
    p: *const BackgroundStitcher,
    plane: u8,
    x: i32,
    y: i32,
    out_code: *mut u32,
) -> bool {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        match (p.as_ref(), out_code.is_null()) {
            (Some(s), false) => match s.get(plane, x, y) {
                Some(code) => {
                    *out_code = code;
                    true
                }
                None => false,
            },
            _ => false,
        }
    }
}

/// Allocate a scroll unwrapper for a plane of `period` pixels.
#[unsafe(no_mangle)]
pub extern "C" fn ayther_scroll_unwrapper_new(period: i32) -> *mut ScrollUnwrapper {
    Box::into_raw(Box::new(ScrollUnwrapper::new(period)))
}
/// Frees a scroll unwrapper. Null is accepted as a no-op.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_scroll_unwrapper_free(p: *mut ScrollUnwrapper) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !p.is_null() {
            drop(Box::from_raw(p));
        }
    }
}
/// Returns the pixel delta from the latest update.
///
/// A non-physical delta (roughly more than 32 pixels per frame) indicates a
/// scene cut, so callers should pause stitch accumulation.
/// Returns whether the unwrapper detected a scene cut.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_scroll_unwrapper_last_step(p: *const ScrollUnwrapper) -> i32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if p.is_null() {
            return 0;
        }
        (*p).last_step()
    }
}

/// Feeds this frame's wrapped scroll (`[0, period)`) and returns the absolute camera.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_scroll_unwrapper_push(
    p: *mut ScrollUnwrapper,
    wrapped: i32,
) -> i64 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe { p.as_mut().map_or(0, |u| u.push(wrapped)) }
}

// ===========================================================================
// TileHasher — opaque-handle C API
// ===========================================================================
//
// Ownership model:
//   ayther_tile_hasher_new()  → caller owns the pointer
//   ayther_tile_hasher_free() → drops the Box, pointer invalid afterwards
//
// Thread safety: NOT thread-safe.  Drive from a single emulation thread.

/// Allocate a new TileHasher on the heap.  Caller must free with
/// `ayther_tile_hasher_free`.
#[unsafe(no_mangle)]
pub extern "C" fn ayther_tile_hasher_new() -> *mut TileHasher {
    Box::into_raw(Box::new(TileHasher::new()))
}

/// Free a TileHasher created by `ayther_tile_hasher_new`.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tile_hasher_free(ptr: *mut TileHasher) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            drop(Box::from_raw(ptr));
        }
    }
}

/// Submit one video frame for tile fingerprinting.
///
/// Returns the number of **new** unique tiles discovered in this frame
/// (0 if the frame contains only tiles already in the catalog).
///
/// `pixel_format` maps to `RETRO_PIXEL_FORMAT_*`:
///   0 = 0RGB1555 (legacy)
///   1 = XRGB8888
///   2 = RGB565   (Genesis Plus GX default)
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tile_hasher_process_frame(
    ptr: *mut TileHasher,
    pixels: *const u8,
    width: u32,
    height: u32,
    pitch: usize,
    pixel_format: u32,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || pixels.is_null() || height == 0 {
            return 0;
        }
        let buf = std::slice::from_raw_parts(pixels, pitch * height as usize);
        let fmt = PixelFormat::from_u32(pixel_format);
        (*ptr)
            .process_frame(buf, width, height, pitch, fmt)
            .tiles_new
    }
}

/// Return the total number of unique tiles accumulated so far.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tile_hasher_unique_count(ptr: *const TileHasher) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return 0;
        }
        (*ptr).unique_tile_count()
    }
}

/// Write the full tile catalog to a TOML file at `path`.
/// Returns `true` on success, `false` on I/O error or null pointer.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tile_hasher_dump_toml(
    ptr: *const TileHasher,
    path: *const std::os::raw::c_char,
) -> bool {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || path.is_null() {
            return false;
        }
        match std::ffi::CStr::from_ptr(path).to_str() {
            Ok(s) => (*ptr).dump_toml(s),
            Err(_) => false,
        }
    }
}

// ===========================================================================
// `AyArchive` opaque-handle C API.
// ===========================================================================
//
// Ownership:
//   ayther_pack_open()  → caller owns the pointer
//   ayther_pack_close() → drops the Box, pointer invalid afterwards
//
// Thread safety: NOT thread-safe.  Use from one thread at a time.

/// Opens a `.ay` pack file. Returns null on any error (missing manifest,
/// bad signature in release builds, corrupt ZIP, etc.).
///
/// Caller must free the returned handle with `ayther_pack_close`.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_open(path: *const std::os::raw::c_char) -> *mut AyArchive {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if path.is_null() {
            return std::ptr::null_mut();
        }
        match std::ffi::CStr::from_ptr(path).to_str() {
            Ok(s) => match AyArchive::open(s) {
                Some(a) => Box::into_raw(Box::new(a)),
                None => std::ptr::null_mut(),
            },
            Err(_) => std::ptr::null_mut(),
        }
    }
}

/// Opens a `.ay` pack using an explicit production public-key registry.
///
/// Both paths must be UTF-8. Returns null when the registry cannot be loaded or
/// when the pack fails container, signature, key-validity, revocation, or scope
/// policy. Caller must free a successful handle with [`ayther_pack_close`].
#[unsafe(no_mangle)]
/// # Safety
///
/// `path` and `trust_registry` must point to readable NUL-terminated strings for
/// the duration of this call.
pub unsafe extern "C" fn ayther_pack_open_trusted(
    path: *const std::os::raw::c_char,
    trust_registry: *const std::os::raw::c_char,
) -> *mut AyArchive {
    // SAFETY: The caller provides readable NUL-terminated strings as documented.
    unsafe {
        if path.is_null() || trust_registry.is_null() {
            return std::ptr::null_mut();
        }
        let Ok(path) = std::ffi::CStr::from_ptr(path).to_str() else {
            return std::ptr::null_mut();
        };
        let Ok(registry_path) = std::ffi::CStr::from_ptr(trust_registry).to_str() else {
            return std::ptr::null_mut();
        };
        let Ok(trust_store) =
            crate::pack_trust::TrustStore::from_path(std::path::Path::new(registry_path))
        else {
            return std::ptr::null_mut();
        };
        match AyArchive::open_with_trust_store(path, &trust_store) {
            Ok(archive) => Box::into_raw(Box::new(archive)),
            Err(_) => std::ptr::null_mut(),
        }
    }
}

/// Free an AyArchive opened with `ayther_pack_open`.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_close(ptr: *mut AyArchive) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            drop(Box::from_raw(ptr));
        }
    }
}

/// Set the active region for transparent asset overrides (e.g. "JP", "PAL").
/// If the region has no override for a given path, the default asset is returned.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_set_region(
    ptr: *mut AyArchive,
    region: *const std::os::raw::c_char,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || region.is_null() {
            return;
        }
        if let Ok(s) = std::ffi::CStr::from_ptr(region).to_str() {
            (*ptr).set_region(s);
        }
    }
}

// ---------------------------------------------------------------------------
// Manifest metadata available without executing the pack.
// ---------------------------------------------------------------------------

/// Returns the manifest schema version written and understood by this build.
///
/// Packs that declare a newer schema are rejected with
/// [`archive_vfs::AyError::UnsupportedSchema`].
#[unsafe(no_mangle)]
pub extern "C" fn ayther_manifest_schema_supported() -> u32 {
    archive_vfs::MANIFEST_SCHEMA
}

/// Returns the `.ay` container format written and understood by this build.
///
/// Packs that declare a newer format are rejected with
/// [`archive_vfs::AyError::UnsupportedFormat`]. This value is independent from
/// [`ayther_manifest_schema_supported`], which versions manifest metadata.
#[unsafe(no_mangle)]
pub extern "C" fn ayther_pack_format_supported() -> u32 {
    archive_vfs::PACK_FORMAT
}

/// Returns the AYTHER release version used to validate a pack's `ayther_min` value.
///
/// The returned string is static and null-terminated. Keeping the value in the
/// core prevents native clients from reporting a version that differs from the
/// one that actually built the pack.
#[unsafe(no_mangle)]
pub extern "C" fn ayther_engine_version() -> *const std::os::raw::c_char {
    concat!(env!("CARGO_PKG_VERSION"), "\0").as_ptr() as *const std::os::raw::c_char
}

/// Returns the schema declared by this pack.
///
/// Legacy packs that predate the field report schema 1. A null pointer returns
/// zero.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_schema(ptr: *const AyArchive) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return 0;
        }
        (*ptr).schema
    }
}

/// Returns the number of subsystems known to this build.
#[unsafe(no_mangle)]
pub extern "C" fn ayther_subsystem_count() -> u32 {
    archive_vfs::SUBSYSTEMS.len() as u32
}

/// Returns the canonical, static, null-terminated nombre of subsystem `i`.
///
/// Returns null when `i` is out of range. Native clients use this list to keep
/// their subsystem enumerations synchronized with the core.
#[unsafe(no_mangle)]
pub extern "C" fn ayther_subsystem_name(i: u32) -> *const std::os::raw::c_char {
    // Static null-terminated strings allow returning non-owning pointers.
    const NAMES: [&str; 8] = [
        "sprites\0",
        "metasprites\0",
        "tiles\0",
        "planes\0",
        "ui\0",
        "music\0",
        "sfx\0",
        "shaders\0",
    ];
    match NAMES.get(i as usize) {
        Some(s) => s.as_ptr() as *const std::os::raw::c_char,
        None => std::ptr::null(),
    }
}

/// Returns the subsystem mask declared by the pack.
///
/// Bit `i` corresponds to [`ayther_subsystem_name`]. A zero mask is deliberately
/// ambiguous: call [`ayther_pack_declares_systems`] to distinguish a legacy pack
/// with no declaration from a pack that explicitly declares no subsystems.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_systems(ptr: *const AyArchive) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return 0;
        }
        (*ptr).systems_mask()
    }
}

/// Returns whether the pack declares a `[systems]` section.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_declares_systems(ptr: *const AyArchive) -> bool {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return false;
        }
        (*ptr).systems_declared
    }
}

/// Returns an authoring or `[compat]` field as a null-terminated C string.
///
/// Supported field nombres are `rom_crc32`, `platform`, `core_min`, `license`, and
/// `contributors`; contributors are comma-separated. The function returns null
/// for an undeclared field, which remains distinct from an explicitly empty
/// value. The pointer remains valid until the next call for the same pack.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_meta_field(
    ptr: *mut AyArchive,
    field: *const std::os::raw::c_char,
) -> *const std::os::raw::c_char {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || field.is_null() {
            return std::ptr::null();
        }
        let key = match std::ffi::CStr::from_ptr(field).to_str() {
            Ok(s) => s,
            Err(_) => return std::ptr::null(),
        };
        let a = &mut *ptr;
        let val: Option<String> = match key {
            "rom_crc32" => a.compat.rom_crc32.clone(),
            "platform" => a.compat.platform.clone(),
            "core_min" => a.compat.core_min.clone(),
            "license" => a.meta.license.clone(),
            "output" => a.meta.output.clone(), // Recommended, not enforced.
            "contributors" => {
                (!a.meta.contributors.is_empty()).then(|| a.meta.contributors.join(","))
            }
            _ => None,
        };
        match val {
            None => std::ptr::null(),
            Some(v) => {
                a.meta_field_cstr = std::ffi::CString::new(v).unwrap_or_default();
                a.meta_field_cstr.as_ptr()
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Compatibility validation before opening the pack.
// ---------------------------------------------------------------------------

/// Runtime-session facts supplied to pack validation through the C ABI.
///
/// Null string pointers and `has_rom == false` mean that the value is unknown.
#[repr(C)]
pub struct AytherValidateCtx {
    /// CRC-32 of the loaded ROM when [`Self::has_rom`] is true.
    pub rom_crc32: u32,
    /// Whether [`Self::rom_crc32`] is available.
    pub has_rom: bool,
    /// Null-terminated platform nombre, or null when unknown.
    pub platform: *const std::os::raw::c_char,
    /// Null-terminated emulator-core build identifier, or null.
    pub core_build_id: *const std::os::raw::c_char,
    /// Null-terminated engine version, or null.
    pub engine_version: *const std::os::raw::c_char,
    /// Whether release signature policy must be enforced.
    pub release_build: bool,
}

/// Opaque pack-validation report queried by index and released with
/// [`ayther_pack_report_free`].
pub struct AytherPackReport {
    findings: Vec<(i32, std::ffi::CString, std::ffi::CString)>, // (sev, code, msg)
    has_err: bool,
}

/// Validates an `.ay` archive against a session without opening it for use.
///
/// Except for an invalid path, incompatibility and archive defects are returned
/// as report findings rather than a null handle. This lets clients reject a pack
/// before it can affect the active session.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_validate(
    path: *const std::os::raw::c_char,
    ctx: *const AytherValidateCtx,
) -> *mut AytherPackReport {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        use crate::pack_validate::{SessionCtx, Severity, validate_path};
        if path.is_null() {
            return std::ptr::null_mut();
        }
        let p = match std::ffi::CStr::from_ptr(path).to_str() {
            Ok(s) => s,
            Err(_) => return std::ptr::null_mut(),
        };

        let cstr = |p: *const std::os::raw::c_char| -> Option<&str> {
            if p.is_null() {
                None
            } else {
                std::ffi::CStr::from_ptr(p).to_str().ok()
            }
        };
        let (rom, plat, core_id, engine, release) = if ctx.is_null() {
            (None, None, None, None, false)
        } else {
            let c = &*ctx;
            (
                c.has_rom.then_some(c.rom_crc32),
                cstr(c.platform),
                cstr(c.core_build_id),
                cstr(c.engine_version),
                c.release_build,
            )
        };
        let sc = SessionCtx {
            rom_crc32: rom,
            platform: plat,
            core_build_id: core_id,
            engine_version: engine,
            release_build: release,
        };
        let rep = validate_path(p, &sc);
        let has_err = rep.has_errors();
        let findings = rep
            .findings
            .into_iter()
            .map(|f| {
                (
                    // Keep recommendations distinct from warnings so callers can present
                    // all three severity levels without weakening actionable warnings.
                    match f.severity {
                        Severity::Error => 0,
                        Severity::Warning => 1,
                        Severity::Info => 2,
                    },
                    std::ffi::CString::new(f.code).unwrap_or_default(),
                    std::ffi::CString::new(f.message).unwrap_or_default(),
                )
            })
            .collect();
        Box::into_raw(Box::new(AytherPackReport { findings, has_err }))
    }
}

#[unsafe(no_mangle)]
/// Returns the number of findings in a validation report.
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_report_count(r: *const AytherPackReport) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if r.is_null() {
            return 0;
        }
        (*r).findings.len() as u32
    }
}

/// Returns the severity of finding `i`: 0 for error, 1 for warning, or -1 when
/// the index is out of range.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_report_severity(r: *const AytherPackReport, i: u32) -> i32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if r.is_null() {
            return -1;
        }
        let report = &*r;
        report.findings.get(i as usize).map(|f| f.0).unwrap_or(-1)
    }
}

#[unsafe(no_mangle)]
/// Returns the stable code of finding `i`, or null when unavailable.
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_report_code(
    r: *const AytherPackReport,
    i: u32,
) -> *const std::os::raw::c_char {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if r.is_null() {
            return std::ptr::null();
        }
        let report = &*r;
        report
            .findings
            .get(i as usize)
            .map(|f| f.1.as_ptr())
            .unwrap_or(std::ptr::null())
    }
}

#[unsafe(no_mangle)]
/// Returns the human-readable mensaje of finding `i`, or null when unavailable.
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_report_message(
    r: *const AytherPackReport,
    i: u32,
) -> *const std::os::raw::c_char {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if r.is_null() {
            return std::ptr::null();
        }
        let report = &*r;
        report
            .findings
            .get(i as usize)
            .map(|f| f.2.as_ptr())
            .unwrap_or(std::ptr::null())
    }
}

/// Returns whether the report contains at least one error.
///
/// Warnings remain available to callers but do not block startup.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_report_has_errors(r: *const AytherPackReport) -> bool {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if r.is_null() {
            return false;
        }
        (*r).has_err
    }
}

#[unsafe(no_mangle)]
/// Frees a validation report returned by [`ayther_pack_validate`].
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_report_free(r: *mut AytherPackReport) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !r.is_null() {
            drop(Box::from_raw(r));
        }
    }
}

// ---------------------------------------------------------------------------
// Compatibility grade exposed through the FFI.
//
// `ay_pack check`, Play, and Hub must all use this verdict so that compatibility
// grades have identical semantics across consumers.
// ---------------------------------------------------------------------------

/// Exact compatibility: all declared requirements were verified.
pub const AYTHER_COMPAT_EXACT: i32 = 0;
/// Compatible with non-blocking warnings.
pub const AYTHER_COMPAT_WARNINGS: i32 = 1;
/// Potentially compatible, but required session facts were unavailable.
pub const AYTHER_COMPAT_EXPERIMENTAL: i32 = 2;
/// Incompatible with the current session.
pub const AYTHER_COMPAT_INCOMPATIBLE: i32 = 3;

/// Opaque compatibility verdict queried through the compatibility accessors and
/// released with [`ayther_compat_free`].
pub struct AytherCompat {
    grade: i32,
    reason: std::ffi::CString,
    json: std::ffi::CString,
    unverified: Vec<std::ffi::CString>,
}

/// Evaluates a pack's compatibility with a session without opening the pack.
///
/// A null `ctx` means no session facts are known, so the result cannot be
/// [`AYTHER_COMPAT_EXACT`].
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_compat(
    path: *const std::os::raw::c_char,
    ctx: *const AytherValidateCtx,
) -> *mut AytherCompat {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        use crate::pack_validate::{CompatGrade, SessionCtx, compat_grade};
        if path.is_null() {
            return std::ptr::null_mut();
        }
        let p = match std::ffi::CStr::from_ptr(path).to_str() {
            Ok(s) => s,
            Err(_) => return std::ptr::null_mut(),
        };

        let cstr = |p: *const std::os::raw::c_char| -> Option<&str> {
            if p.is_null() {
                None
            } else {
                std::ffi::CStr::from_ptr(p).to_str().ok()
            }
        };
        let sc = if ctx.is_null() {
            SessionCtx::default()
        } else {
            let c = &*ctx;
            SessionCtx {
                rom_crc32: c.has_rom.then_some(c.rom_crc32),
                platform: cstr(c.platform),
                core_build_id: cstr(c.core_build_id),
                engine_version: cstr(c.engine_version),
                release_build: c.release_build,
            }
        };

        let v = compat_grade(p, &sc);
        let grade = match v.grade {
            CompatGrade::Exact => AYTHER_COMPAT_EXACT,
            CompatGrade::Warnings => AYTHER_COMPAT_WARNINGS,
            CompatGrade::Experimental => AYTHER_COMPAT_EXPERIMENTAL,
            CompatGrade::Incompatible => AYTHER_COMPAT_INCOMPATIBLE,
        };
        let json = v.to_json();
        Box::into_raw(Box::new(AytherCompat {
            grade,
            reason: std::ffi::CString::new(v.reason).unwrap_or_default(),
            json: std::ffi::CString::new(json).unwrap_or_default(),
            unverified: v
                .unverified
                .iter()
                .map(|u| std::ffi::CString::new(*u).unwrap_or_default())
                .collect(),
        }))
    }
}

#[unsafe(no_mangle)]
/// Returns the numeric compatibility grade, or incompatible for a null handle.
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_compat_grade(c: *const AytherCompat) -> i32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if c.is_null() {
            AYTHER_COMPAT_INCOMPATIBLE
        } else {
            (*c).grade
        }
    }
}

/// Returns a human-readable explanation of the compatibility verdict.
///
/// The explanation is never empty for a valid handle.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_compat_reason(
    c: *const AytherCompat,
) -> *const std::os::raw::c_char {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if c.is_null() {
            std::ptr::null()
        } else {
            (*c).reason.as_ptr()
        }
    }
}

/// Returns the number of compatibility requirements that could not be verified.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_compat_unverified_count(c: *const AytherCompat) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if c.is_null() {
            0
        } else {
            (*c).unverified.len() as u32
        }
    }
}

#[unsafe(no_mangle)]
/// Returns the nombre of unavailable session fact `i`, or null.
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_compat_unverified(
    c: *const AytherCompat,
    i: u32,
) -> *const std::os::raw::c_char {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if c.is_null() {
            return std::ptr::null();
        }
        let v = &(*c).unverified;
        v.get(i as usize).map_or(std::ptr::null(), |s| s.as_ptr())
    }
}

/// Returns the complete compatibility verdict, including its report, as JSON.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_compat_json(c: *const AytherCompat) -> *const std::os::raw::c_char {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if c.is_null() {
            std::ptr::null()
        } else {
            (*c).json.as_ptr()
        }
    }
}

#[unsafe(no_mangle)]
/// Frees a compatibility verdict returned by [`ayther_pack_compat`].
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_compat_free(c: *mut AytherCompat) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !c.is_null() {
            drop(Box::from_raw(c));
        }
    }
}

// ---------------------------------------------------------------------------
// Remaster profiles.
// ---------------------------------------------------------------------------
//
// Packs declare profiles and the Engine applies them; the core only reads them.
// `AyArchive::profiles` guarantees that `original` is first and that exactly one
// profile is the default.

/// Returns the number of profiles offered by the pack.
///
/// A valid pack always exposes at least the implicit `original` profile and one
/// contenido profile.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_profile_count(ptr: *const AyArchive) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return 0;
        }
        (*ptr).profiles().len() as u32
    }
}

/// Returns field `id`, `nombre`, or `description` from profile `i`.
///
/// The returned string is backed by a shared Rust-side cache and remains valid
/// only until the next profile-field query; callers should copy it as needed.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_profile_field(
    ptr: *mut AyArchive,
    i: u32,
    field: *const std::os::raw::c_char,
) -> *const std::os::raw::c_char {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || field.is_null() {
            return std::ptr::null();
        }
        let f = match std::ffi::CStr::from_ptr(field).to_str() {
            Ok(s) => s,
            Err(_) => return std::ptr::null(),
        };
        let all = (*ptr).profiles();
        let p = match all.get(i as usize) {
            Some(p) => p,
            None => return std::ptr::null(),
        };
        let v = match f {
            "id" => p.id.clone(),
            "nombre" => p.display_name().to_string(),
            "description" => p.description.clone().unwrap_or_default(),
            _ => return std::ptr::null(),
        };
        // Reuse the metadata buffer because both APIs expose the same lifetime:
        // the returned pointer remains valid only until the next field query.
        (*ptr).meta_field_cstr = std::ffi::CString::new(v).unwrap_or_default();
        (*ptr).meta_field_cstr.as_ptr()
    }
}

/// Returns the subsystem mask enabled by profile `i`.
///
/// Bit `j` corresponds to `SUBSYSTEMS[j]`; the `original` profile returns zero.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_profile_systems(ptr: *const AyArchive, i: u32) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return 0;
        }
        (*ptr)
            .profiles()
            .get(i as usize)
            .map(|p| p.systems_mask())
            .unwrap_or(0)
    }
}

/// Returns the profile selected when a pack is loaded without an explicit choice.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_default_profile(ptr: *const AyArchive) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return 0;
        }
        let all = (*ptr).profiles();
        all.iter().position(|p| p.default).unwrap_or(0) as u32
    }
}

/// Returns the index of profile `id`, or -1 when it is not present.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_profile_index(
    ptr: *const AyArchive,
    id: *const std::os::raw::c_char,
) -> i32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || id.is_null() {
            return -1;
        }
        let s = match std::ffi::CStr::from_ptr(id).to_str() {
            Ok(s) => s,
            Err(_) => return -1,
        };
        (*ptr)
            .profiles()
            .iter()
            .position(|p| p.id == s)
            .map(|i| i as i32)
            .unwrap_or(-1)
    }
}

/// Returns the audio buses muted by profile `i` as a bit mask.
///
/// Bits use the engine's canonical `AudioBus` order: 0 `Unclassified`, 1
/// `Music`, 2 `Sfx`, and 3 `Voice`. Unknown bus nombres are ignored.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_profile_muted_buses(ptr: *const AyArchive, i: u32) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return 0;
        }
        const BUSES: [&str; 4] = ["unclassified", "music", "sfx", "voice"];
        (*ptr)
            .profiles()
            .get(i as usize)
            .map(|p| {
                let mut m = 0u32;
                for b in &p.muted_buses {
                    let lower = b.to_ascii_lowercase();
                    if let Some(j) = BUSES.iter().position(|x| *x == lower) {
                        m |= 1 << j;
                    }
                }
                m
            })
            .unwrap_or(0)
    }
}

// ---------------------------------------------------------------------------
// Pack credits and provenance for Play and Hub.
// ---------------------------------------------------------------------------

/// Opaque handle owning parsed credits and stable C strings.
///
/// Credits are parsed on demand and released with [`ayther_credits_free`].
pub struct AytherCredits {
    parsed: crate::pack_credits::Credits,
    /// Cached `(author, role, comma-separated licenses)` strings for each credit.
    people: Vec<(std::ffi::CString, std::ffi::CString, std::ffi::CString)>,
    /// Cached attribution strings whose returned pointers must remain valid.
    attribs: std::cell::RefCell<std::collections::HashMap<String, std::ffi::CString>>,
}

/// Reads `credits.toml` from a pack.
///
/// Returns null when the file is absent or malformed. Credits are optional, so
/// either case leaves the pack usable without inventing attribution datos.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_credits(ptr: *const AyArchive) -> *mut AytherCredits {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return std::ptr::null_mut();
        }
        let bytes = match (*ptr).read("credits.toml") {
            Some(b) => b,
            None => return std::ptr::null_mut(),
        };
        let parsed = match crate::pack_credits::Credits::parse(&bytes) {
            Some(c) => c,
            None => return std::ptr::null_mut(),
        };
        let people = parsed
            .credits
            .iter()
            .map(|c| {
                (
                    std::ffi::CString::new(c.author.as_str()).unwrap_or_default(),
                    std::ffi::CString::new(c.role.clone().unwrap_or_default()).unwrap_or_default(),
                    std::ffi::CString::new(c.licenses.join(", ")).unwrap_or_default(),
                )
            })
            .collect();
        Box::into_raw(Box::new(AytherCredits {
            parsed,
            people,
            attribs: std::cell::RefCell::new(std::collections::HashMap::new()),
        }))
    }
}

/// Returns the number of credited contributors, not the number of assets.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_credits_count(c: *const AytherCredits) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if c.is_null() {
            return 0;
        }
        (*c).people.len() as u32
    }
}

#[unsafe(no_mangle)]
/// Returns the contributor nombre at index `i`, or null.
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_credits_author(
    c: *const AytherCredits,
    i: u32,
) -> *const std::os::raw::c_char {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if c.is_null() {
            return std::ptr::null();
        }
        let credits = &*c;
        credits
            .people
            .get(i as usize)
            .map(|p| p.0.as_ptr())
            .unwrap_or(std::ptr::null())
    }
}

/// Returns the declared role or an empty string when no role was provided.
///
/// Null remains reserved for an invalid index.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_credits_role(
    c: *const AytherCredits,
    i: u32,
) -> *const std::os::raw::c_char {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if c.is_null() {
            return std::ptr::null();
        }
        let credits = &*c;
        credits
            .people
            .get(i as usize)
            .map(|p| p.1.as_ptr())
            .unwrap_or(std::ptr::null())
    }
}

/// Returns the contributor's licenses as a comma-separated string.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_credits_licenses(
    c: *const AytherCredits,
    i: u32,
) -> *const std::os::raw::c_char {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if c.is_null() {
            return std::ptr::null();
        }
        let credits = &*c;
        credits
            .people
            .get(i as usize)
            .map(|p| p.2.as_ptr())
            .unwrap_or(std::ptr::null())
    }
}

/// Returns the number of assets attributed to the contributor.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_credits_assets(c: *const AytherCredits, i: u32) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if c.is_null() {
            return 0;
        }
        let credits = &*c;
        credits
            .parsed
            .credits
            .get(i as usize)
            .map(|x| x.assets)
            .unwrap_or(0)
    }
}

/// Returns attribution for `asset_id`, or null when the asset has no provenance.
///
/// The ID is the contenido-derived entry nombre without the `assets/` directory or
/// tier prefix, so all resolution tiers of one image share one attribution.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_credits_attribution(
    c: *const AytherCredits,
    asset_id: *const std::os::raw::c_char,
) -> *const std::os::raw::c_char {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if c.is_null() || asset_id.is_null() {
            return std::ptr::null();
        }
        let id = match std::ffi::CStr::from_ptr(asset_id).to_str() {
            Ok(s) => s,
            Err(_) => return std::ptr::null(),
        };
        let text = match (*c).parsed.attribution_of(id) {
            Some(t) => t.to_string(),
            None => return std::ptr::null(),
        };
        let mut cache = (*c).attribs.borrow_mut();
        let entry = cache
            .entry(id.to_string())
            .or_insert_with(|| std::ffi::CString::new(text).unwrap_or_default());
        entry.as_ptr()
    }
}

#[unsafe(no_mangle)]
/// Frees credits metadata returned by the pack-credits API.
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_credits_free(c: *mut AytherCredits) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !c.is_null() {
            drop(Box::from_raw(c));
        }
    }
}

/// Returns the included resolution tiers as a bit mask.
///
/// Bit `t` denotes tier `t`; zero indicates a legacy pack without `[tiers]`.
/// Tiers are 0 = HD 3×, 1 = Full HD 4.5×, 2 = 4K 9×, and 3 = 8K 18×.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_tiers(ptr: *const AyArchive) -> u8 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return 0;
        }
        (*ptr).tiers_mask()
    }
}

/// Selects the active tier for the display's `ideal` tier.
///
/// The smallest included tier at least as large as `ideal` is preferred, falling
/// back to the largest included tier. Later lookups transparently resolve
/// `tiers/<active>/<nombre>`. Legacy packs are unchanged.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_set_tier(ptr: *mut AyArchive, ideal: i32) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return;
        }
        (*ptr).set_tier(ideal.clamp(0, 7) as u8);
    }
}

/// Maps an output height to its ideal tier and activates it.
///
/// Heights up to 720 select HD, up to 1080 select Full HD, up to 2160 select 4K,
/// and larger outputs select 8K.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_set_tier_for_height(ptr: *mut AyArchive, out_height_px: i32) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return;
        }
        let ideal: u8 = if out_height_px <= 720 {
            0
        } else if out_height_px <= 1080 {
            1
        } else if out_height_px <= 2160 {
            2
        } else {
            3
        };
        (*ptr).set_tier(ideal);
    }
}

/// Return the size in bytes of a logical asset, or -1 if not found.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_file_size(
    ptr: *const AyArchive,
    logical_path: *const std::os::raw::c_char,
) -> i64 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || logical_path.is_null() {
            return -1;
        }
        match std::ffi::CStr::from_ptr(logical_path).to_str() {
            Ok(s) => (*ptr).file_size(s).map(|n| n as i64).unwrap_or(-1),
            Err(_) => -1,
        }
    }
}

/// Read a logical asset into `out_buf` (capacity `buf_cap` bytes).
///
/// Returns the number of bytes written, or -1 if the path is not found
/// or the buffer is too small.
///
/// To pre-allocate: call `ayther_pack_file_size` first.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_read(
    ptr: *const AyArchive,
    logical_path: *const std::os::raw::c_char,
    out_buf: *mut u8,
    buf_cap: usize,
) -> i64 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || logical_path.is_null() || out_buf.is_null() {
            return -1;
        }
        let path = match std::ffi::CStr::from_ptr(logical_path).to_str() {
            Ok(s) => s,
            Err(_) => return -1,
        };
        match (*ptr).read(path) {
            None => -1,
            Some(data) => {
                if data.len() > buf_cap {
                    return -1;
                }
                std::ptr::copy_nonoverlapping(data.as_ptr(), out_buf, data.len());
                data.len() as i64
            }
        }
    }
}

/// Returns whether an entry supports verified range reads.
///
/// Callers can select streaming once and otherwise fall back to
/// [`ayther_pack_read`]. Capability is queried separately because a failed range
/// read can also mean a missing entry, invalid range, or integrity failure.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_entry_streamable(
    ptr: *const AyArchive,
    logical_path: *const std::os::raw::c_char,
) -> bool {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || logical_path.is_null() {
            return false;
        }
        match std::ffi::CStr::from_ptr(logical_path).to_str() {
            Ok(s) => (*ptr).is_streamable(s),
            Err(_) => false,
        }
    }
}

/// Returns the number of entries in the pack.
///
/// Entries are exposed in stable alphabetical order so installers, catalogs,
/// and validators can produce deterministic output.
///
/// # Safety
/// `ptr` must be a live handle returned by [`ayther_pack_open`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ayther_pack_entry_count(ptr: *const AyArchive) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return 0;
        }
        (*ptr).iter_paths().count() as u32
    }
}

/// Copies the nombre of entry `i` into the caller-provided buffer.
///
/// Returns the number of bytes written, excluding the null terminator; zero for
/// an invalid index; or the negative required length when the buffer is too
/// small. Copying avoids exposing a borrowed pointer with archive-dependent
/// lifetime rules.
///
/// # Safety
/// `ptr` must come from [`ayther_pack_open`], and `dst` must reference at least
/// `cap` writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ayther_pack_entry_name(
    ptr: *const AyArchive,
    i: u32,
    dst: *mut std::os::raw::c_char,
    cap: u32,
) -> i32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || dst.is_null() {
            return 0;
        }
        let mut names: Vec<&str> = (*ptr).iter_paths().collect();
        names.sort_unstable();
        let Some(n) = names.get(i as usize) else {
            return 0;
        };
        let bytes = n.as_bytes();
        if bytes.len() + 1 > cap as usize {
            return -(bytes.len() as i32 + 1);
        }
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), dst as *mut u8, bytes.len());
        *dst.add(bytes.len()) = 0;
        bytes.len() as i32
    }
}

/// Reads up to `len` bytes from an entry at `offset` without materializing it.
///
/// The result may be shorter at end of file. Returns -1 when the entry is not
/// range-addressable, the range is invalid, or chunk integrity verification
/// fails; callers should then use [`ayther_pack_read`] where appropriate.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_read_range(
    ptr: *const AyArchive,
    logical_path: *const std::os::raw::c_char,
    offset: u64,
    out_buf: *mut u8,
    len: usize,
) -> i64 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || logical_path.is_null() || out_buf.is_null() {
            return -1;
        }
        let path = match std::ffi::CStr::from_ptr(logical_path).to_str() {
            Ok(s) => s,
            Err(_) => return -1,
        };
        match (*ptr).read_range(path, offset, len) {
            None => -1,
            Some(data) => {
                // `read_range` already clamps to the entry boundary. A short read
                // therefore indicates a bug and must not be silently truncated.
                if data.len() > len {
                    return -1;
                }
                std::ptr::copy_nonoverlapping(data.as_ptr(), out_buf, data.len());
                data.len() as i64
            }
        }
    }
}

/// Return the pack's game_id as a null-terminated C string.
/// The pointer is valid for the lifetime of the AyArchive; do NOT free it.
/// Returns null if `ptr` is null.
///
/// # Implementation note
/// The `CString` is owned by the `AyArchive` struct (`game_id_cstr` field),
/// built once at `open_verbose` and freed when the archive drops.
/// No heap allocation takes place on each call.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_game_id(ptr: *const AyArchive) -> *const std::os::raw::c_char {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return std::ptr::null();
        }
        (*ptr).game_id_cstr.as_ptr()
    }
}

/// Returns the pack build ID identifying one concrete build.
///
/// Legacy packs without `integrity.toml` return an empty string, which callers
/// must treat as unknown. The returned string is owned by the archive and remains
/// valid only while that archive is alive.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_build_id(
    ptr: *const AyArchive,
) -> *const std::os::raw::c_char {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return std::ptr::null();
        }
        (*ptr).build_id_cstr.as_ptr()
    }
}

/// Computes the asset ID used as a file's extension-free nombre inside a pack.
///
/// The ID is the first 32 hexadecimal characters of the contenido SHA-256 digest,
/// shared with `integrity.toml` verification. Files are hashed incrementally so
/// large cinematic masters are not loaded in full. `out` receives 32 characters
/// plus a null terminator and therefore requires 33 bytes. Returns false on an
/// I/O error or insufficient capacity.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_asset_id(
    fs_path: *const std::os::raw::c_char,
    out: *mut std::os::raw::c_char,
    cap: usize,
) -> bool {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if fs_path.is_null() || out.is_null() || cap < 33 {
            return false;
        }
        let path = match std::ffi::CStr::from_ptr(fs_path).to_str() {
            Ok(p) => p,
            Err(_) => return false,
        };
        let hex = match asset_id_of_file(std::path::Path::new(path)) {
            Some(h) => h,
            None => return false,
        };
        std::ptr::copy_nonoverlapping(hex.as_ptr() as *const std::os::raw::c_char, out, hex.len());
        *out.add(hex.len()) = 0;
        true
    }
}

/// Computes an asset ID from bytes already in memory.
///
/// This supports generated assets such as trimmed SoundFonts that do not have a
/// source file and prevents same-named inputs from colliding in a pack.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_asset_id_bytes(
    data: *const u8,
    len: usize,
    out: *mut std::os::raw::c_char,
    cap: usize,
) -> bool {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if out.is_null() || cap < 33 {
            return false;
        }
        if data.is_null() && len != 0 {
            return false;
        }
        let bytes: &[u8] = if len == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(data, len)
        };
        let hex = asset_id_of_bytes(bytes);
        std::ptr::copy_nonoverlapping(hex.as_ptr() as *const std::os::raw::c_char, out, hex.len());
        *out.add(hex.len()) = 0;
        true
    }
}

/// Safe implementation behind [`ayther_asset_id_bytes`].
pub fn asset_id_of_bytes(data: &[u8]) -> String {
    use sha2::{Digest, Sha256};
    format!("{:x}", Sha256::digest(data))[..32].to_string()
}

/// Safe implementation behind [`ayther_asset_id`].
pub fn asset_id_of_file(path: &std::path::Path) -> Option<String> {
    use sha2::{Digest, Sha256};
    use std::io::Read;
    let mut f = std::fs::File::open(path).ok()?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 16];
    loop {
        let n = f.read(&mut buf).ok()?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Some(format!("{:x}", h.finalize())[..32].to_string())
}

// ===========================================================================
// Cross-platform pack-watcher FFI.
// ===========================================================================
//
// C API:
//   AytherPackWatcher* ayther_pack_watcher_new(const char* path);
//   bool               ayther_pack_watcher_poll(AytherPackWatcher* w);
//   void               ayther_pack_watcher_free(AytherPackWatcher* w);
//
// `poll` is non-blocking and drains all pending OS events in one call.
// Call it once per frame; returns true if the watched file changed.

use crate::file_watcher::FileWatcher;

/// Opaque newtype so the C++ side gets a distinct pointer type.
pub struct AytherPackWatcher(FileWatcher);

/// Start watching `path` for modifications.
///
/// Uses ReadDirectoryChangesW on Windows and inotify on Linux.
/// Returns null if the parent directory could not be watched.
/// Free with `ayther_pack_watcher_free`.
///
/// # Safety
///
/// `path` must point to a valid null-terminated C string for the duration of
/// the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ayther_pack_watcher_new(
    path: *const std::os::raw::c_char,
) -> *mut AytherPackWatcher {
    if path.is_null() {
        return std::ptr::null_mut();
    }
    // SAFETY: `path` is non-null and the caller guarantees a valid C string.
    let path_str = unsafe {
        match std::ffi::CStr::from_ptr(path).to_str() {
            Ok(s) => s,
            Err(_) => return std::ptr::null_mut(),
        }
    };
    match FileWatcher::new(path_str) {
        Some(w) => Box::into_raw(Box::new(AytherPackWatcher(w))),
        None => {
            eprintln!(
                "[ayther_core] ayther_pack_watcher_new: \
                       could not watch '{}'",
                path_str
            );
            std::ptr::null_mut()
        }
    }
}

/// Non-blocking poll.  Returns `true` if the watched file was created or
/// modified since the last call.  Drains the OS event queue — safe to call
/// every frame (negligible cost when idle).
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_watcher_poll(w: *mut AytherPackWatcher) -> bool {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if w.is_null() {
            return false;
        }
        (*w).0.has_changed()
    }
}

/// Destroy a watcher created by `ayther_pack_watcher_new`.
/// Passing null is a no-op.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_watcher_free(w: *mut AytherPackWatcher) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !w.is_null() {
            drop(Box::from_raw(w));
        }
    }
}

// ===========================================================================
// `ScriptEnv` opaque-handle C API.
// ===========================================================================
//
// Ownership:
//   ayther_script_new()  → caller owns the pointer
//   ayther_script_free() → drops the Box, pointer invalid afterwards
//
// Typical per-frame call sequence:
//   ayther_script_set_pack(env, pack);          // once on pack load
//   // each frame:
//   ayther_script_on_frame(env, ram, ram_size); // updates RAM + fires callbacks
//
// Thread safety: NOT thread-safe. Drive from the emulation thread only.

/// Creates a sandboxed Lua 5.4 [`ScriptEnv`].
///
/// Returns null if Lua initialization fails.
#[unsafe(no_mangle)]
pub extern "C" fn ayther_script_new() -> *mut ScriptEnv {
    match ScriptEnv::new() {
        Ok(env) => Box::into_raw(Box::new(env)),
        Err(e) => {
            eprintln!("[ayther_core] ScriptEnv::new() failed: {}", e);
            std::ptr::null_mut()
        }
    }
}

/// Destroy a ScriptEnv created with `ayther_script_new`.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_script_free(ptr: *mut ScriptEnv) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            drop(Box::from_raw(ptr));
        }
    }
}

/// Link the loaded.ay pack so Lua can call `ayther.pack.read(path)`.
/// Pass null to detach any previously linked pack.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_script_set_pack(env: *mut ScriptEnv, pack: *const AyArchive) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !env.is_null() {
            (*env).set_pack(pack);
        }
    }
}

/// Load and execute a Lua source string.
///
/// `chunk_name` is used in error messages (e.g. `"scripts/init.lua"`).
/// Returns `true` on success, `false` on syntax / runtime error (details to stderr).
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_script_load_string(
    env: *mut ScriptEnv,
    source: *const std::os::raw::c_char,
    chunk_name: *const std::os::raw::c_char,
) -> bool {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if env.is_null() || source.is_null() {
            return false;
        }
        let src = match std::ffi::CStr::from_ptr(source).to_str() {
            Ok(s) => s,
            Err(_) => return false,
        };
        let name = if chunk_name.is_null() {
            "?"
        } else {
            std::ffi::CStr::from_ptr(chunk_name).to_str().unwrap_or("?")
        };

        match (*env).load_string(src, name) {
            Ok(_) => true,
            Err(e) => {
                eprintln!("[ScriptEnv] load_string '{}': {}", name, e);
                false
            }
        }
    }
}

/// Snapshot `ram_size` bytes from `ram`, then fire all `ayther.on_frame` callbacks.
///
/// Returns the number of callbacks that completed without error (0 if env is null
/// or no callbacks are registered).
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_script_on_frame(
    env: *mut ScriptEnv,
    ram: *const u8,
    ram_size: usize,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if env.is_null() {
            return 0;
        }
        if !ram.is_null() && ram_size > 0 {
            let slice = std::slice::from_raw_parts(ram, ram_size);
            (*env).update_ram(slice);
        }
        (*env).call_on_frame()
    }
}

/// Push the current frame's tile occurrences so `ayther.tiles.list()` returns
/// correct datos inside on_frame callbacks.
///
/// Call this before [`ayther_script_on_frame`] each frame.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_script_update_tiles(
    env: *mut ScriptEnv,
    occs: *const AytherTileOccurrence,
    occ_count: u32,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if env.is_null() {
            return;
        }
        let raw_occs = if occs.is_null() || occ_count == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(occs, occ_count as usize)
        };
        let occ_vec: Vec<sprite_hasher::TileOccurrence> = raw_occs
            .iter()
            .map(|o| sprite_hasher::TileOccurrence {
                hash: o.hash,
                tile_x: o.tile_x,
                tile_y: o.tile_y,
            })
            .collect();
        (*env).update_tiles(&occ_vec);
    }
}

/// Read tile override entries registered by Lua `ayther.tiles.replace()`.
/// Fill `out_buf` with up to `buf_cap` entries.
/// Returns the number of entries written.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_script_get_tile_overrides(
    env: *const ScriptEnv,
    out_buf: *mut AytherTileOverride,
    buf_cap: u32,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if env.is_null() || out_buf.is_null() {
            return 0;
        }
        let overrides = (*env).get_tile_overrides();
        let n = overrides.len().min(buf_cap as usize);
        for (i, (hash, path)) in overrides[..n].iter().enumerate() {
            let entry = &mut *out_buf.add(i);
            entry.hash = *hash;
            let bytes = path.as_bytes();
            let copy_len = bytes.len().min(255);
            std::ptr::write_bytes(entry.asset_path.as_mut_ptr(), 0, 256);
            std::ptr::copy_nonoverlapping(
                bytes.as_ptr() as *const std::os::raw::c_char,
                entry.asset_path.as_mut_ptr(),
                copy_len,
            );
        }
        n as u32
    }
}

/// Push the current frame's sprite occurrences so `ayther.sprites.list()` returns
/// correct datos inside on_frame callbacks.
///
/// Call this before [`ayther_script_on_frame`] each frame.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_script_update_sprites(
    env: *mut ScriptEnv,
    occs: *const AytherSpriteOccurrence,
    occ_count: u32,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if env.is_null() {
            return;
        }
        let raw_occs = if occs.is_null() || occ_count == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(occs, occ_count as usize)
        };
        let occ_vec: Vec<vram_sprite::SpriteOccurrence> = raw_occs
            .iter()
            .map(|o| vram_sprite::SpriteOccurrence {
                hash: o.hash,
                anim_group_id: o.anim_group_id,
                w_tiles: o.w_tiles,
                h_tiles: o.h_tiles,
                screen_x: o.screen_x,
                screen_y: o.screen_y,
                link: o.link,
                palette: o.palette,
                priority: o.priority,
                slot: o.slot,
                hflip: o.hflip,
                vflip: o.vflip,
            })
            .collect();
        (*env).update_sprites(&occ_vec);
    }
}

/// C-compatible sprite override analogous to [`AytherTileOverride`].
#[repr(C)]
pub struct AytherSpriteOverride {
    /// Sprite hash to override.
    pub hash: u64,
    /// Null-terminated logical replacement asset path.
    pub asset_path: [std::os::raw::c_char; 256],
}

/// Read sprite override entries registered by Lua `ayther.sprites.replace()`.
/// Fill `out_buf` with up to `buf_cap` entries.
/// Returns the number of entries written.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_script_get_sprite_overrides(
    env: *const ScriptEnv,
    out_buf: *mut AytherSpriteOverride,
    buf_cap: u32,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if env.is_null() || out_buf.is_null() {
            return 0;
        }
        let overrides = (*env).get_sprite_overrides();
        let n = overrides.len().min(buf_cap as usize);
        for (i, (hash, path)) in overrides[..n].iter().enumerate() {
            let entry = &mut *out_buf.add(i);
            entry.hash = *hash;
            let bytes = path.as_bytes();
            let copy_len = bytes.len().min(255);
            std::ptr::write_bytes(entry.asset_path.as_mut_ptr(), 0, 256);
            std::ptr::copy_nonoverlapping(
                bytes.as_ptr() as *const std::os::raw::c_char,
                entry.asset_path.as_mut_ptr(),
                copy_len,
            );
        }
        n as u32
    }
}

/// Push the current tick's audio occurrences so `ayther.audio.list()` returns
/// correct datos inside on_frame callbacks.
///
/// Call this before [`ayther_script_on_frame`] each tick.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_script_update_audio(
    env: *mut ScriptEnv,
    occs: *const AytherAudioOccurrence,
    occ_count: u32,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if env.is_null() {
            return;
        }
        let raw_occs = if occs.is_null() || occ_count == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(occs, occ_count as usize)
        };
        let occ_vec: Vec<audio_hasher::AudioOccurrence> = raw_occs
            .iter()
            .map(|o| audio_hasher::AudioOccurrence {
                hash: o.hash,
                frame_count: o.frame_count as usize,
                hits: o.hits,
            })
            .collect();
        (*env).update_audio(&occ_vec);
    }
}

/// C-compatible audio override analogous to [`AytherTileOverride`].
#[repr(C)]
pub struct AytherAudioOverride {
    /// Audio-batch hash to override.
    pub hash: u64,
    /// Null-terminated logical replacement asset path.
    pub asset_path: [std::os::raw::c_char; 256],
}

/// Read audio override entries registered by Lua `ayther.audio.replace()`.
/// Fill `out_buf` with up to `buf_cap` entries.
/// Returns the number of entries written.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_script_get_audio_overrides(
    env: *const ScriptEnv,
    out_buf: *mut AytherAudioOverride,
    buf_cap: u32,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if env.is_null() || out_buf.is_null() {
            return 0;
        }
        let overrides = (*env).get_audio_overrides();
        let n = overrides.len().min(buf_cap as usize);
        for (i, (hash, path)) in overrides[..n].iter().enumerate() {
            let entry = &mut *out_buf.add(i);
            entry.hash = *hash;
            let bytes = path.as_bytes();
            let copy_len = bytes.len().min(255);
            std::ptr::write_bytes(entry.asset_path.as_mut_ptr(), 0, 256);
            std::ptr::copy_nonoverlapping(
                bytes.as_ptr() as *const std::os::raw::c_char,
                entry.asset_path.as_mut_ptr(),
                copy_len,
            );
        }
        n as u32
    }
}

// ===========================================================================
// Shader-parameter FFI.
// ===========================================================================

/// C-compatible shader parameter block matching the GLSL push-constant layout.
/// Values in [0, 1].  Defaults: crt_strength=0.0, scan_strength=0.5, vignette=0.2.
#[repr(C)]
pub struct AytherShaderParams {
    /// CRT effect strength in the inclusive range `[0, 1]`.
    pub crt_strength: f32,
    /// Scanline effect strength in the inclusive range `[0, 1]`.
    pub scan_strength: f32,
    /// Vignette effect strength in the inclusive range `[0, 1]`.
    pub vignette: f32,
}

/// Read the shader parameters set by `ayther.shader.set_param()` from Lua.
/// Writes exactly one [`AytherShaderParams`] to `out`.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_script_get_shader_params(
    env: *const ScriptEnv,
    out: *mut AytherShaderParams,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if env.is_null() || out.is_null() {
            return;
        }
        let (crt, scan, vig) = (*env).get_shader_params();
        (*out).crt_strength = crt;
        (*out).scan_strength = scan;
        (*out).vignette = vig;
    }
}

// ===========================================================================
// `TileHasher` per-frame occurrence query.
// ===========================================================================

/// C-compatible tile occurrence for FFI.
#[repr(C)]
pub struct AytherTileOccurrence {
    /// Luma-normalized tile hash.
    pub hash: u64,
    /// Column in the native tile grid.
    pub tile_x: u32,
    /// Row in the native tile grid.
    pub tile_y: u32,
}

/// C-compatible tile override for FFI.
#[repr(C)]
pub struct AytherTileOverride {
    /// Tile hash to override.
    pub hash: u64,
    /// Null-terminated logical replacement asset path.
    pub asset_path: [std::os::raw::c_char; 256],
}

/// C-compatible resolved tile substitution for FFI.
#[repr(C)]
pub struct AytherTileSub {
    /// Null-terminated logical replacement asset path.
    pub asset_path: [std::os::raw::c_char; 256],
    /// Column in the native tile grid.
    pub tile_x: u32,
    /// Row in the native tile grid.
    pub tile_y: u32,
}

/// Retrieve all tile occurrences from the most-recently-processed frame.
///
/// Fills `out_buf` with up to `buf_cap` entries.  Returns the number written.
/// A 320×240 frame produces up to 1200 occurrences (40×30 tiles).
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tile_hasher_get_occurrences(
    ptr: *const TileHasher,
    out_buf: *mut AytherTileOccurrence,
    buf_cap: u32,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || out_buf.is_null() {
            return 0;
        }
        let occs = (*ptr).last_occurrences();
        let n = occs.len().min(buf_cap as usize);
        for (i, occ) in occs[..n].iter().enumerate() {
            *out_buf.add(i) = AytherTileOccurrence {
                hash: occ.hash,
                tile_x: occ.tile_x,
                tile_y: occ.tile_y,
            };
        }
        n as u32
    }
}

// ===========================================================================
// `TileSubstitutor` opaque-handle C API.
// ===========================================================================
//
// Ownership:
//   ayther_tile_sub_new()  → caller owns the pointer
//   ayther_tile_sub_free() → drops the Box, pointer invalid afterwards
//
// Typical per-frame call sequence:
//   // once on pack load:
//   ayther_tile_sub_load_pack(sub, pack);
//
//   // each frame (after ayther_script_on_frame):
//   ayther_tile_sub_clear_overrides(sub);
//   for each override from ayther_script_get_tile_overrides():
//       ayther_tile_sub_add_override(sub, hash, asset_path);
//   n_subs = ayther_tile_sub_resolve(sub, occs, n_occs, out_buf, buf_cap);

/// Create a new TileSubstitutor.  Free with `ayther_tile_sub_free`.
#[unsafe(no_mangle)]
pub extern "C" fn ayther_tile_sub_new() -> *mut TileSubstitutor {
    Box::into_raw(Box::new(TileSubstitutor::new()))
}

/// Destroy a TileSubstitutor.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tile_sub_free(ptr: *mut TileSubstitutor) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            drop(Box::from_raw(ptr));
        }
    }
}

/// Load the substitution catalog from `tile_substitutions.toml` in the pack.
/// Safe to call again if the pack changes.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tile_sub_load_pack(
    ptr: *mut TileSubstitutor,
    pack: *const AyArchive,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || pack.is_null() {
            return;
        }
        (*ptr).load_from_pack(&*pack);
    }
}

/// Register a runtime tile override (from a Lua script).
/// `hash` is the xxHash3-64 tile fingerprint.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tile_sub_add_override(
    ptr: *mut TileSubstitutor,
    hash: u64,
    asset_path: *const std::os::raw::c_char,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || asset_path.is_null() {
            return;
        }
        if let Ok(s) = std::ffi::CStr::from_ptr(asset_path).to_str() {
            (*ptr).add_override(hash, s.to_string());
        }
    }
}

/// Load the catalog from a NAMED toml in the pack (e.g. "plane_tile_substitutions.toml").
/// Uses the same `[[sub]]` format as `tile_substitutions.toml` for background planes.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tile_sub_load_pack_named(
    ptr: *mut TileSubstitutor,
    pack: *const AyArchive,
    file: *const std::os::raw::c_char,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || pack.is_null() || file.is_null() {
            return;
        }
        if let Ok(name) = std::ffi::CStr::from_ptr(file).to_str() {
            (*ptr).load_from_pack_named(&*pack, name);
        }
    }
}

/// Resolves a hash directly to an asset, with overrides taking priority.
///
/// Copies a null-terminated path into `out` and returns true when a mapping is
/// found. This entry point is intended for resolvers that compute placement
/// independently, such as scroll-aware background-plane tiles.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tile_sub_lookup(
    ptr: *const TileSubstitutor,
    hash: u64,
    out: *mut std::os::raw::c_char,
    cap: u32,
) -> bool {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || out.is_null() || cap == 0 {
            return false;
        }
        match (*ptr).lookup(hash) {
            Some(s) => {
                let bytes = s.as_bytes();
                let copy_len = bytes.len().min((cap - 1) as usize);
                std::ptr::copy_nonoverlapping(
                    bytes.as_ptr() as *const std::os::raw::c_char,
                    out,
                    copy_len,
                );
                *out.add(copy_len) = 0;
                true
            }
            None => false,
        }
    }
}

/// Evaluates catalog conditions and selects each hash's active asset for a frame.
///
/// Call once per frame before lookups. Set `word_swapped` when work RAM uses the
/// libretro `addr ^ 1` representation on little-endian hosts. This is a no-op for
/// packs without conditions.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tile_sub_begin_frame(
    ptr: *mut TileSubstitutor,
    frame_number: u64,
    ram: *const u8,
    ram_len: u32,
    word_swapped: bool,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return;
        }
        let slice: &[u8] = if ram.is_null() || ram_len == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(ram, ram_len as usize)
        };
        (*ptr).begin_frame(frame_number, slice, word_swapped);
    }
}

/// Clear all Lua-registered overrides.  Call at the start of each frame.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tile_sub_clear_overrides(ptr: *mut TileSubstitutor) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            (*ptr).clear_overrides();
        }
    }
}

/// Resolve tile occurrences to substitution instructions.
///
/// Fills `out_buf` with up to `buf_cap` entries.
/// Returns the number of substitutions written.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tile_sub_resolve(
    ptr: *const TileSubstitutor,
    occs: *const AytherTileOccurrence,
    occ_count: u32,
    out_buf: *mut AytherTileSub,
    buf_cap: u32,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || out_buf.is_null() {
            return 0;
        }
        let raw_occs = if occs.is_null() || occ_count == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(occs, occ_count as usize)
        };

        let occ_vec: Vec<sprite_hasher::TileOccurrence> = raw_occs
            .iter()
            .map(|o| sprite_hasher::TileOccurrence {
                hash: o.hash,
                tile_x: o.tile_x,
                tile_y: o.tile_y,
            })
            .collect();

        let subs = (*ptr).resolve(&occ_vec);
        let n = subs.len().min(buf_cap as usize);

        for (i, sub) in subs[..n].iter().enumerate() {
            let entry = &mut *out_buf.add(i);
            let bytes = sub.asset_path.as_bytes();
            let copy_len = bytes.len().min(255);
            std::ptr::write_bytes(entry.asset_path.as_mut_ptr(), 0, 256);
            std::ptr::copy_nonoverlapping(
                bytes.as_ptr() as *const std::os::raw::c_char,
                entry.asset_path.as_mut_ptr(),
                copy_len,
            );
            entry.tile_x = sub.tile_x;
            entry.tile_y = sub.tile_y;
        }
        n as u32
    }
}

// ===========================================================================
// `SpriteHasher` opaque-handle C API.
// ===========================================================================
//
// Reads raw VRAM (as exposed by libretro via `retro_memory_data(RETRO_MEMORY_VIDEO_RAM)` = 3)
// and produces position-independent sprite hashes from the Sprite Attribute Table.
//
// Ownership:
//   ayther_sprite_hasher_new()  → caller owns
//   ayther_sprite_hasher_free() → drops the Box

/// C-compatible sprite occurrence for FFI.
///
/// Layout (32 bytes):
///   hash(8) + anim_group_id(8) + w_tiles(1) + h_tiles(1) + screen_x(2) + screen_y(2)
///   + link(1) + palette(1) + priority(1) + slot(1) + hflip(1) + vflip(1)
#[repr(C)]
pub struct AytherSpriteOccurrence {
    /// Palette- and flip-independent sprite hash.
    pub hash: u64,
    /// Detected animation-group identifier, or zero.
    pub anim_group_id: u64,
    /// Sprite width in native tiles.
    pub w_tiles: u8,
    /// Sprite height in native tiles.
    pub h_tiles: u8,
    /// Horizontal screen position in pixels.
    pub screen_x: i16,
    /// Vertical screen position in pixels.
    pub screen_y: i16,
    /// SAT chain link.
    pub link: u8, // SAT link field (metasprite grouping hint)
    /// VDP palette line.
    pub palette: u8, // VDP palette index 0–3
    /// VDP priority bit.
    pub priority: u8, // VDP priority bit (0=low, 1=high) — metasprite front/back
    /// SAT slot index.
    pub slot: u8, // SAT slot index 0–79 (Ayther hide-by-hash)
    /// VDP horizontal-flip bit.
    pub hflip: u8, // VDP horizontal flip for automatic HD-sheet mirroring.
    /// VDP vertical-flip bit.
    pub vflip: u8, // VDP v-flip
}

/// C-compatible resolved sprite substitution for FFI.
#[repr(C)]
pub struct AytherSpriteSub {
    /// Null-terminated logical replacement asset path.
    pub asset_path: [std::os::raw::c_char; 256],
    /// Horizontal destination position in pixels.
    pub screen_x: i16,
    /// Vertical destination position in pixels.
    pub screen_y: i16,
    /// Destination width in native tiles.
    pub w_tiles: u8,
    /// Destination height in native tiles.
    pub h_tiles: u8,
    /// Exact destination width in pixels, or zero to derive it from tiles.
    pub w_px: u16,
    /// Exact destination height in pixels, or zero to derive it from tiles.
    pub h_px: u16,
    /// Pose-match transform: bit 0 is horizontal mirror and bit 1 is vertical
    /// mirror; zero is the captured orientation.
    pub mirror: u8,
    /// Palette observed on the sprite or pose anchor, or `0xFF` when unknown.
    ///
    /// Color modulation is anchored here rather than to an unrelated overlapping
    /// occurrence in the bounding box.
    pub palette: u8,
    /// Authored palette of the selected candidate, or `0xFF` when no palette
    /// synthesis is required.
    pub synth_pal: u8,
    /// Authored E1 tint reference: average 0–255 RGB of the CRAM line captured
    /// with the pose. `[0, 0, 0]` selects scalar peak-hold fallback.
    pub ref_rgb: [u8; 3],
    /// Horizontal origin of the normalized asset sub-rectangle.
    ///
    /// Mixed-palette pose groups use these UV bounds to select their portion of
    /// the replacement asset; `(0, 0, 1, 1)` denotes the full image.
    pub u0: f32,
    /// Vertical origin of the normalized asset sub-rectangle.
    pub v0: f32,
    /// Width of the normalized asset sub-rectangle.
    pub uw: f32,
    /// Height of the normalized asset sub-rectangle.
    pub vh: f32,
    /// Stable identity of the pose that emitted this substitution.
    ///
    /// [`vram_sprite::TweenPlayer`] uses it to track instances. This trailing ABI
    /// field requires the core and its native clients to be rebuilt together when
    /// changed.
    pub pose_key: u64,
    /// Path or asset ID of the base pose's costume-tint mask, or an empty string
    /// when no mask is available. This trailing field follows the same ABI rule
    /// as `pose_key`.
    pub mask_path: [std::os::raw::c_char; 256],
}

// Compile-time ABI guard: changing this structure on only one side of the FFI
// boundary must fail the build.
const _: () = assert!(std::mem::size_of::<AytherSpriteSub>() == 552);

/// Allocate a new SpriteHasher.  Free with `ayther_sprite_hasher_free`.
#[unsafe(no_mangle)]
pub extern "C" fn ayther_sprite_hasher_new() -> *mut SpriteHasher {
    Box::into_raw(Box::new(SpriteHasher::new()))
}

/// Destroy a SpriteHasher.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sprite_hasher_free(ptr: *mut SpriteHasher) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            drop(Box::from_raw(ptr));
        }
    }
}

/// Process one frame of raw VRAM.
///
/// `vram`      — pointer to the full VRAM buffer (typically 64 KB for Mega Drive).
/// `vram_size` — size in bytes.
/// `sat_base`  — SAT start offset, or `usize::MAX` (AYTHER_SAT_AUTODETECT) to
///               derive it from VRAM structure each frame (recommended; the MD
///               SAT base is set by VDP reg $5 and varies by game/mode).
///
/// Returns the number of *new* unique sprites discovered this frame.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sprite_hasher_process_vram(
    ptr: *mut SpriteHasher,
    vram: *const u8,
    vram_size: usize,
    sat_base: usize,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || vram.is_null() || vram_size == 0 {
            return 0;
        }
        let buf = std::slice::from_raw_parts(vram, vram_size);
        (*ptr).process_vram(buf, sat_base)
    }
}

/// Process the sprites the VDP actually parsed this frame (the fork captures them in
/// parse_satb — AYTHER_MEMORY_PARSED_SPRITES). `sprites` is `count` records of 8
/// bytes each (yr/xr/attr u16 LE + w/h u8). The authoritative "what was drawn"
/// source — robust to mid-frame SAT rewrites/base swaps (Aladdin's Sega-logo genie).
/// Tiles hashed from `vram`. count == 0 → returns 0 (caller falls back).
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sprite_hasher_process_sprites(
    ptr: *mut SpriteHasher,
    sprites: *const u8,
    count: usize,
    vram: *const u8,
    vram_size: usize,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || vram.is_null() || vram_size == 0 {
            return 0;
        }
        if sprites.is_null() && count != 0 {
            return 0;
        }
        // A zero count is a valid frame with no sprites, not an error. Processing an
        // empty list clears previous-frame occurrences and prevents a fallback SAT
        // scan from exposing stale entries that the game did not clear.
        // Entries are 10-byte `AytherSpr` records:
        // `yr`, `xr`, `attr`, `w`, `h`, `sat_idx`, and `chain_pos`.
        let data = if count == 0 {
            &[][..]
        } else {
            std::slice::from_raw_parts(sprites, count * 10)
        };
        let v = std::slice::from_raw_parts(vram, vram_size);
        (*ptr).process_parsed_sprites(data, v)
    }
}

/// Return the total number of unique sprite patterns accumulated so far.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sprite_hasher_unique_count(ptr: *const SpriteHasher) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return 0;
        }
        (*ptr).unique_sprite_count()
    }
}

/// Allocate an `AudioEventDetector`. Free with `ayther_audio_evdet_free`.
#[unsafe(no_mangle)]
pub extern "C" fn ayther_audio_evdet_new() -> *mut audio_event::BatchEventDetector {
    Box::into_raw(Box::new(audio_event::BatchEventDetector::new()))
}

/// Destroy an `AudioEventDetector`.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_evdet_free(ptr: *mut audio_event::BatchEventDetector) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            drop(Box::from_raw(ptr));
        }
    }
}

/// Toggle re-attack splitting (default on): split a run when the sound's head
/// recurs (a retrigger with no intervening silence) into distinct instances.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_evdet_set_split_on_reattack(
    ptr: *mut audio_event::BatchEventDetector,
    on: bool,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            (*ptr).set_split_on_reattack(on);
        }
    }
}

/// Feed one batch hash (`0` = silent). Call once per audio batch (≈ per frame).
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_evdet_push(
    ptr: *mut audio_event::BatchEventDetector,
    hash: u64,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            (*ptr).push(hash);
        }
    }
}

/// Close any in-flight run (end of stream / recording).
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_evdet_flush(ptr: *mut audio_event::BatchEventDetector) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            (*ptr).flush();
        }
    }
}

/// Number of completed events.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_evdet_event_count(
    ptr: *const audio_event::BatchEventDetector,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            0
        } else {
            (&*ptr).event_count() as u32
        }
    }
}

/// Copy completed events into `out_buf` (up to `cap`). Returns the total event
/// count (may exceed `cap` — grow and retry).
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_evdet_get_events(
    ptr: *const audio_event::BatchEventDetector,
    out_buf: *mut AytherAudioEvent,
    cap: u32,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return 0;
        }
        let events = (&*ptr).events();
        if !out_buf.is_null() && cap > 0 {
            let n = events.len().min(cap as usize);
            for (i, e) in events[..n].iter().enumerate() {
                *out_buf.add(i) = AytherAudioEvent {
                    signature: e.signature,
                    instrument: e.signature, // PCM detection has no parche; use the signature.
                    start_frame: e.start_frame as u32,
                    end_frame: e.end_frame as u32,
                    chip: 255, // 255 identifies batch detection without a source channel.
                    channel: 0,
                    pitch: audio_event::NO_PITCH,
                    velocity: audio_event::NO_VELOCITY,
                };
            }
        }
        events.len() as u32
    }
}

/// Fill `out_buf` with the sprite occurrences from the last processed frame.
/// Returns the number of entries written (up to `buf_cap`).
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sprite_hasher_get_occurrences(
    ptr: *const SpriteHasher,
    out_buf: *mut AytherSpriteOccurrence,
    buf_cap: u32,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || out_buf.is_null() {
            return 0;
        }
        let occs = (*ptr).last_occurrences();
        let n = occs.len().min(buf_cap as usize);
        for (i, occ) in occs[..n].iter().enumerate() {
            *out_buf.add(i) = AytherSpriteOccurrence {
                hash: occ.hash,
                anim_group_id: occ.anim_group_id,
                w_tiles: occ.w_tiles,
                h_tiles: occ.h_tiles,
                screen_x: occ.screen_x,
                screen_y: occ.screen_y,
                link: occ.link,
                palette: occ.palette,
                priority: occ.priority,
                slot: occ.slot,
                hflip: occ.hflip,
                vflip: occ.vflip,
            };
        }
        n as u32
    }
}

// ---------------------------------------------------------------------------
// Animation clips: ordered pose sequences and timing per animation group.
//
// The C ABI mirror of `vram_sprite::AnimFrame`. The `SpriteHasher` detects
// looping cycles and consolidates each into an ordered clip. The Animation
// workspace uses these values as authoring defaults.
// ---------------------------------------------------------------------------

/// One frame (pose) of a detected clip. 16 bytes, align 8:
///   pose(8) | duration(2) | _pad(6)
#[repr(C)]
pub struct AytherAnimFrame {
    /// Stable pose hash.
    pub pose: u64,
    /// Number of emulated frames for which the pose is held.
    pub duration: u16,
}

/// Returns the number of animation clips detected by the latest recomputation.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sprite_hasher_clip_count(ptr: *const SpriteHasher) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            0
        } else {
            (*ptr).animation_clips().len() as u32
        }
    }
}

/// Resets the animation detector, including slot history, groups, and clips.
///
/// Call this before clip generation so results reflect only the recording being scanned.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sprite_hasher_reset_animation_grouper(ptr: *mut SpriteHasher) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            (*ptr).reset_animation_grouper();
        }
    }
}

/// Read animation clip `index`. Writes `out_id` (= anim_group_id) and
/// `out_looping` (0/1), and up to `frames_cap` frames into `out_frames` in order.
/// Returns the clip's frame count (may exceed `frames_cap` — grow and retry), or
/// [`u32::MAX`] if `index` is out of range.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sprite_hasher_get_clip(
    ptr: *const SpriteHasher,
    index: u32,
    out_id: *mut u64,
    out_looping: *mut u8,
    out_frames: *mut AytherAnimFrame,
    frames_cap: u32,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return u32::MAX;
        }
        let clips = (*ptr).animation_clips();
        let clip = match clips.get(index as usize) {
            Some(c) => c,
            None => return u32::MAX,
        };
        if !out_id.is_null() {
            *out_id = clip.id;
        }
        if !out_looping.is_null() {
            *out_looping = clip.looping as u8;
        }
        if !out_frames.is_null() && frames_cap > 0 {
            let n = clip.frames.len().min(frames_cap as usize);
            for (i, f) in clip.frames[..n].iter().enumerate() {
                *out_frames.add(i) = AytherAnimFrame {
                    pose: f.pose,
                    duration: f.duration,
                };
            }
        }
        clip.frames.len() as u32
    }
}

// Geometric-tween opaque-handle C API.
//
// `animation::Transform` is already `#[repr(C)]` (4×f32 = 16 B); expose it as
// `AytherTransform` in the header. The engine builds a tween from a clip's
// per-keyframe bboxes + durations and samples it each render tick to glide the
// HD asset between keyframes instead of popping.

/// Builds a geometric tween from `n` keyframe transforms and hold durations.
///
/// When `looping` is true, the last key wraps to the first. Returns null when
/// `n == 0`.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_geo_tween_new(
    transforms: *const animation::Transform,
    durs: *const u16,
    n: usize,
    looping: bool,
) -> *mut animation::GeometricTween {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if transforms.is_null() || durs.is_null() || n == 0 {
            return std::ptr::null_mut();
        }
        let tf = std::slice::from_raw_parts(transforms, n);
        let du = std::slice::from_raw_parts(durs, n);
        let keys = (0..n).map(|i| (tf[i], du[i])).collect();
        match animation::GeometricTween::new(keys, looping) {
            Some(t) => Box::into_raw(Box::new(t)),
            None => std::ptr::null_mut(),
        }
    }
}

/// Frees a tween returned by [`ayther_geo_tween_new`]. Null is accepted as a no-op.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_geo_tween_free(p: *mut animation::GeometricTween) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !p.is_null() {
            drop(Box::from_raw(p));
        }
    }
}

/// Returns the number of ticks in one tween cycle.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_geo_tween_duration(p: *const animation::GeometricTween) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe { p.as_ref().map_or(0, |t| t.duration()) }
}

/// Returns the interpolated transform at `tick`.
///
/// Looping tweens wrap and one-shot tweens clamp at the final transform.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_geo_tween_sample(
    p: *const animation::GeometricTween,
    tick: u32,
) -> animation::Transform {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        p.as_ref().map_or(
            animation::Transform {
                x: 0.0,
                y: 0.0,
                w: 0.0,
                h: 0.0,
            },
            |t| t.sample(tick),
        )
    }
}

// ===========================================================================
// `SpriteSubstitutor` opaque-handle C API.
// ===========================================================================

/// Allocate a new SpriteSubstitutor.  Free with `ayther_sprite_sub_free`.
#[unsafe(no_mangle)]
pub extern "C" fn ayther_sprite_sub_new() -> *mut SpriteSubstitutor {
    Box::into_raw(Box::new(SpriteSubstitutor::new()))
}

/// Destroy a SpriteSubstitutor.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sprite_sub_free(ptr: *mut SpriteSubstitutor) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            drop(Box::from_raw(ptr));
        }
    }
}

/// Load substitution catalog from `sprite_substitutions.toml` inside the pack.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sprite_sub_load_pack(
    ptr: *mut SpriteSubstitutor,
    pack: *const AyArchive,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || pack.is_null() {
            return;
        }
        (*ptr).load_from_pack(&*pack);
    }
}

/// Add a runtime override (e.g. from a Lua script).
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sprite_sub_add_override(
    ptr: *mut SpriteSubstitutor,
    hash: u64,
    asset_path: *const std::os::raw::c_char,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || asset_path.is_null() {
            return;
        }
        if let Ok(s) = std::ffi::CStr::from_ptr(asset_path).to_str() {
            (*ptr).add_override(hash, s.to_string());
        }
    }
}

/// Add a runtime override WITH the authored E1 chromatic reference:
/// `ref_rgb` points to 3 bytes (CRAM line average at assign time) or is null
/// (`[0, 0, 0]` makes the engine fall back to the scalar grey peak-hold).
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sprite_sub_add_override_ref(
    ptr: *mut SpriteSubstitutor,
    hash: u64,
    asset_path: *const std::os::raw::c_char,
    ref_rgb: *const u8,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || asset_path.is_null() {
            return;
        }
        if let Ok(s) = std::ffi::CStr::from_ptr(asset_path).to_str() {
            let r = if ref_rgb.is_null() {
                [0, 0, 0]
            } else {
                [*ref_rgb, *ref_rgb.add(1), *ref_rgb.add(2)]
            };
            (*ptr).add_override_ref(hash, s.to_string(), r);
        }
    }
}

/// Clear all Lua-registered runtime overrides.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sprite_sub_clear_overrides(ptr: *mut SpriteSubstitutor) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            (*ptr).clear_overrides();
        }
    }
}

/// Resolve sprite occurrences to HD substitution instructions.
///
/// Fills `out_buf` with up to `buf_cap` entries.  Returns count written.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sprite_sub_resolve(
    ptr: *const SpriteSubstitutor,
    occs: *const AytherSpriteOccurrence,
    occ_count: u32,
    out_buf: *mut AytherSpriteSub,
    buf_cap: u32,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || out_buf.is_null() {
            return 0;
        }
        let raw_occs = if occs.is_null() || occ_count == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(occs, occ_count as usize)
        };

        let occ_vec: Vec<vram_sprite::SpriteOccurrence> = raw_occs
            .iter()
            .map(|o| vram_sprite::SpriteOccurrence {
                hash: o.hash,
                anim_group_id: o.anim_group_id,
                w_tiles: o.w_tiles,
                h_tiles: o.h_tiles,
                screen_x: o.screen_x,
                screen_y: o.screen_y,
                link: o.link,
                palette: o.palette,
                priority: o.priority,
                slot: o.slot,
                hflip: o.hflip,
                vflip: o.vflip,
            })
            .collect();

        let subs = (*ptr).resolve(&occ_vec);
        let n = subs.len().min(buf_cap as usize);

        for (i, sub) in subs[..n].iter().enumerate() {
            let entry = &mut *out_buf.add(i);
            let bytes = sub.asset_path.as_bytes();
            let copy_len = bytes.len().min(255);
            std::ptr::write_bytes(entry.asset_path.as_mut_ptr(), 0, 256);
            std::ptr::copy_nonoverlapping(
                bytes.as_ptr() as *const std::os::raw::c_char,
                entry.asset_path.as_mut_ptr(),
                copy_len,
            );
            entry.screen_x = sub.screen_x;
            entry.screen_y = sub.screen_y;
            entry.w_tiles = sub.w_tiles;
            entry.h_tiles = sub.h_tiles;
            entry.w_px = sub.w_px;
            entry.h_px = sub.h_px;
            entry.mirror = sub.mirror;
            entry.palette = sub.palette;
            entry.synth_pal = sub.synth_pal;
            entry.ref_rgb = sub.ref_rgb;
            entry.u0 = sub.u0;
            entry.v0 = sub.v0;
            entry.uw = sub.uw;
            entry.vh = sub.vh;
            entry.pose_key = sub.pose_key;
            let mbytes = sub.mask_path.as_bytes();
            let mlen = mbytes.len().min(255);
            std::ptr::write_bytes(entry.mask_path.as_mut_ptr(), 0, 256);
            std::ptr::copy_nonoverlapping(
                mbytes.as_ptr() as *const std::os::raw::c_char,
                entry.mask_path.as_mut_ptr(),
                mlen,
            );
        }
        n as u32
    }
}

// ---------------------------------------------------------------------------
// `PoseSetSubstitutor` resolves animated multi-sprite pose signatures before
// per-sprite substitution and claims their members as one HD asset.
// ---------------------------------------------------------------------------
use vram_sprite::PoseSetSubstitutor;

#[unsafe(no_mangle)]
/// Allocates an empty whole-pose substitutor.
pub extern "C" fn ayther_pose_sub_new() -> *mut PoseSetSubstitutor {
    Box::into_raw(Box::new(PoseSetSubstitutor::new()))
}
#[unsafe(no_mangle)]
/// Frees a whole-pose substitutor.
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pose_sub_free(ptr: *mut PoseSetSubstitutor) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            drop(Box::from_raw(ptr));
        }
    }
}
#[unsafe(no_mangle)]
/// Loads pose substitutions from a pack and returns the catalog size.
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pose_sub_load_pack(
    ptr: *mut PoseSetSubstitutor,
    pack: *const AyArchive,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || pack.is_null() {
            return 0;
        }
        (*ptr).load_from_pack(&*pack);
        (*ptr).catalog_len() as u32
    }
}
/// Adds a live pose override from a set of hashes to an asset.
///
/// Non-null `rel_x` and `rel_y` arrays describe an instantiated pose that matches
/// only at those exact relative offsets. Optional `dim_w` and `dim_h` arrays give
/// each member's pixel dimensions for accurate off-screen tolerance.
/// `base_mirror` describes the asset orientation relative to the capture and is
/// XORed with the detected transform. Optional `ref_rgb` supplies the authored E1
/// tint reference; null or `[0, 0, 0]` selects scalar peak-hold fallback. Live
/// overrides take priority over the pack catalog.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pose_sub_add_override(
    ptr: *mut PoseSetSubstitutor,
    hashes: *const u64,
    rel_x: *const i16,
    rel_y: *const i16,
    dim_w: *const i16,
    dim_h: *const i16,
    mem_flips: *const u8, // Per-member SAT flips (bit 0 H, bit 1 V), or null.
    n: u32,
    max_w: u16,
    max_h: u16,
    base_mirror: u8,
    ref_rgb: *const u8,
    ref_lines: *const u8, // Twelve bytes (four RGB lines), or null.
    asset: *const std::os::raw::c_char,
    mask: *const std::os::raw::c_char, // Costume mask; null or empty means none.
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || hashes.is_null() || asset.is_null() || n == 0 {
            return;
        }
        let hv: Vec<u64> = std::slice::from_raw_parts(hashes, n as usize).to_vec();
        let rel = if !rel_x.is_null() && !rel_y.is_null() {
            let rx = std::slice::from_raw_parts(rel_x, n as usize);
            let ry = std::slice::from_raw_parts(rel_y, n as usize);
            Some(rx.iter().zip(ry.iter()).map(|(&x, &y)| (x, y)).collect())
        } else {
            None
        };
        let dims = if !dim_w.is_null() && !dim_h.is_null() {
            let dw = std::slice::from_raw_parts(dim_w, n as usize);
            let dh = std::slice::from_raw_parts(dim_h, n as usize);
            Some(dw.iter().zip(dh.iter()).map(|(&w, &h)| (w, h)).collect())
        } else {
            None
        };
        let flips = if !mem_flips.is_null() {
            Some(std::slice::from_raw_parts(mem_flips, n as usize).to_vec())
        } else {
            None
        };
        let a = match std::ffi::CStr::from_ptr(asset).to_str() {
            Ok(s) => s.to_string(),
            Err(_) => return,
        };
        let rr = if ref_rgb.is_null() {
            [0, 0, 0]
        } else {
            [*ref_rgb, *ref_rgb.add(1), *ref_rgb.add(2)]
        };
        let rl = read_ref_lines(ref_lines);
        let m = read_mask(mask);
        (*ptr).add_override_variants(
            hv,
            rel,
            dims,
            flips,
            max_w,
            max_h,
            base_mirror,
            rr,
            rl,
            a,
            m,
            Vec::new(),
        );
    }
}

/// Sets the costume-tint mask path; null or invalid input means no mask.
unsafe fn read_mask(p: *const std::os::raw::c_char) -> String {
    // SAFETY: A non-null `p` is guaranteed by the caller to reference a valid,
    // null-terminated C string for the duration of this call.
    unsafe {
        if p.is_null() {
            return String::new();
        }
        std::ffi::CStr::from_ptr(p)
            .to_str()
            .map(str::to_string)
            .unwrap_or_default()
    }
}

/// Sets per-line E1 references from 12 bytes: four RGB triplets.
///
/// A null pointer clears all per-line references.
unsafe fn read_ref_lines(p: *const u8) -> [[u8; 3]; 4] {
    // SAFETY: A non-null `p` is guaranteed by the caller to reference at least
    // twelve readable bytes.
    unsafe {
        let mut out = [[0u8; 3]; 4];
        if !p.is_null() {
            for (line, channels) in out.iter_mut().enumerate() {
                for (channel, value) in channels.iter_mut().enumerate() {
                    *value = *p.add(line * 3 + channel);
                }
            }
        }
        out
    }
}

/// Adds a live pose override with variant candidates.
///
/// `default_asset` is the fallback. Candidate palette, horizontal-flip, and
/// vertical-flip selectors are parallel `i8` arrays where -1 is a wildcard,
/// accompanied by an array of asset paths. Resolution chooses the closest
/// candidate to the observed anchor variant; palettes are matched and flips may
/// be synthesized.
///
/// # Safety
/// Hash and relative-position pointers must be valid for `n` elements. Candidate
/// arrays and paths must be valid for `n_var` elements.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ayther_pose_sub_add_override_variants(
    ptr: *mut PoseSetSubstitutor,
    hashes: *const u64,
    rel_x: *const i16,
    rel_y: *const i16,
    dim_w: *const i16,
    dim_h: *const i16,
    mem_flips: *const u8, // Per-member SAT flips (bit 0 H, bit 1 V), or null.
    n: u32,
    max_w: u16,
    max_h: u16,
    base_mirror: u8,
    ref_rgb: *const u8,
    ref_lines: *const u8, // Twelve bytes (four RGB lines), or null.
    default_asset: *const std::os::raw::c_char,
    var_pal: *const i8,
    var_hf: *const i8,
    var_vf: *const i8,
    var_slots: *const u16, // Per-candidate slot bitmask, or null.
    var_sig: *const u64,   // Per-candidate contenido signature, or null.
    var_assets: *const *const std::os::raw::c_char,
    n_var: u32,
    mask: *const std::os::raw::c_char, // Costume mask; null or empty means none.
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || hashes.is_null() || default_asset.is_null() || n == 0 {
            return;
        }
        let hv: Vec<u64> = std::slice::from_raw_parts(hashes, n as usize).to_vec();
        let rel = if !rel_x.is_null() && !rel_y.is_null() {
            let rx = std::slice::from_raw_parts(rel_x, n as usize);
            let ry = std::slice::from_raw_parts(rel_y, n as usize);
            Some(rx.iter().zip(ry.iter()).map(|(&x, &y)| (x, y)).collect())
        } else {
            None
        };
        let dims = if !dim_w.is_null() && !dim_h.is_null() {
            let dw = std::slice::from_raw_parts(dim_w, n as usize);
            let dh = std::slice::from_raw_parts(dim_h, n as usize);
            Some(dw.iter().zip(dh.iter()).map(|(&w, &h)| (w, h)).collect())
        } else {
            None
        };
        let flips = if !mem_flips.is_null() {
            Some(std::slice::from_raw_parts(mem_flips, n as usize).to_vec())
        } else {
            None
        };
        let def = match std::ffi::CStr::from_ptr(default_asset).to_str() {
            Ok(s) => s.to_string(),
            Err(_) => return,
        };
        let mut cands: Vec<(crate::vram_sprite::VariantKey, String)> = Vec::new();
        if !var_assets.is_null()
            && !var_pal.is_null()
            && !var_hf.is_null()
            && !var_vf.is_null()
            && n_var > 0
        {
            let pal = std::slice::from_raw_parts(var_pal, n_var as usize);
            let hf = std::slice::from_raw_parts(var_hf, n_var as usize);
            let vf = std::slice::from_raw_parts(var_vf, n_var as usize);
            let ap = std::slice::from_raw_parts(var_assets, n_var as usize);
            for i in 0..n_var as usize {
                if ap[i].is_null() {
                    continue;
                }
                if let Ok(s) = std::ffi::CStr::from_ptr(ap[i]).to_str() {
                    cands.push((
                        crate::vram_sprite::VariantKey {
                            palette: pal[i],
                            hflip: hf[i],
                            vflip: vf[i],
                            slots: if var_slots.is_null() {
                                0
                            } else {
                                *var_slots.add(i)
                            },
                            sig: if var_sig.is_null() {
                                0
                            } else {
                                *var_sig.add(i)
                            },
                        },
                        s.to_string(),
                    ));
                }
            }
        }
        let rr = if ref_rgb.is_null() {
            [0, 0, 0]
        } else {
            [*ref_rgb, *ref_rgb.add(1), *ref_rgb.add(2)]
        };
        let rl = read_ref_lines(ref_lines);
        let m = read_mask(mask);
        (*ptr).add_override_variants(
            hv,
            rel,
            dims,
            flips,
            max_w,
            max_h,
            base_mirror,
            rr,
            rl,
            def,
            m,
            cands,
        );
    }
}
/// Clears all live pose overrides without modifying the pack catalog.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pose_sub_clear_overrides(ptr: *mut PoseSetSubstitutor) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            (*ptr).clear_overrides();
        }
    }
}
/// Supplies the frame's live CRAM words packed as R0–2/G3–5/B6–8.
///
/// At least 64 words are required. The substitutor tracks line stability and
/// latches contenido signatures from stable states. Call before resolution on each
/// frame.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pose_sub_set_cram(
    ptr: *mut PoseSetSubstitutor,
    words: *const u16,
    n: u32,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || words.is_null() || n < 64 {
            return;
        }
        (*ptr).set_cram(std::slice::from_raw_parts(words, n as usize));
    }
}
/// Computes a palette-line contenido signature.
///
/// The result is xxHash3 over the raw 9-bit colors in the selected `slots`. Both
/// authoring and runtime latching use this function to keep signatures identical.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_palette_signature(
    words: *const u16,
    n: u32,
    line: u8,
    slots: u16,
) -> u64 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if words.is_null() || n < 64 {
            return 0;
        }
        vram_sprite::palette_signature(std::slice::from_raw_parts(words, n as usize), line, slots)
    }
}
/// Sets the visible display area for the active video mode.
///
/// A missing pose member is tolerated only when its expected rectangle is fully
/// outside `[0, width) × [0, height)`. Zero dimensions disable this constraint.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pose_sub_set_screen(ptr: *mut PoseSetSubstitutor, w: u16, h: u16) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            (*ptr).set_screen(w, h);
        }
    }
}
#[unsafe(no_mangle)]
/// Resolves whole poses, updates the claim map, and writes substitutions.
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pose_sub_resolve(
    ptr: *const PoseSetSubstitutor,
    occs: *const AytherSpriteOccurrence,
    occ_count: u32,
    claimed: *mut u8, // In/out claim map; pose substitution marks its members.
    out_buf: *mut AytherSpriteSub,
    buf_cap: u32,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || out_buf.is_null() {
            return 0;
        }
        let raw_occs = if occs.is_null() || occ_count == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(occs, occ_count as usize)
        };
        let occ_vec: Vec<vram_sprite::SpriteOccurrence> = raw_occs
            .iter()
            .map(|o| vram_sprite::SpriteOccurrence {
                hash: o.hash,
                anim_group_id: o.anim_group_id,
                w_tiles: o.w_tiles,
                h_tiles: o.h_tiles,
                screen_x: o.screen_x,
                screen_y: o.screen_y,
                link: o.link,
                palette: o.palette,
                priority: o.priority,
                slot: o.slot,
                hflip: o.hflip,
                vflip: o.vflip,
            })
            .collect();
        let mut claimed_vec: Vec<bool> = if claimed.is_null() {
            vec![false; occ_vec.len()]
        } else {
            (0..occ_vec.len()).map(|i| *claimed.add(i) != 0).collect()
        };
        let subs = (*ptr).resolve(&occ_vec, &mut claimed_vec);
        if !claimed.is_null() {
            for (i, &c) in claimed_vec.iter().enumerate() {
                *claimed.add(i) = c as u8;
            }
        }
        let n = subs.len().min(buf_cap as usize);
        for (i, sub) in subs[..n].iter().enumerate() {
            let entry = &mut *out_buf.add(i);
            let bytes = sub.asset_path.as_bytes();
            let copy_len = bytes.len().min(255);
            std::ptr::write_bytes(entry.asset_path.as_mut_ptr(), 0, 256);
            std::ptr::copy_nonoverlapping(
                bytes.as_ptr() as *const std::os::raw::c_char,
                entry.asset_path.as_mut_ptr(),
                copy_len,
            );
            entry.screen_x = sub.screen_x;
            entry.screen_y = sub.screen_y;
            entry.w_tiles = sub.w_tiles;
            entry.h_tiles = sub.h_tiles;
            entry.w_px = sub.w_px;
            entry.h_px = sub.h_px;
            entry.mirror = sub.mirror;
            entry.palette = sub.palette;
            entry.synth_pal = sub.synth_pal;
            entry.ref_rgb = sub.ref_rgb;
            entry.u0 = sub.u0;
            entry.v0 = sub.v0;
            entry.uw = sub.uw;
            entry.vh = sub.vh;
            entry.pose_key = sub.pose_key;
            let mbytes = sub.mask_path.as_bytes();
            let mlen = mbytes.len().min(255);
            std::ptr::write_bytes(entry.mask_path.as_mut_ptr(), 0, 256);
            std::ptr::copy_nonoverlapping(
                mbytes.as_ptr() as *const std::os::raw::c_char,
                entry.mask_path.as_mut_ptr(),
                mlen,
            );
        }
        n as u32
    }
}

/// Copy `s` into a NUL-terminated C buffer, truncating to `cap`.
unsafe fn ffi_write_cstr(buf: *mut std::os::raw::c_char, cap: usize, s: &str) {
    // SAFETY: A non-null `buf` is guaranteed by the caller to reference `cap`
    // writable bytes that do not overlap `s`.
    unsafe {
        if buf.is_null() || cap == 0 {
            return;
        }
        let bytes = s.as_bytes();
        let n = bytes.len().min(cap - 1);
        std::ptr::copy_nonoverlapping(bytes.as_ptr() as *const std::os::raw::c_char, buf, n);
        *buf.add(n) = 0;
    }
}

// ---------------------------------------------------------------------------
// `TweenPlayer` advances in-between frames over time. Given a resolved HD
// `target`, it returns either the active intermediate frame or that target.
// ---------------------------------------------------------------------------
use vram_sprite::TweenPlayer;

#[unsafe(no_mangle)]
/// Allocates an empty in-between transition player.
pub extern "C" fn ayther_tween_new() -> *mut TweenPlayer {
    Box::into_raw(Box::new(TweenPlayer::new()))
}
#[unsafe(no_mangle)]
/// Frees an in-between transition player.
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tween_free(ptr: *mut TweenPlayer) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            drop(Box::from_raw(ptr));
        }
    }
}
#[unsafe(no_mangle)]
/// Loads transition sequences from a pack and returns the catalog size.
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tween_load_pack(
    ptr: *mut TweenPlayer,
    pack: *const AyArchive,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || pack.is_null() {
            return 0;
        }
        (*ptr).load_from_pack(&*pack);
        (*ptr).catalog_len() as u32
    }
}
#[unsafe(no_mangle)]
/// Advances active transition timers by one emulated frame.
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tween_begin_frame(ptr: *mut TweenPlayer) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            (*ptr).begin_frame();
        }
    }
}
/// Resolves the asset to render for one pose instance on this frame.
///
/// `target` is the resolved replacement, while `pose_key` and the screen-space
/// bounding-box center identify the instance. The function writes either the
/// active in-between frame or `target` as a null-terminated path. Transitions are
/// tracked per instance and per `(from, target)` pair.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tween_resolve(
    ptr: *mut TweenPlayer,
    target: *const std::os::raw::c_char,
    pose_key: u64,
    cx: i32,
    cy: i32,
    out_buf: *mut std::os::raw::c_char,
    cap: u32,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || target.is_null() || out_buf.is_null() || cap == 0 {
            return;
        }
        let t = match std::ffi::CStr::from_ptr(target).to_str() {
            Ok(s) => s,
            Err(_) => return,
        };
        let render = (*ptr).resolve(t, pose_key, cx as i64, cy as i64);
        ffi_write_cstr(out_buf, cap as usize, &render);
    }
}

/// Clears per-instance transition state after seeks or contenido changes.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tween_clear(ptr: *mut TweenPlayer) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            (*ptr).clear_state();
        }
    }
}

/// Registers a live transition override with priority over the pack catalog.
///
/// A null `from` is a wildcard matching any source pose. Frame paths and tick
/// counts must correspond to assets in the live authoring channel.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tween_set_override(
    ptr: *mut TweenPlayer,
    from: *const std::os::raw::c_char, // Null matches any source.
    target: *const std::os::raw::c_char,
    frames: *const *const std::os::raw::c_char,
    n_frames: u32,
    ticks: u32,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || target.is_null() || frames.is_null() || n_frames == 0 {
            return;
        }
        let t = match std::ffi::CStr::from_ptr(target).to_str() {
            Ok(s) => s,
            Err(_) => return,
        };
        let f = if from.is_null() {
            None
        } else {
            match std::ffi::CStr::from_ptr(from).to_str() {
                Ok(s) => Some(s),
                Err(_) => return,
            }
        };
        let mut list = Vec::with_capacity(n_frames as usize);
        for i in 0..n_frames as usize {
            let p = *frames.add(i);
            if p.is_null() {
                return;
            }
            match std::ffi::CStr::from_ptr(p).to_str() {
                Ok(s) => list.push(s.to_string()),
                Err(_) => return,
            }
        }
        (*ptr).set_override(f, t, list, ticks);
    }
}

/// Clears live transition overrides without modifying the pack catalog.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tween_clear_overrides(ptr: *mut TweenPlayer) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            (*ptr).clear_overrides();
        }
    }
}

// ===========================================================================
// `PackBuilder` assembly and signing C API.
// ===========================================================================
//
// Lets the Ayther Lab build a signed pack in-process (no shelling out to the
// ay_pack CLI). Ownership: ayther_pack_builder_new() → caller owns; free drops.
//
//   b = ayther_pack_builder_new();
//   ayther_pack_builder_add_bytes(b, "manifest.toml",..);
//   ayther_pack_builder_add_bytes(b, "sprite_substitutions.toml",..);
//   ayther_pack_builder_add_file (b, "hero.png", "/lib/hero.png");
//   ok = ayther_pack_builder_finish(b, true, "out.ay", err, sizeof err);
//   ayther_pack_builder_free(b);

use crate::pack_builder::PackBuilder;

#[unsafe(no_mangle)]
/// Allocates an empty in-process pack builder.
pub extern "C" fn ayther_pack_builder_new() -> *mut PackBuilder {
    Box::into_raw(Box::new(PackBuilder::new()))
}

#[unsafe(no_mangle)]
/// Frees an in-process pack builder and its staged entries.
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_builder_free(ptr: *mut PackBuilder) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            drop(Box::from_raw(ptr));
        }
    }
}

/// Stage an entry from raw bytes. Returns false on a null/invalid path
/// (`signature.bin` is reserved and rejected).
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_builder_add_bytes(
    ptr: *mut PackBuilder,
    path: *const std::os::raw::c_char,
    data: *const u8,
    len: usize,
) -> bool {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || path.is_null() {
            return false;
        }
        let path = match std::ffi::CStr::from_ptr(path).to_str() {
            Ok(s) => s,
            Err(_) => return false,
        };
        let bytes = if data.is_null() || len == 0 {
            Vec::new()
        } else {
            std::slice::from_raw_parts(data, len).to_vec()
        };
        (*ptr).add_bytes(path, bytes)
    }
}

/// Stage an entry by reading `source_fs_path` into the pack at `path_in_pack`.
/// Returns false if the path is invalid or the source file can't be read.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_builder_add_file(
    ptr: *mut PackBuilder,
    path_in_pack: *const std::os::raw::c_char,
    source_fs_path: *const std::os::raw::c_char,
) -> bool {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || path_in_pack.is_null() || source_fs_path.is_null() {
            return false;
        }
        let pin = match std::ffi::CStr::from_ptr(path_in_pack).to_str() {
            Ok(s) => s,
            Err(_) => return false,
        };
        let src = match std::ffi::CStr::from_ptr(source_fs_path).to_str() {
            Ok(s) => s,
            Err(_) => return false,
        };
        (*ptr).add_file(pin, std::path::Path::new(src)).is_ok()
    }
}

/// Number of entries staged (excludes the signature added by finish).
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_builder_count(ptr: *const PackBuilder) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            0
        } else {
            (*ptr).file_count() as u32
        }
    }
}

/// Write the pack to `out_path`, dev-signing when `sign` is true. On failure
/// copies a mensaje into `err_buf` (NUL-terminated, capped at `err_cap`) and
/// returns false.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_pack_builder_finish(
    ptr: *mut PackBuilder,
    sign: bool,
    out_path: *const std::os::raw::c_char,
    err_buf: *mut std::os::raw::c_char,
    err_cap: usize,
) -> bool {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        let write_err = |msg: &str| {
            if !err_buf.is_null() && err_cap > 0 {
                let bytes = msg.as_bytes();
                let n = bytes.len().min(err_cap - 1);
                std::ptr::copy_nonoverlapping(
                    bytes.as_ptr() as *const std::os::raw::c_char,
                    err_buf,
                    n,
                );
                *err_buf.add(n) = 0;
            }
        };
        if ptr.is_null() || out_path.is_null() {
            write_err("null argument");
            return false;
        }
        let out = match std::ffi::CStr::from_ptr(out_path).to_str() {
            Ok(s) => s,
            Err(_) => {
                write_err("output path not UTF-8");
                return false;
            }
        };
        match (*ptr).finish(sign, std::path::Path::new(out)) {
            Ok(()) => true,
            Err(e) => {
                write_err(&e);
                false
            }
        }
    }
}

/// Test-support boundary used by the native fixtures to exercise production
/// trust policy without accepting the embedded RFC 8032 development key.
///
/// This symbol is intentionally absent from the public C header. It derives a
/// deterministic, non-development Ed25519 key, writes its public trust registry,
/// and signs the staged pack with the matching private key.
#[unsafe(no_mangle)]
/// # Safety
///
/// All pointers must be valid for the duration of the call. Paths must point to
/// readable NUL-terminated UTF-8 strings, and `err_buf` must be writable for
/// `err_cap` bytes when it is non-null.
pub unsafe extern "C" fn ayther_test_pack_builder_finish_trusted(
    ptr: *mut PackBuilder,
    out_path: *const std::os::raw::c_char,
    registry_path: *const std::os::raw::c_char,
    err_buf: *mut std::os::raw::c_char,
    err_cap: usize,
) -> bool {
    unsafe {
        let write_err = |message: &str| {
            if !err_buf.is_null() && err_cap > 0 {
                let bytes = message.as_bytes();
                let length = bytes.len().min(err_cap - 1);
                std::ptr::copy_nonoverlapping(
                    bytes.as_ptr().cast::<std::os::raw::c_char>(),
                    err_buf,
                    length,
                );
                *err_buf.add(length) = 0;
            }
        };
        if ptr.is_null() || out_path.is_null() || registry_path.is_null() {
            write_err("null argument");
            return false;
        }
        let Ok(out) = std::ffi::CStr::from_ptr(out_path).to_str() else {
            write_err("output path not UTF-8");
            return false;
        };
        let Ok(registry) = std::ffi::CStr::from_ptr(registry_path).to_str() else {
            write_err("registry path not UTF-8");
            return false;
        };

        const TEST_KEY_ID: &str = "ayther-native-test-2026";
        const TEST_SIGNING_SEED: [u8; 32] = [7; 32];
        let signing_key = ed25519_dalek::SigningKey::from_bytes(&TEST_SIGNING_SEED);
        let public_key = signing_key
            .verifying_key()
            .to_bytes()
            .iter()
            .map(|byte| format!("{byte:02x}"))
            .collect::<String>();
        let trust_registry = format!(
            "version = 1\n\n[[keys]]\n\
             id = \"{TEST_KEY_ID}\"\n\
             algorithm = \"ed25519\"\n\
             public_key = \"{public_key}\"\n\
             not_before_unix = 0\n\
             not_after_unix = 4102444800\n\
             revoked = false\n\
             games = [\"*\"]\n"
        );
        if let Err(error) = std::fs::write(registry, trust_registry) {
            write_err(&format!("writing trust registry: {error}"));
            return false;
        }
        match (*ptr).finish_with_signing_key(TEST_KEY_ID, &signing_key, std::path::Path::new(out)) {
            Ok(()) => true,
            Err(error) => {
                write_err(&error);
                false
            }
        }
    }
}

// ===========================================================================
// `AudioHasher` opaque-handle C API.
// ===========================================================================
//
// Fingerprints libretro PCM batches (retro_audio_sample_batch) using
// xxHash3-64 over the raw i16 stereo buffer.  Silent batches are skipped.
//
// Ownership:
//   ayther_audio_hasher_new()  → caller owns
//   ayther_audio_hasher_free() → drops the Box
//
// Typical per-frame call sequence:
//   // inside retro_audio_sample_batch callback:
//   ayther_audio_hasher_process_batch(h, datos, frames);
//
//   // once at the end of run_frame():
//   ayther_audio_hasher_end_tick(h);
//
//   // read occurrences for the Lab / substitutor:
//   n = ayther_audio_hasher_get_occurrences(h, buf, cap);

/// C-compatible audio occurrence for FFI.
#[repr(C)]
pub struct AytherAudioOccurrence {
    /// Exact hash of the observed PCM batch.
    pub hash: u64,
    /// Stereo frames in the original batch (samples / 2).
    pub frame_count: u32,
    /// Times this hash appeared in the current game tick.
    pub hits: u32,
}

/// C-compatible resolved audio substitution for FFI.
#[repr(C)]
pub struct AytherAudioSub {
    /// Hash of the emulator PCM batch to suppress.
    pub hash: u64,
    /// Null-terminated logical replacement asset path.
    pub asset_path: [std::os::raw::c_char; 256],
    /// Duration hint: stereo frames in the original batch.
    pub frame_count: u32,
}

/// Allocate a new AudioHasher.  Free with `ayther_audio_hasher_free`.
#[unsafe(no_mangle)]
pub extern "C" fn ayther_audio_hasher_new() -> *mut AudioHasher {
    Box::into_raw(Box::new(AudioHasher::new()))
}

/// Destroy an AudioHasher.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_hasher_free(ptr: *mut AudioHasher) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            drop(Box::from_raw(ptr));
        }
    }
}

/// Fingerprint one stereo PCM batch from `retro_audio_sample_batch`.
///
/// `datos`   — pointer to the stereo-interleaved i16 buffer (L₀,R₀,L₁,R₁,…).
/// `frames` — number of stereo frames (as passed by libretro; len = frames × 2).
///
/// Returns the hash, or `0` if the batch was silent and was skipped.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_hasher_process_batch(
    ptr: *mut AudioHasher,
    data: *const i16,
    frames: usize,
) -> u64 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || data.is_null() || frames == 0 {
            return 0;
        }
        let samples = std::slice::from_raw_parts(data, frames * 2);
        (*ptr).process_batch(samples)
    }
}

/// Snapshot this tick's occurrences and reset in-flight counters.
/// Call once per `run_frame()` cycle.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_hasher_end_tick(ptr: *mut AudioHasher) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            (*ptr).end_tick();
        }
    }
}

/// Total unique PCM patterns accumulated since creation.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_hasher_unique_count(ptr: *const AudioHasher) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return 0;
        }
        (*ptr).unique_count()
    }
}

/// Fill `out_buf` with occurrences from the most-recently-completed tick.
/// Returns the number of entries written (up to `buf_cap`).
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_hasher_get_occurrences(
    ptr: *const AudioHasher,
    out_buf: *mut AytherAudioOccurrence,
    buf_cap: u32,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || out_buf.is_null() {
            return 0;
        }
        let occs = (*ptr).last_occurrences();
        let n = occs.len().min(buf_cap as usize);
        for (i, occ) in occs[..n].iter().enumerate() {
            *out_buf.add(i) = AytherAudioOccurrence {
                hash: occ.hash,
                frame_count: occ.frame_count as u32,
                hits: occ.hits,
            };
        }
        n as u32
    }
}

// ===========================================================================
// `AudioEventDetector` opaque-handle C API for per-channel events.
//
// Events are detected from the command sequence sent to sound chips, rather
// than from PCM, so results remain stable across replays. Each frame consumes
// `AytherAudioWrite` records and emits signed per-channel activity blocks.
//
//   d = ayther_audio_event_new();
//   for each frame:  ayther_audio_event_process_frame(d, frame, writes, n);
//   ayther_audio_event_finish(d);          // cierra bloques abiertos
//   k = ayther_audio_event_count(d);
//   ayther_audio_event_get(d, buf, cap);
//   ayther_audio_event_free(d);
// ===========================================================================

/// C-compatible detected audio event matching [`audio_event::AudioEvent`].
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AytherAudioEvent {
    /// Stable hash of the channel state at key-on.
    pub signature: u64,
    /// Instrument identity: a frequency- and channel-independent parche hash, or
    /// the full signature for DAC events. It groups the same sound across notes
    /// and channels for DAW export.
    pub instrument: u64,
    /// First emulated frame covered by the event.
    pub start_frame: u32,
    /// Last emulated frame covered by the event, inclusive.
    pub end_frame: u32,
    /// Audio-chip identifier.
    pub chip: u8, // 0 = YM2612 (FM), 1 = SN76489 (PSG)
    /// Chip-local channel number.
    pub channel: u8, // FM 0-5 | PSG 0-3
    /// MIDI note at key-on, or 255 for pitchless DAC, noise, and PCM events.
    pub pitch: u8,
    /// MIDI-style velocity at key-on in 1–127, or zero when unknown.
    ///
    /// This field reuses a former padding byte, preserving the 32-byte ABI layout.
    /// See [`audio_event::AudioEvent::velocity`].
    pub velocity: u8,
}

/// Allocate a new AudioEventDetector.  Free with `ayther_audio_event_free`.
#[unsafe(no_mangle)]
pub extern "C" fn ayther_audio_event_new() -> *mut audio_event::AudioEventDetector {
    Box::into_raw(Box::new(audio_event::AudioEventDetector::new()))
}

/// Sets the clock region used for pitch decoding: 0 for NTSC, 1 for PAL.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_event_set_pal(
    ptr: *mut audio_event::AudioEventDetector,
    pal: u8,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            (*ptr).set_pal(pal != 0);
        }
    }
}

/// Supplies audio evidence for channels already sounding at capture start.
///
/// Bits 0–5 represent FM channels and bits 6–9 represent PSG channels. PCM
/// evidence is provided by the engine probe.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_event_set_initial_active(
    ptr: *mut audio_event::AudioEventDetector,
    mask: u16,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            (*ptr).set_initial_active(mask);
        }
    }
}

/// Destroy an AudioEventDetector.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_event_free(ptr: *mut audio_event::AudioEventDetector) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            drop(Box::from_raw(ptr));
        }
    }
}

/// Clear all state (new recording / re-analysis).
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_event_reset(ptr: *mut audio_event::AudioEventDetector) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            (*ptr).reset();
        }
    }
}

/// Ingest one frame's raw chip-write log. `writes` points to `n`
/// `audio_event::AudioWrite` (== AytherAudioWrite) records in bus order.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_event_process_frame(
    ptr: *mut audio_event::AudioEventDetector,
    frame: u32,
    writes: *const audio_event::AudioWrite,
    n: u32,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return;
        }
        let slice = if writes.is_null() || n == 0 {
            &[][..]
        } else {
            std::slice::from_raw_parts(writes, n as usize)
        };
        (*ptr).process_frame(frame, slice);
    }
}

/// Ingest one frame by BOTH audio paths. `writes` are the raw FM/PSG bus
/// writes; `pcm` are the already-typed Sega CD RF5C164 events (that chip has no
/// exposed bus, so there are no writes to interpret). One call per frame, so the
/// two paths share a frame number.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_event_process_frame_ex(
    ptr: *mut audio_event::AudioEventDetector,
    frame: u32,
    writes: *const audio_event::AudioWrite,
    n: u32,
    pcm: *const audio_event::PcmEvent,
    m: u32,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return;
        }
        let w = if writes.is_null() || n == 0 {
            &[][..]
        } else {
            std::slice::from_raw_parts(writes, n as usize)
        };
        let p = if pcm.is_null() || m == 0 {
            &[][..]
        } else {
            std::slice::from_raw_parts(pcm, m as usize)
        };
        (*ptr).process_frame_ex(frame, w, p);
    }
}

/// Close any blocks still open at the end of the take.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_event_finish(ptr: *mut audio_event::AudioEventDetector) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            (*ptr).finish();
        }
    }
}

/// Number of detected (closed) events so far.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_event_count(
    ptr: *const audio_event::AudioEventDetector,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return 0;
        }
        (*ptr).event_count() as u32
    }
}

/// C representation of a currently active audio channel.
///
/// Instrument and pitch are captured at key-on so runtime instrument rules can
/// resolve without waiting for the event to close. The structure is 24 bytes and
/// mirrors [`audio_event::ActiveChannel`].
#[repr(C)]
pub struct AytherAudioActive {
    /// Stable event signature captured at key-on.
    pub signature: u64,
    /// Timbre identity captured at key-on, or zero when unavailable.
    pub instrument: u64, // fm_instrument/psg_instrument (0 = desconocido)
    /// Audio-chip identifier.
    pub chip: u8,
    /// Chip-local channel number.
    pub channel: u8,
    /// MIDI note captured at key-on, or 255 when unavailable.
    pub pitch: u8, // MIDI note at key-on; 255 means unpitched.
    /// Reserved bytes; initialize to zero.
    pub _pad: [u8; 5],
}

/// Copies the channels that are currently active, including their signatures.
///
/// Writes at most `buf_cap` entries to `out_buf` and returns the count written.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_event_active(
    ptr: *const audio_event::AudioEventDetector,
    out_buf: *mut AytherAudioActive,
    buf_cap: u32,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || out_buf.is_null() {
            return 0;
        }
        let act = (*ptr).active_channels();
        let n = act.len().min(buf_cap as usize);
        for (i, a) in act[..n].iter().enumerate() {
            *out_buf.add(i) = AytherAudioActive {
                signature: a.signature,
                instrument: a.instrument,
                chip: a.chip,
                channel: a.channel,
                pitch: a.pitch,
                _pad: [0; 5],
            };
        }
        n as u32
    }
}

/// Clears closed events without changing live channel state.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_event_clear_events(
    ptr: *mut audio_event::AudioEventDetector,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            (*ptr).clear_events();
        }
    }
}

/// Fill `out_buf` with up to `buf_cap` detected events. Returns the count written.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_event_get(
    ptr: *const audio_event::AudioEventDetector,
    out_buf: *mut AytherAudioEvent,
    buf_cap: u32,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || out_buf.is_null() {
            return 0;
        }
        let evs = (*ptr).events();
        let n = evs.len().min(buf_cap as usize);
        for (i, e) in evs[..n].iter().enumerate() {
            *out_buf.add(i) = AytherAudioEvent {
                signature: e.signature,
                instrument: e.instrument,
                start_frame: e.start_frame,
                end_frame: e.end_frame,
                chip: e.chip,
                channel: e.channel,
                pitch: e.pitch,
                velocity: e.velocity,
            };
        }
        n as u32
    }
}

// ===========================================================================
// `audio_events.toml` event-substitution catalog.
//
// The TOML persists signature-to-asset and channel mappings. The core owns the
// format and parser; the session enumerates assignments and handles storage.
// ===========================================================================

/// C representation of an event-based audio substitution.
///
/// Sequence fields reuse former padding; rule fields are appended. The structure
/// is 288 bytes and mirrors [`audio_event::EventSub`].
#[repr(C)]
pub struct AytherEventSub {
    /// Exact trigger signature used as the persistent key.
    pub signature: u64,
    /// Null-terminated logical replacement asset path.
    pub asset: [std::os::raw::c_char; 256],
    /// Bit mask of audio channels covered by the substitution.
    pub channels: u32,
    /// Whether the replacement loops until the sequence window closes.
    pub looping: u8,
    /// Reserved byte; initialize to zero.
    pub _pad: u8,
    /// Sequence window in frames, or zero for a classic per-event substitution.
    pub duration_frames: u32,
    /// Timbre identity used by the rule, or zero when no rule applies.
    pub match_instrument: u64,
    /// Match rule: 0 exact signature, 1 instrument, or 2 instrument and note.
    pub match_rule: u8,
    /// MIDI note for rule 2, or 255 when pitch is unavailable.
    pub match_pitch: u8,
    /// Audio bus: 0 unclassified, 1 music, 2 effects, or 3 voices.
    ///
    /// This reuses a former padding byte, so older binaries read zero, which is
    /// also the correct value when a pack did not classify the sound.
    pub bus: u8,
    /// Reserved bytes; initialize to zero.
    pub _pad2: [u8; 5],
}

unsafe fn read_asset_field(buf: &[std::os::raw::c_char; 256]) -> String {
    // SAFETY: `c_char` and `u8` have identical size and alignment, and every bit
    // pattern is valid for both types. The shared borrow is preserved.
    unsafe {
        let bytes = &*(buf as *const _ as *const [u8; 256]);
        let len = bytes.iter().position(|&b| b == 0).unwrap_or(256);
        String::from_utf8_lossy(&bytes[..len]).into_owned()
    }
}

unsafe fn write_asset_field(buf: &mut [std::os::raw::c_char; 256], s: &str) {
    // SAFETY: `c_char` and `u8` have identical size and alignment, and every bit
    // pattern is valid for both types. The exclusive borrow is preserved.
    unsafe {
        let dst = &mut *(buf as *mut _ as *mut [u8; 256]);
        let b = s.as_bytes();
        let n = b.len().min(255);
        dst[..n].copy_from_slice(&b[..n]);
        dst[n] = 0;
    }
}

// ---------------------------------------------------------------------------
// Audio-condition gate evaluated in the core.
//
// The native client creates this gate when loading `audio_events.toml` and
// evaluates it against live RAM each frame. Reusing `crate::conditions` keeps
// audio and tile rules on the same condition dialect.
// ---------------------------------------------------------------------------

/// Compiles an audio-condition gate from `audio_events.toml` text.
///
/// Returns null when there are no conditions, allowing callers to skip per-frame
/// evaluation.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_gate_new(
    text: *const std::os::raw::c_char,
) -> *mut audio_event::AudioEventGate {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if text.is_null() {
            return std::ptr::null_mut();
        }
        let s = std::ffi::CStr::from_ptr(text).to_string_lossy();
        let g = audio_event::AudioEventGate::from_toml(&s);
        if g.is_empty() {
            return std::ptr::null_mut();
        }
        Box::into_raw(Box::new(g))
    }
}

/// Frees an audio-condition gate. Null is accepted as a no-op.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_gate_free(g: *mut audio_event::AudioEventGate) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !g.is_null() {
            drop(Box::from_raw(g));
        }
    }
}

/// Returns signatures whose conditions are false on this frame.
///
/// These events must use original audio. At most `cap` signatures are written,
/// while the return value reports the total available. Set `word_swapped` for
/// the 68000 work-RAM representation with interleaved host-endian words.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_gate_eval(
    g: *const audio_event::AudioEventGate,
    ram: *const u8,
    ram_len: usize,
    word_swapped: bool,
    frame: u32,
    out: *mut u64,
    cap: u32,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if g.is_null() || ram.is_null() || ram_len == 0 {
            return 0;
        }
        let bytes = std::slice::from_raw_parts(ram, ram_len);
        let view = if word_swapped {
            conditions::RamView::word_swapped(bytes)
        } else {
            conditions::RamView::linear(bytes)
        };
        let ctx = conditions::FrameCtx::new(frame as u64, view);
        let blocked = (*g).blocked(&ctx);
        let k = blocked.len().min(cap as usize);
        if !out.is_null() {
            for (i, sig) in blocked[..k].iter().enumerate() {
                *out.add(i) = *sig;
            }
        }
        blocked.len() as u32
    }
}

// ---------------------------------------------------------------------------
// Widescreen gate using the same condition path as the audio gate.
//
// The native client compiles this gate while loading the pack and evaluates the
// width against live RAM each frame through `crate::conditions`.
// ---------------------------------------------------------------------------

/// Compiles a widescreen gate from `widescreen.toml` text.
///
/// Returns null when the pack declares no `[[widescreen]]` rules. This avoids
/// per-frame evaluation and leaves manual authoring overrides unchanged.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_widescreen_gate_new(
    text: *const std::os::raw::c_char,
) -> *mut widescreen_gate::WidescreenGate {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if text.is_null() {
            return std::ptr::null_mut();
        }
        let s = std::ffi::CStr::from_ptr(text).to_string_lossy();
        let g = widescreen_gate::WidescreenGate::from_toml(&s);
        if g.is_empty() {
            return std::ptr::null_mut();
        }
        Box::into_raw(Box::new(g))
    }
}

/// Frees a widescreen gate. Null is accepted as a no-op.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_widescreen_gate_free(g: *mut widescreen_gate::WidescreenGate) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !g.is_null() {
            drop(Box::from_raw(g));
        }
    }
}

/// Evaluates the logical width for the current frame.
///
/// Writes `out_width` and returns true only when a rule matches; otherwise the
/// caller must preserve its existing width. Set `word_swapped` for the 68000
/// work-RAM representation with interleaved host-endian words.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_widescreen_gate_eval(
    g: *const widescreen_gate::WidescreenGate,
    ram: *const u8,
    ram_len: usize,
    word_swapped: bool,
    frame: u32,
    out_width: *mut u32,
) -> bool {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if g.is_null() || out_width.is_null() {
            return false;
        }
        // Without RAM, only conditions such as `FrameRange` can match. Memory-based
        // conditions evaluate to false, preserving the safe default width.
        let view = if ram.is_null() || ram_len == 0 {
            conditions::RamView::empty()
        } else {
            let bytes = std::slice::from_raw_parts(ram, ram_len);
            if word_swapped {
                conditions::RamView::word_swapped(bytes)
            } else {
                conditions::RamView::linear(bytes)
            }
        };
        let ctx = conditions::FrameCtx::new(frame as u64, view);
        match (*g).width_for(&ctx) {
            Some(w) => {
                *out_width = w;
                true
            }
            None => false,
        }
    }
}

#[unsafe(no_mangle)]
/// Serializes event substitutions as `audio_events.toml`.
///
/// Returns the required byte count excluding the trailing null. The output is
/// written only when `out_cap` is large enough for the text and terminator.
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_events_format(
    subs: *const AytherEventSub,
    n: u32,
    out: *mut std::os::raw::c_char,
    out_cap: u32,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        let slice = if subs.is_null() || n == 0 {
            &[][..]
        } else {
            std::slice::from_raw_parts(subs, n as usize)
        };
        let v: Vec<audio_event::EventSub> = slice
            .iter()
            .map(|s| audio_event::EventSub {
                signature: s.signature,
                asset: read_asset_field(&s.asset),
                channels: s.channels,
                duration_frames: s.duration_frames,
                looping: s.looping != 0,
                match_rule: s.match_rule,
                match_instrument: s.match_instrument,
                match_pitch: s.match_pitch,
                bus: s.bus,
            })
            .collect();
        let text = audio_event::events_to_toml(&v);
        let bytes = text.as_bytes();
        if !out.is_null() && (out_cap as usize) > bytes.len() {
            let dst = std::slice::from_raw_parts_mut(out as *mut u8, out_cap as usize);
            dst[..bytes.len()].copy_from_slice(bytes);
            dst[bytes.len()] = 0;
        }
        bytes.len() as u32
    }
}

/// Parses `audio_events.toml` into at most `cap` entries in `out` and returns the
/// number written.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_events_parse(
    text: *const std::os::raw::c_char,
    out: *mut AytherEventSub,
    cap: u32,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if text.is_null() {
            return 0;
        }
        let s = std::ffi::CStr::from_ptr(text).to_string_lossy();
        let subs = audio_event::events_from_toml(&s);
        // Write at most `cap` entries but return the total available, allowing a
        // first call with `cap == 0` to size the destination buffer.
        let k = subs.len().min(cap as usize);
        if !out.is_null() {
            for (i, e) in subs[..k].iter().enumerate() {
                let dst = &mut *out.add(i);
                dst.signature = e.signature;
                dst.channels = e.channels;
                dst.looping = e.looping as u8;
                dst._pad = 0;
                dst.duration_frames = e.duration_frames;
                dst.match_instrument = e.match_instrument;
                dst.match_rule = e.match_rule;
                dst.match_pitch = e.match_pitch;
                dst.bus = e.bus;
                dst._pad2 = [0; 5];
                write_asset_field(&mut dst.asset, &e.asset);
            }
        }
        subs.len() as u32
    }
}

// ===========================================================================
// `AudioSubstitutor` opaque-handle C API.
// ===========================================================================

/// Allocate a new AudioSubstitutor.  Free with `ayther_audio_sub_free`.
#[unsafe(no_mangle)]
pub extern "C" fn ayther_audio_sub_new() -> *mut AudioSubstitutor {
    Box::into_raw(Box::new(AudioSubstitutor::new()))
}

/// Destroy an AudioSubstitutor.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_sub_free(ptr: *mut AudioSubstitutor) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            drop(Box::from_raw(ptr));
        }
    }
}

/// Load substitution catalog from `audio_substitutions.toml` inside the pack.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_sub_load_pack(
    ptr: *mut AudioSubstitutor,
    pack: *const AyArchive,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || pack.is_null() {
            return;
        }
        (*ptr).load_from_pack(&*pack);
    }
}

/// Register a runtime override (e.g. from Lua `ayther.audio.replace()`).
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_sub_add_override(
    ptr: *mut AudioSubstitutor,
    hash: u64,
    asset_path: *const std::os::raw::c_char,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || asset_path.is_null() {
            return;
        }
        if let Ok(s) = std::ffi::CStr::from_ptr(asset_path).to_str() {
            (*ptr).add_override(hash, s);
        }
    }
}

/// Clear all runtime overrides.  Call at the start of each tick.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_sub_clear_overrides(ptr: *mut AudioSubstitutor) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            (*ptr).clear_overrides();
        }
    }
}

/// Number of entries loaded from the TOML catalog.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_sub_catalog_len(ptr: *const AudioSubstitutor) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            return 0;
        }
        (*ptr).catalog_len() as u32
    }
}

/// Resolve audio occurrences to HD substitution instructions.
///
/// Fills `out_buf` with up to `buf_cap` entries.  Returns count written.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_sub_resolve(
    ptr: *const AudioSubstitutor,
    occs: *const AytherAudioOccurrence,
    occ_count: u32,
    out_buf: *mut AytherAudioSub,
    buf_cap: u32,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || out_buf.is_null() {
            return 0;
        }
        let raw_occs = if occs.is_null() || occ_count == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(occs, occ_count as usize)
        };

        let occ_vec: Vec<AudioOccurrence> = raw_occs
            .iter()
            .map(|o| AudioOccurrence {
                hash: o.hash,
                frame_count: o.frame_count as usize,
                hits: o.hits,
            })
            .collect();

        let subs = (*ptr).resolve(&occ_vec);
        let n = subs.len().min(buf_cap as usize);

        for (i, sub) in subs[..n].iter().enumerate() {
            let entry = &mut *out_buf.add(i);
            entry.hash = sub.hash;
            entry.frame_count = sub.frame_count as u32;
            let bytes = sub.asset_path.as_bytes();
            let copy_len = bytes.len().min(255);
            std::ptr::write_bytes(entry.asset_path.as_mut_ptr(), 0, 256);
            std::ptr::copy_nonoverlapping(
                bytes.as_ptr() as *const std::os::raw::c_char,
                entry.asset_path.as_mut_ptr(),
                copy_len,
            );
        }
        n as u32
    }
}

// Audio-event substitution C API.

/// C-compatible resolved audio-event substitution: mute the emulator over
/// `start_frame..=end_frame` and play `asset_path` aligned to `start_frame`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AytherAudioEventSub {
    /// Stable trigger signature.
    pub signature: u64,
    /// First emulated frame covered by the replacement.
    pub start_frame: u64,
    /// Last emulated frame covered by the replacement, inclusive.
    pub end_frame: u64,
    /// Null-terminated logical replacement asset path.
    pub asset_path: [std::os::raw::c_char; 256],
    /// Whether the replacement loops until the event ends.
    pub looping: u8,
}

/// Bind an HD asset to an event signature (authoring assign; persists).
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_sub_add_event_override(
    ptr: *mut AudioSubstitutor,
    signature: u64,
    asset_path: *const std::os::raw::c_char,
    looping: u8,
) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || asset_path.is_null() {
            return;
        }
        if let Ok(s) = std::ffi::CStr::from_ptr(asset_path).to_str() {
            (*ptr).add_event_override(signature, s, looping != 0);
        }
    }
}

/// Clear all event overrides.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_sub_clear_event_overrides(ptr: *mut AudioSubstitutor) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !ptr.is_null() {
            (*ptr).clear_event_overrides();
        }
    }
}

/// Number of event substitutions in the TOML catalog.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_sub_event_catalog_len(ptr: *const AudioSubstitutor) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() {
            0
        } else {
            (*ptr).event_catalog_len() as u32
        }
    }
}

/// Resolve detected audio events (from `ayther_audio_evdet_get_events`) into
/// substitution instructions. Fills `out_buf` up to `buf_cap`; returns count.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_audio_sub_resolve_events(
    ptr: *const AudioSubstitutor,
    events: *const AytherAudioEvent,
    event_count: u32,
    out_buf: *mut AytherAudioEventSub,
    buf_cap: u32,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if ptr.is_null() || out_buf.is_null() {
            return 0;
        }
        let raw = if events.is_null() || event_count == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(events, event_count as usize)
        };

        let evs: Vec<audio_event::AudioEvent> = raw
            .iter()
            .map(|e| audio_event::AudioEvent {
                signature: e.signature,
                instrument: e.instrument,
                chip: e.chip,
                channel: e.channel,
                start_frame: e.start_frame,
                end_frame: e.end_frame,
                pitch: e.pitch,
                velocity: e.velocity,
            })
            .collect();

        let subs = (&*ptr).resolve_events(&evs);
        let n = subs.len().min(buf_cap as usize);
        for (i, sub) in subs[..n].iter().enumerate() {
            let entry = &mut *out_buf.add(i);
            entry.signature = sub.signature;
            entry.start_frame = sub.start_frame;
            entry.end_frame = sub.end_frame;
            entry.looping = sub.looping as u8;
            let bytes = sub.asset_path.as_bytes();
            let copy_len = bytes.len().min(255);
            std::ptr::write_bytes(entry.asset_path.as_mut_ptr(), 0, 256);
            std::ptr::copy_nonoverlapping(
                bytes.as_ptr() as *const std::os::raw::c_char,
                entry.asset_path.as_mut_ptr(),
                copy_len,
            );
        }
        subs.len() as u32
    }
}

// ---------------------------------------------------------------------------
// Audio-event FFI round-trip. Rust verifies behavior; the native ABI test checks
// only the layout.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Content-derived asset identifiers.
// ---------------------------------------------------------------------------
#[cfg(test)]
mod engine_version_ffi_tests {
    use super::*;

    /// The FFI must append a null terminator, so the value is written twice. This
    /// test keeps both copies synchronized so reports identify the actual build.
    #[test]
    fn ffi_returns_exact_engine_version() {
        // SAFETY: The version function returns a static null-terminated string.
        let s = unsafe { std::ffi::CStr::from_ptr(ayther_engine_version()) };
        assert_eq!(s.to_str().unwrap(), pack_validate::ENGINE_VERSION);
    }
}

#[cfg(test)]
mod asset_id_tests {
    use super::*;

    fn tmp(name: &str) -> std::path::PathBuf {
        let p = std::env::temp_dir().join(format!("ayther_asset_id_{}", name));
        let _ = std::fs::create_dir_all(&p);
        p
    }

    #[test]
    fn asset_name_comes_from_content() {
        let d = tmp("contenido");
        // Equal contenido must produce one identifier regardless of filename or
        // extension, allowing identities to deduplicate the same artwork.
        let a = d.join("heroe.png");
        let b = d.join("otro_nombre.bin");
        std::fs::write(&a, b"los mismos bytes").unwrap();
        std::fs::write(&b, b"los mismos bytes").unwrap();
        let id_a = asset_id_of_file(&a).unwrap();
        let id_b = asset_id_of_file(&b).unwrap();
        assert_eq!(id_a, id_b, "el nombre y la extension no participan");
        assert_eq!(id_a.len(), 32, "32 hex, sin extension");
        assert!(id_a.chars().all(|c| c.is_ascii_hexdigit()));

        // Equal basenames with different contenido must not collide.
        let c = tmp("colision_1").join("pose.png");
        let e = tmp("colision_2").join("pose.png");
        std::fs::write(&c, b"arte de Avanza").unwrap();
        std::fs::write(&e, b"arte de Cubre").unwrap();
        assert_ne!(asset_id_of_file(&c).unwrap(), asset_id_of_file(&e).unwrap());

        assert!(asset_id_of_file(&d.join("no_existe")).is_none());
    }

    #[test]
    fn byte_and_file_ids_match() {
        // In-memory and file-backed assets must use the same hash so a trimmed
        // SoundFont cannot enter a pack twice under different identifiers.
        let d = tmp("bytes");
        let f = d.join("x.bin");
        let data = b"contenido cualquiera".to_vec();
        std::fs::write(&f, &data).unwrap();
        assert_eq!(asset_id_of_bytes(&data), asset_id_of_file(&f).unwrap());

        // Empty contenido is valid and still has a stable identifier.
        assert_eq!(asset_id_of_bytes(&[]).len(), 32);
        // SAFETY: Every pointer comes from a live local buffer whose reported
        // length matches its allocation.
        unsafe {
            let mut out = [0i8; 33];
            assert!(ayther_asset_id_bytes(
                data.as_ptr(),
                data.len(),
                out.as_mut_ptr(),
                out.len()
            ));
            let got = std::ffi::CStr::from_ptr(out.as_ptr()).to_str().unwrap();
            assert_eq!(got, asset_id_of_bytes(&data));
            let mut short_buffer = [0i8; 32];
            assert!(!ayther_asset_id_bytes(
                data.as_ptr(),
                data.len(),
                short_buffer.as_mut_ptr(),
                short_buffer.len()
            ));
        }
    }

    #[test]
    fn ffi_returns_same_id_and_rejects_short_buffer() {
        let d = tmp("ffi");
        let f = d.join("a.png");
        std::fs::write(&f, b"contenido").unwrap();
        let want = asset_id_of_file(&f).unwrap();
        let c = std::ffi::CString::new(f.to_str().unwrap()).unwrap();
        // SAFETY: `c` is a valid C string and both output arrays remain alive
        // with their exact capacities for every call.
        unsafe {
            let mut out = [0i8; 33];
            assert!(ayther_asset_id(c.as_ptr(), out.as_mut_ptr(), out.len()));
            let got = std::ffi::CStr::from_ptr(out.as_ptr()).to_str().unwrap();
            assert_eq!(got, want);
            // A 32-digit identifier plus its null terminator needs 33 bytes.
            // Reject smaller buffers instead of returning a collision-prone prefix.
            let mut short_buffer = [0i8; 32];
            assert!(!ayther_asset_id(
                c.as_ptr(),
                short_buffer.as_mut_ptr(),
                short_buffer.len()
            ));
        }
    }
}

#[cfg(test)]
mod audio_evdet_ffi_tests {
    use super::*;

    #[test]
    fn audio_evdet_ffi_round_trips() {
        // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
        // documented for this FFI function.
        unsafe {
            let d = ayther_audio_evdet_new();
            for h in [0xA_u64, 0xB, 0xC] {
                ayther_audio_evdet_push(d, h);
            }
            ayther_audio_evdet_flush(d);

            assert_eq!(ayther_audio_evdet_event_count(d), 1);

            let mut buf = [AytherAudioEvent {
                signature: 0,
                instrument: 0,
                start_frame: 0,
                end_frame: 0,
                chip: 0,
                channel: 0,
                pitch: 255,
                velocity: 0,
            }; 4];
            let n = ayther_audio_evdet_get_events(d, buf.as_mut_ptr(), buf.len() as u32);
            assert_eq!(n, 1);
            assert_eq!(buf[0].start_frame, 0);
            assert_eq!(buf[0].end_frame, 2);
            assert_eq!(buf[0].chip, 255, "255 = detector por batches (mezcla)");
            assert_ne!(buf[0].signature, 0);

            ayther_audio_evdet_free(d);
        }
    }
}

// ---------------------------------------------------------------------------
// Audio-event substitution FFI round-trip.
// ---------------------------------------------------------------------------
#[cfg(test)]
mod audio_sub_event_ffi_tests {
    use super::*;

    #[test]
    fn resolve_events_ffi_round_trips() {
        // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
        // documented for this FFI function.
        unsafe {
            let sub = ayther_audio_sub_new();
            let asset = std::ffi::CString::new("music/zone1.ogg").unwrap();
            ayther_audio_sub_add_event_override(sub, 0xBEEF, asset.as_ptr(), 1);
            // Override, not catalog → catalog stays empty.
            assert_eq!(ayther_audio_sub_event_catalog_len(sub), 0);

            let events = [AytherAudioEvent {
                signature: 0xBEEF,
                instrument: 0,
                start_frame: 5,
                end_frame: 20,
                chip: 0,
                channel: 0,
                pitch: 255,
                velocity: 0,
            }];
            let mut out = [AytherAudioEventSub {
                signature: 0,
                start_frame: 0,
                end_frame: 0,
                asset_path: [0; 256],
                looping: 0,
            }; 2];
            let n = ayther_audio_sub_resolve_events(sub, events.as_ptr(), 1, out.as_mut_ptr(), 2);

            assert_eq!(n, 1);
            assert_eq!(out[0].signature, 0xBEEF);
            assert_eq!((out[0].start_frame, out[0].end_frame), (5, 20));
            assert_eq!(out[0].looping, 1);
            let path = std::ffi::CStr::from_ptr(out[0].asset_path.as_ptr())
                .to_str()
                .unwrap();
            assert_eq!(path, "music/zone1.ogg");

            ayther_audio_sub_free(sub);
        }
    }
}

// ---------------------------------------------------------------------------
// SoundFont synthesis for voices assigned to game timbres.
//
// Synthesis remains on the Rust side of the existing FFI boundary so the static
// Engine library does not acquire LGPL relinking or dynamic-distribution duties.
// ---------------------------------------------------------------------------

/// Opens a SoundFont from bytes in memory.
///
/// Returns null on failure. Release a successful result with [`ayther_sf2_free`].
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sf2_new(
    data: *const u8,
    len: usize,
    sample_rate: i32,
) -> *mut sf2::Sf2Synth {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if data.is_null() || len == 0 || sample_rate < 8000 {
            return std::ptr::null_mut();
        }
        let bytes = std::slice::from_raw_parts(data, len);
        match sf2::Sf2Synth::new(bytes, sample_rate) {
            Ok(s) => Box::into_raw(Box::new(s)),
            Err(e) => {
                eprintln!("[sf2] {e}");
                std::ptr::null_mut()
            }
        }
    }
}

/// Opens a SoundFont while sharing parsed datos among instances with the same key.
///
/// The key is normally derived from the source path. Sharing avoids duplicating
/// large SoundFonts when the engine creates one synthesizer instance per timbre.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sf2_new_shared(
    key: u64,
    data: *const u8,
    len: usize,
    sample_rate: i32,
) -> *mut sf2::Sf2Synth {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if data.is_null() || len == 0 || sample_rate < 8000 {
            return std::ptr::null_mut();
        }
        let bytes = std::slice::from_raw_parts(data, len);
        match sf2::Sf2Synth::new_shared(key, bytes, sample_rate) {
            Ok(s) => Box::into_raw(Box::new(s)),
            Err(e) => {
                eprintln!("[sf2] {e}");
                std::ptr::null_mut()
            }
        }
    }
}

/// Releases cached SoundFonts no longer referenced by any synthesizer instance.
#[unsafe(no_mangle)]
pub extern "C" fn ayther_sf2_trim_cache() {
    sf2::Sf2Synth::trim_font_cache();
}

#[unsafe(no_mangle)]
/// Frees a SoundFont synthesizer instance.
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sf2_free(p: *mut sf2::Sf2Synth) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if !p.is_null() {
            drop(Box::from_raw(p));
        }
    }
}

/// Selects a channel preset with a MIDI Program Change.
///
/// Bank selection uses MIDI controllers 0 and 32; this call alone is sufficient
/// for the common bank-zero case.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sf2_program(p: *mut sf2::Sf2Synth, ch: i32, preset: i32) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if let Some(s) = p.as_mut() {
            s.program_change(ch, preset);
        }
    }
}

/// Sends a MIDI Control Change, such as CC 7 channel volume in 0–127.
///
/// Per-timbre gain is applied at the channel level because one SoundFont may
/// serve several timbres simultaneously.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sf2_control(p: *mut sf2::Sf2Synth, ch: i32, cc: i32, value: i32) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if let Some(s) = p.as_mut() {
            s.control_change(ch, cc, value);
        }
    }
}

#[unsafe(no_mangle)]
/// Starts a MIDI note on a synthesizer channel.
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sf2_note_on(p: *mut sf2::Sf2Synth, ch: i32, key: i32, vel: i32) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if let Some(s) = p.as_mut() {
            s.note_on(ch, key, vel);
        }
    }
}

#[unsafe(no_mangle)]
/// Releases a MIDI note on a synthesizer channel.
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sf2_note_off(p: *mut sf2::Sf2Synth, ch: i32, key: i32) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if let Some(s) = p.as_mut() {
            s.note_off(ch, key);
        }
    }
}

/// Stops all notes immediately, including their release tails.
///
/// Use for discontinuities such as scene changes and seeks.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sf2_all_notes_off(p: *mut sf2::Sf2Synth) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if let Some(s) = p.as_mut() {
            s.all_notes_off();
        }
    }
}

/// Renders `frames` samples into interleaved stereo `f32` output.
///
/// `out` must hold `frames * 2` values. Rendering exact emulated-frame spans
/// keeps synthesis deterministic and independent of wall-clock timing.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sf2_render(p: *mut sf2::Sf2Synth, out: *mut f32, frames: usize) {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if out.is_null() || frames == 0 {
            return;
        }
        let buf = std::slice::from_raw_parts_mut(out, frames * 2);
        match p.as_mut() {
            Some(s) => s.render_interleaved(buf),
            // Explicitly emit silence when no synthesizer exists because the caller
            // still queues this buffer and uninitialized samples would become noise.
            None => buf.fill(0.0),
        }
    }
}

/// Lists presets in an SF2 without loading it into the synthesizer.
///
/// Writes at most `cap` entries to `out_bank` and `out_preset`, and returns the
/// total number available, which may be larger than `cap`.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sf2_list_presets(
    data: *const u8,
    len: usize,
    out_bank: *mut u16,
    out_preset: *mut u16,
    cap: u32,
) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if data.is_null() || len == 0 {
            return 0;
        }
        let bytes = std::slice::from_raw_parts(data, len);
        let list = match sf2_bake::list_presets(bytes) {
            Ok(l) => l,
            Err(_) => return 0,
        };
        if !out_bank.is_null() && !out_preset.is_null() {
            for (i, (b, p, _)) in list.iter().take(cap as usize).enumerate() {
                *out_bank.add(i) = *b;
                *out_preset.add(i) = *p;
            }
        }
        list.len() as u32
    }
}

/// Lists named presets from an SF2 file, one per line as `bank:preset|nombre`.
///
/// The function reads only the RIFF `pdta` chunk from the supplied path, avoiding
/// full-file loads for large libraries. Plain text keeps string ownership out of
/// the ABI; nombres are sanitized to remove `|` and line breaks. Because only the
/// preset table is parsed, partially damaged SoundFonts may still be listed even
/// if the synthesizer rejects them. Returns bytes written, or zero for `cap = 0`,
/// insufficient capacity, or failure.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sf2_preset_list(
    path: *const std::os::raw::c_char,
    out: *mut u8,
    cap: usize,
) -> usize {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if path.is_null() || out.is_null() || cap == 0 {
            return 0;
        }
        let p = match std::ffi::CStr::from_ptr(path).to_str() {
            Ok(t) => t,
            Err(_) => return 0,
        };
        // An SFZ file represents one instrument: bank 0, preset 0, named after the
        // file. Listing must not load samples because library browsing touches many
        // files at once. SF3 follows the normal path because its `pdta` matches SF2.
        if std::path::Path::new(p)
            .extension()
            .map(|e| e.to_string_lossy().eq_ignore_ascii_case("sfz"))
            .unwrap_or(false)
        {
            let stem = std::path::Path::new(p)
                .file_stem()
                .map(|s| s.to_string_lossy().into_owned())
                .unwrap_or_else(|| "SFZ".into());
            let clean: String = stem
                .chars()
                .map(|c| {
                    if c == '|' || c == '\n' || c == '\r' {
                        ' '
                    } else {
                        c
                    }
                })
                .collect();
            let s = format!("0:0|{}\n", clean.trim());
            let b = s.as_bytes();
            if b.len() > cap {
                return 0;
            }
            std::ptr::copy_nonoverlapping(b.as_ptr(), out, b.len());
            return b.len();
        }
        let list = match sf2_bake::list_presets_file(p) {
            Ok(l) => l,
            Err(_) => return 0,
        };
        let mut s = String::new();
        for (bank, preset, name) in list {
            let clean: String = name
                .chars()
                .map(|c| {
                    if c == '|' || c == '\n' || c == '\r' {
                        ' '
                    } else {
                        c
                    }
                })
                .collect();
            s.push_str(&format!("{bank}:{preset}|{}\n", clean.trim()));
        }
        let b = s.as_bytes();
        if b.len() > cap {
            return 0;
        }
        std::ptr::copy_nonoverlapping(b.as_ptr(), out, b.len());
        b.len()
    }
}

/// Lists SoundFonts referenced by `instruments.toml`.
///
/// Each line is `basename|bank:preset,bank:preset,...`, grouping the presets that
/// must be baked from one source file. Returns bytes written, or zero when the
/// output does not fit or no references exist.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_instruments_soundfonts(
    toml_text: *const std::os::raw::c_char,
    out: *mut u8,
    cap: usize,
) -> usize {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if toml_text.is_null() || out.is_null() || cap == 0 {
            return 0;
        }
        let text = match std::ffi::CStr::from_ptr(toml_text).to_str() {
            Ok(t) => t,
            Err(_) => return 0,
        };
        let subs = instrument_map::instruments_from_toml(text);
        let mut s = String::new();
        for (sf, presets) in instrument_map::soundfonts_used(&subs) {
            s.push_str(&sf);
            s.push('|');
            for (i, (b, p)) in presets.iter().enumerate() {
                if i > 0 {
                    s.push(',');
                }
                s.push_str(&format!("{b}:{p}"));
            }
            s.push('\n');
        }
        let bytes = s.as_bytes();
        if bytes.len() > cap {
            return 0;
        }
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), out, bytes.len());
        bytes.len()
    }
}

/// Normalizes a disk SoundFont to plain SF2 bytes in memory.
///
/// `.sf2` is passed through, `.sf3` Vorbis samples are decoded to PCM, and `.sfz`
/// text plus external samples becomes preset 0:0. Downstream synthesis and pack
/// code therefore handles only SF2. Call first with `cap = 0` to obtain the
/// required size, then again to copy the path-cached conversion. Returns zero on
/// conversion failure.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_soundfont_normalize_file(
    path: *const std::os::raw::c_char,
    out: *mut u8,
    cap: usize,
) -> usize {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        use std::sync::{Mutex, OnceLock};
        type CachedSoundFont = Option<(String, Vec<u8>)>;
        static CACHE: OnceLock<Mutex<CachedSoundFont>> = OnceLock::new();
        if path.is_null() {
            return 0;
        }
        let p = match std::ffi::CStr::from_ptr(path).to_str() {
            Ok(t) => t,
            Err(_) => return 0,
        };

        let cache = CACHE.get_or_init(|| Mutex::new(None));
        let mut slot = match cache.lock() {
            Ok(g) => g,
            Err(_) => return 0,
        };
        let hit = matches!(&*slot, Some((k, _)) if k == p);
        if !hit {
            let pb = std::path::Path::new(p);
            let ext = pb
                .extension()
                .map(|e| e.to_string_lossy().to_ascii_lowercase())
                .unwrap_or_default();
            let norm = if ext == "sfz" {
                match sfz::to_sf2(pb) {
                    Ok(v) => v,
                    Err(e) => {
                        eprintln!("[soundfont] {e}");
                        return 0;
                    }
                }
            } else {
                let raw = match std::fs::read(pb) {
                    Ok(v) => v,
                    Err(e) => {
                        eprintln!("[soundfont] {p}: {e}");
                        return 0;
                    }
                };
                match sf3::to_sf2(&raw) {
                    Ok(Some(v)) => v, // era SF3
                    Ok(None) => raw,  // Plain SF2; no conversion is needed.
                    Err(e) => {
                        eprintln!("[soundfont] {p}: {e}");
                        return 0;
                    }
                }
            };
            *slot = Some((p.to_string(), norm));
        }
        let bytes = match &*slot {
            Some((_, v)) => v,
            None => return 0,
        };
        if cap == 0 || bytes.len() > cap {
            return bytes.len();
        }
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), out, bytes.len());
        bytes.len()
    }
}

/// Bakes an SF2 containing only the requested `(bank, preset)` pairs.
///
/// Returns bytes written to `out`, or zero on failure or insufficient capacity.
/// Call first with `cap = 0` to query the required size.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_sf2_bake(
    src: *const u8,
    src_len: usize,
    banks: *const u16,
    presets: *const u16,
    n: u32,
    out: *mut u8,
    cap: usize,
) -> usize {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if src.is_null() || src_len == 0 || banks.is_null() || presets.is_null() || n == 0 {
            return 0;
        }
        let s = std::slice::from_raw_parts(src, src_len);
        let mut keep = Vec::with_capacity(n as usize);
        for i in 0..n as usize {
            keep.push((*banks.add(i), *presets.add(i)));
        }
        let (baked, rep) = match sf2_bake::bake(s, &keep, "Ayther") {
            Ok(v) => v,
            Err(e) => {
                eprintln!("[sf2 bake] {e}");
                return 0;
            }
        };
        if !rep.missing.is_empty() {
            eprintln!(
                "[sf2 bake] {} preset(s) pedidos no estaban en el origen",
                rep.missing.len()
            );
        }
        if cap == 0 || baked.len() > cap {
            return baked.len();
        }
        std::ptr::copy_nonoverlapping(baked.as_ptr(), out, baked.len());
        baked.len()
    }
}

// ---------------------------------------------------------------------------
// User-supplied IPS/BPS patches applied in memory.
// ---------------------------------------------------------------------------

/// Returns whether the bytes contain an IPS or BPS parche, based on file magic.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_is_rom_patch(data: *const u8, n: u32) -> bool {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if data.is_null() || n == 0 {
            return false;
        }
        crate::rom_patch::is_patch(std::slice::from_raw_parts(data, n as usize))
    }
}

/// Applies `parche` to `rom` and writes the result to caller-owned `out` storage.
///
/// Returns bytes written, or a negative status: -1 invalid arguments, -2 unknown
/// parche format, -3 insufficient output capacity, or -4 parche failure. Detailed
/// failure text is available from [`ayther_rom_patch_error`]. With `out_cap = 0`,
/// the function reports -3 and writes the required size to `out_needed`.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_apply_rom_patch(
    rom: *const u8,
    rom_n: u32,
    patch: *const u8,
    patch_size: u32,
    out: *mut u8,
    out_cap: u32,
    out_needed: *mut u32,
) -> i64 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if rom.is_null() || patch.is_null() {
            return -1;
        }
        let r = std::slice::from_raw_parts(rom, rom_n as usize);
        let p = std::slice::from_raw_parts(patch, patch_size as usize);
        if !crate::rom_patch::is_patch(p) {
            return -2;
        }
        match crate::rom_patch::apply(r, p) {
            Ok(v) => {
                if !out_needed.is_null() {
                    *out_needed = v.len() as u32;
                }
                if out.is_null() || (v.len() as u32) > out_cap {
                    return -3;
                }
                std::ptr::copy_nonoverlapping(v.as_ptr(), out, v.len());
                v.len() as i64
            }
            Err(e) => {
                LAST_PATCH_ERROR.with(|c| *c.borrow_mut() = e.message());
                -4
            }
        }
    }
}

thread_local! {
    static LAST_PATCH_ERROR: std::cell::RefCell<String> =
        const { std::cell::RefCell::new(String::new()) };
}

/// Returns a display-ready explanation of the latest ROM-parche failure.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_rom_patch_error(buf: *mut u8, cap: u32) -> u32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if buf.is_null() || cap == 0 {
            return 0;
        }
        LAST_PATCH_ERROR.with(|c| {
            let m = c.borrow();
            let n = m.len().min(cap as usize - 1);
            std::ptr::copy_nonoverlapping(m.as_ptr(), buf, n);
            *buf.add(n) = 0;
            n as u32
        })
    }
}

// ---------------------------------------------------------------------------
// Brightness-independent tile-shape families.
// ---------------------------------------------------------------------------

/// Computes the shape hash of a 32-byte planar 4-bpp tile.
///
/// The result is brightness-invariant but silhouette-sensitive. It complements
/// the exact identity hash by grouping visually related tiles.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tile_shape_hash(tile: *const u8, n: u32) -> u64 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if tile.is_null() || n < 32 {
            return 0;
        }
        crate::shape_hash::shape_hash(std::slice::from_raw_parts(tile, 32))
    }
}

/// Returns the mean level of opaque tile pixels in 0–15.
///
/// A negative result means the tile is fully transparent rather than black.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tile_mean_level(tile: *const u8, n: u32) -> f32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if tile.is_null() || n < 32 {
            return -1.0;
        }
        crate::shape_hash::mean_level(std::slice::from_raw_parts(tile, 32)).unwrap_or(-1.0)
    }
}

/// Returns the brightness factor of `tile` relative to `reference`.
///
/// This is the attenuation needed to reproduce the tile from the brightest group
/// asset. A negative result means either tile has no opaque pixels.
#[unsafe(no_mangle)]
/// # Safety
///
/// The caller must uphold the crate-level FFI safety contract documented at the
/// crate root, including all pointer, buffer, lifetime, and ownership requirements.
pub unsafe extern "C" fn ayther_tile_brightness_factor(
    tile: *const u8,
    referencia: *const u8,
) -> f32 {
    // SAFETY: The caller upholds the pointer, lifetime, and ownership invariants
    // documented for this FFI function.
    unsafe {
        if tile.is_null() || referencia.is_null() {
            return -1.0;
        }
        crate::shape_hash::brightness_factor(
            std::slice::from_raw_parts(tile, 32),
            std::slice::from_raw_parts(referencia, 32),
        )
        .unwrap_or(-1.0)
    }
}
