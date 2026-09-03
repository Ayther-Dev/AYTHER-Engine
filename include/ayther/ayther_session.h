#pragma once
// ---------------------------------------------------------------------------
// ayther_session.h — AytherSession, the motor's control facade (R2).
//
// AytherSession is the single, coherent surface the frontends drive instead of
// wiring the ~9 raw ayther_core handles by hand. It owns the whole deterministic
// pipeline — emulator host + tile/sprite/audio hashing + substitution + Lua
// scripting + HD audio output — behind one object, and exposes the *result* of
// each frame as a plain-data FrameView for the frontend to render.
//
// Engine / frontend boundary (see docs/ARCHITECTURE.md#session-and-renderer-boundary):
//
//   AytherSession (motor, this object)        Frontend (ayther_play / ayther_lab)
//   ----------------------------------        -----------------------------------
//   run_frame() the libretro core             window + SDL events + input source
//   hash tiles/sprites/audio                   upload FrameView.fb -> VkTexture
//   fire Lua on_frame, resolve overrides       draw tile/sprite substitutions
//   resolve substitutions                      CRT post-process / present
//   play + mute + flush HD audio   <-- audio   Lab authoring UI / timeline
//   produce a FrameView  ----------------->    consume FrameView, render it
//
// Audio output lives entirely inside the session (the AudioPlayer is owned by
// the motor in R1): play/mute/flush happen in step(), nothing audio crosses the
// boundary. The frontend never touches Vulkan-from-the-motor or SDL-audio.
//
// No-throw: construction and fallible operations return ayther::Result (§4.1.1);
// nothing throws across the FFI users. Opaque handles are held as
// ayther::unique_handle inside the pimpl — no raw Rust pointer lives loose (§4.1).
//
// Threading: a session is single-owner and must be driven from one thread (the
// emulation thread), matching ayther_core's rule. Non-copyable; movable.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "audio_asset_level.h"    // AudioAssetLevel (measured level of an asset)
#include "audio_match_rule.h"     // AudioMatchRule (F3: match rules)
#include "ayther_animation.h"     // AnimationPlayer / AnimHdFrame / HdPose (C-S2)
#include "ayther_audio_events.h"  // AudioEventSubstitution / AudioEventAssignment (C-A2)
#include "ayther_core_ffi.h"   // AytherTileSub, AytherSpriteSub, *Occurrence (POD)
#include "ayther_layers.h"     // AytherLayerContent (the pack's Acetates)
#include "ayther_mode3.h"      // Mode3Resolver / EntityInstance (Mode 3, RAM anchoring)
#include "ayther_result.h"     // ayther::Result / Error
#include "engine/input.hpp"    // typed public joypad input contract
#include "engine/pack.hpp"     // typed, non-owning pack boundary for frontends

namespace ayther {

struct AytherRecording;   // ayther_recording.h — deterministic .arp take (R7)

// A tile of one VDP plane (A/B/Window) present this frame — deduplicated by
// pattern content (not by cell). Identity by pattern hash (+ palette), stable
// across frames/sessions (content-based, like sprites/tiles). AytherSession
// computes it by reading the nametable + VRAM (Phase 2 of the Layers panel).
// `cell_x/y` is a REPRESENTATIVE cell (the first) — for data/preview, not a
// screen position (planes scroll).
struct PlaneTileOccurrence {
    uint64_t hash    = 0;
    uint16_t cell_x  = 0;   ///< representative nametable cell
    uint16_t cell_y  = 0;
    uint16_t pattern = 0;   ///< pattern index (0..2047) for decoding
    uint8_t  plane   = 0;   ///< 0 = Plane A · 1 = Plane B · 2 = Window
    uint8_t  palette = 0;   ///< 0..3 (CRAM)
    uint8_t  hflip   = 0;
    uint8_t  vflip   = 0;
};

/// RE-PALETTING of a plane tile hash: returns the hash THE SAME pattern would
/// have under another CRAM line. Without touching VRAM and without the variant
/// being on screen.
///
/// It is possible because the hash is `FNV1a(32 bytes of the pattern)` and the
/// palette is mixed in AT THE END — `h = (h_pattern ^ pal) * PRIME` — with an
/// odd PRIME, i.e. invertible mod 2^64. Undoing the last round, changing the
/// line and redoing it is exact arithmetic, not an approximation. (See the hash
/// computation in ayther_session.cpp, `collect_plane_tiles`.)
///
/// It is what makes cloning a Character or a character set to another colour
/// cheap: the clone receives the hashes of the variant and, since the element
/// id mixes them in, its identity comes out different on its own.
///
/// Oracle (paint_repalette_smoke): on any frame, for every pair of
/// PlaneTileOccurrence with the SAME `pattern` and a different `palette`,
/// `repalette(a.hash, a.palette, b.palette) == b.hash` must hold.
inline uint64_t ayther_plane_tile_hash_repalette(uint64_t h, uint8_t from,
                                                 uint8_t to) {
    constexpr uint64_t kPrime = 1099511628211ULL;
    // Inverse of kPrime mod 2^64 by Newton-Raphson (6 steps suffice for 64
    // bits: each iteration doubles the number of correct bits).
    uint64_t inv = 1;
    for (int i = 0; i < 6; ++i) inv *= 2ULL - kPrime * inv;
    return ((h * inv) ^ (uint64_t)(from & 3u) ^ (uint64_t)(to & 3u)) * kPrime;
}

/// The FOUR readings of a cell hash observed under line `pal`: the same cell as
/// it would have been hashed under each CRAM line. `out[0]` is the hash as-is,
/// so a searcher can try the direct path first and pay nothing in the normal
/// case.
///
/// It serves for comparing against indexes that store hashes WITHOUT their
/// palette — `PanoramaCell` is hash + position, and that is also the pack
/// format — which would otherwise come unstuck when the game reassigns the cell
/// to another line (the day/night cycles that REPAINT instead of changing the
/// content of the line). All four are exact, not approximations: see
/// `..._repalette`.
///
/// Oracle: `plane_hash_variants` (round-trip, coverage of the 4 lines, and the
/// absence of collisions between different patterns).
inline void ayther_plane_tile_hash_variants(uint64_t h, uint8_t pal,
                                            uint64_t out[4]) {
    out[0] = h;
    unsigned n = 1;
    for (uint8_t to = 0; to < 4; ++to)
        if (to != (pal & 3u))
            out[n++] = ayther_plane_tile_hash_repalette(h, pal, to);
}

// A VISIBLE cell of a plane this frame, with its SCREEN position already
// resolved scroll-aware (the same inverse as the Phase 2c HD resolver). Unlike
// PlaneTileOccurrence (deduplicated by content), here there is ONE entry per
// on-screen appearance → the Lab synchronises the viewport with the Layers
// panel: it draws the outline of a selected plane tile and picks the background
// under the cursor. Empty without a core exposing VRAM+VSRAM.
struct PlaneCellHit {
    uint64_t hash     = 0;   ///< content identity (== PlaneTileOccurrence.hash)
    int16_t  screen_x = 0;   ///< px of the top-left corner of the 8×8 tile
    int16_t  screen_y = 0;
    uint8_t  plane    = 0;   ///< 0 = Plane A · 1 = Plane B · 2 = Window
    /// R-3: the SOURCE of this cell (the nametable word already carries it) —
    /// without this the scene inventory would have to re-derive
    /// pattern/palette from the hash, which is flip-invariant and therefore
    /// ambiguous.
    uint16_t pattern  = 0;   ///< VRAM pattern index 0..2047
    uint8_t  palette  = 0;   ///< CRAM line 0..3
    /// Flips/priority OF THIS CELL (from the nametable word, Phase C): bit0 =
    /// hflip · bit1 = vflip · bit2 = VDP priority. Unlike the occurrence
    /// (representative by content), here they are the real ones per appearance
    /// — the Paint capture uses them for the faithful export.
    /// R-3: bit3 = PARTIAL edge cell (negative screen x/y from scroll not
    /// aligned to 8). The partial ones go at the END of plane_cells, after the
    /// cells that take part in the Picture signature — that prefix did not
    /// change and the authored signatures remain valid.
    uint8_t  flags    = 0;
};

// R-3: a DRAWABLE element of the frame in the single scene list — plane cell or
// sprite — with position, source, layer, priority and stable identity. The
// ORDER in the list is the global draw order (back→front): B pri0 · A pri0 ·
// Window pri0 · sprites pri0 · then the same for high priority; sprites within
// their group go from back to front (the SAT chain reversed).
// CAREFUL (VDP semantics the list does NOT flatten): between sprites the first
// of the CHAIN wins regardless of priority, and that single pixel competes with
// the planes at ITS priority — an exact compositor passes the sprites through a
// first-wins buffer and blends afterwards. R-5: FrameView publishes it (scene)
// when the scene compose is ON — which is why it lives at namespace level.
struct SceneElement {
    uint64_t hash     = 0;    ///< stable identity (flip-invariant hash); 0 = no hash
    int16_t  x = 0, y = 0;    ///< screen px (top-left corner)
    uint8_t  w = 8, h = 8;    ///< size in px
    uint16_t pattern  = 0;    ///< VRAM pattern (cell) / base tile (sprite)
    uint8_t  palette  = 0;    ///< CRAM line 0..3
    uint8_t  flips    = 0;    ///< bit0 hflip · bit1 vflip
    uint8_t  layer    = 0;    ///< 0=Plane B · 1=Plane A · 2=Window · 3=Sprite
    uint8_t  priority = 0;    ///< the VDP priority bit
    uint8_t  slot     = 0xFF; ///< sprite: SAT slot 0-79; planes: 0xFF
    /// Sprite: position in the link CHAIN at parse time (lower = further
    /// front). It is the real priority between sprites and it is GLOBAL (it
    /// crosses the priority groups). Planes: 0xFF.
    uint8_t  chain    = 0xFF;
    uint8_t  sub_kind = 0;    ///< HD source: 0=pure VRAM · 1=fv.sprite_subs · 2=fv.plane_tile_subs
    uint8_t  hidden   = 0;    ///< R-4: 1 = hidden (set_hidden_elements or the Lab channel)
    /// R-5: 1 = an already-resolved HD asset REPLACES this element (a direct
    /// sub, a claimed member of a pose, or a cell consumed by a set) → the
    /// per-element compose does not draw the original while HD is active. It is
    /// what replaces the suppression channels: what does not win is not
    /// emitted.
    uint8_t  claimed  = 0;
    /// R-6: element EFFECTS (set_element_effects, by layer+hash) — with the
    /// indexed pipeline they are one uniform per quad, not a lane. Multiplicative
    /// Q2.6 tint per channel (64 = neutral, the same format as the E1 of the HD
    /// sprites; >64 = brighter), opacity 0-255 (255 = opaque), and an authoring
    /// outline (AABBGGRR colour behind the element; 0 = none).
    uint8_t  fx_tint[3] = { 64, 64, 64 };
    uint8_t  fx_opacity = 255;
    uint32_t fx_outline = 0;
    /// runtime_enhancement: 1 = the element is drawn with the EPX upscaler over
    /// palette indices ("Enhance in software"). It is only published if the
    /// element is NOT claimed by HD (the asset won). The identity is
    /// (layer, hash), like hidden/fx: other appearances of the same graphic are
    /// enhanced too (documented semantics, not something to "fix").
    uint8_t  fx_enhance = 0;
    /// Smoothing strength 0..255 (it only means something with
    /// fx_enhance = 1). 255 = clean vector · 0 = pixel art barely rounded.
    uint8_t  fx_enhance_k = 255;
    int32_t  sub      = -1;   ///< index into the corresponding subs array (-1 = none)
};

// R-4: identity of a HIDDEN element of the inventory — (layer, hash), not the
// hash alone: the same graphic can exist as a sprite AND as a plane tile (the
// same flip-invariant hash in both domains — element_hidden_smoke caught it),
// and hiding the sprite must not hide the cells.
struct HiddenElement {
    uint64_t hash  = 0;
    uint8_t  layer = 3;   ///< SceneElement.layer: 0=B · 1=A · 2=W · 3=Sprite
};

// runtime_enhancement: identity of an element to ENHANCE in software — the same
// (layer, hash) identity as hiding and effects. The session unites two sources:
// the Lab's live list (set_enhanced_elements) and the pack's ([[enhance]] in
// elements.toml, load_pack_into). The inventory publishes it in
// SceneElement.fx_enhance and the indexed compose applies it as one bit per
// quad.
struct EnhancedElement {
    uint64_t hash  = 0;
    uint8_t  layer = 3;   ///< SceneElement.layer: 0=B · 1=A · 2=W · 3=Sprite
    uint8_t  k     = 255; ///< strength 0..255
};

// R-6: an effect assigned to an element of the inventory — the same
// (layer, hash) identity as hiding. The fields are those of SceneElement.fx_*:
// the inventory resolves them per element and the compose applies them as a
// uniform per quad (the point of the epic: adding an effect is a shader, not
// architecture).
struct ElementEffect {
    uint64_t hash    = 0;
    uint8_t  layer   = 3;              ///< 0=B · 1=A · 2=W · 3=Sprite
    uint8_t  tint[3] = { 64, 64, 64 }; ///< Q2.6 per channel (64 = neutral)
    uint8_t  opacity = 255;            ///< 0-255 (255 = opaque)
    uint32_t outline = 0;              ///< outline AABBGGRR (0 = none)
};

// ---------------------------------------------------------------------------
// FrameView — the motor -> frontend data boundary for exactly one stepped frame.
//
// Lifetime: every pointer below is owned by the session and valid only until the
// next call that mutates session state — step(), reset(), unserialize(),
// set_pack() or reload_pack(). The frontend must copy anything it needs to keep
// (e.g. a thumbnail) before the next step().
// ---------------------------------------------------------------------------
struct FrameView {
    // -- Emulator framebuffer: the base image to upload + present --------------
    // fb_pixels is null when the core duplicated the previous frame (no redraw);
    // the frontend then keeps last frame's texture.
    const void* fb_pixels   = nullptr;
    uint32_t    fb_width    = 0;
    uint32_t    fb_height   = 0;
    uint32_t    fb_pitch    = 0;       ///< bytes per row
    int         fb_format   = 0;       ///< RETRO_PIXEL_FORMAT_* (frontend maps to VkFormat)

    // -- Resolved tile substitutions: HD asset to draw per on-screen tile ------
    const AytherTileSub*   tile_subs        = nullptr;
    uint32_t               tile_sub_count   = 0;

    // -- Resolved sprite substitutions: HD alpha-blended overlay sprites -------
    const AytherSpriteSub* sprite_subs      = nullptr;
    uint32_t               sprite_sub_count = 0;
    const uint8_t*         sprite_sub_flips = nullptr;  ///< CU-AN-11: parallel to sprite_subs
                                                        ///< (bit0 hflip, bit1 vflip) → auto-mirror
    /// CHROMATIC E1 (fade + palette change): RGB tint per sub, 3 bytes per
    /// entry (stride 3, parallel to sprite_subs), Q2.6 fixed point (64 = 1.0,
    /// max ~3.98). The renderer multiplies the HD colour by this tint so it
    /// follows the game's fades AND its flashes/colour changes (an orange flash
    /// tints the HD, it does not merely darken it; >64 = brighter than normal →
    /// it saturates towards the flash). Reference per sub: the authored one
    /// (the pose's `ref_rgb`) or, without it, the scalar per-palette peak-hold
    /// (classic E1 behaviour, grey tint).
    const uint8_t*         sprite_sub_tint  = nullptr;
    /// C8 (z-order between overlapping HD): SAT slot 0-79 per sub, parallel to
    /// sprite_subs (the lowest slot among the occurrences overlapping the sub =
    /// the frontmost member). The renderer draws the subs by DESCENDING slot
    /// (frontmost last) to respect sprite-vs-sprite occlusion. 255 = no
    /// overlapping occurrence (at the back).
    const uint8_t*         sprite_sub_slot  = nullptr;
    /// VDP PRIORITY bit per sub (0/1), parallel to sprite_subs — that of the
    /// exact occurrence, or that of the frontmost of the bbox. The hardware
    /// orders sprite pri-1 > plane A pri-1 (the letters of the GA title orbit
    /// the logo by toggling the bit): the renderer draws the pri-1 subs IN
    /// FRONT of the HD foreground, and the pri-0 ones behind, PER FRAME.
    const uint8_t*         sprite_sub_prio  = nullptr;

    // -- Resolved PLANE-tile substitutions (Phase 2c): the HD overlay of a
    //    background tile, resolved scroll-aware to screen positions. The sprite
    //    struct is reused (a 1×1 quad with alpha) and the renderer draws them
    //    BELOW the real sprites. Empty with no assignments / without a core
    //    exposing VSRAM.
    const AytherSpriteSub* plane_tile_subs      = nullptr;
    uint32_t               plane_tile_sub_count = 0;
    const uint8_t*         plane_tile_flips     = nullptr;  ///< parallel: bit0 hflip, bit1 vflip
    uint32_t               plane_tile_sub_hi    = 0;        ///< [0,hi)=low prio (under sprites); [hi,count)=high (over)
    /// E1 tint per plane sub (stride 3, Q2.6 — the same contract as
    /// sprite_sub_tint). Today only the SET quads with an authored reference
    /// (the element's `ref`) come out other than 64/64/64: live/ref PER CHANNEL
    /// of the anchor's CRAM line, so the Object follows the game's palette fades
    /// (the GA title logo used to appear in full colour over the black pre-fade
    /// CRAM, report 2026-08-19). Without a reference = neutral (byte-exact with
    /// the previous behaviour).
    const uint8_t*         plane_tile_sub_tint  = nullptr;

    // -- Mode 3 (RAM anchoring): PER-INSTANCE HD entity substitution ----------
    //    One AytherSpriteSub per anchored instance whose kind has an assigned
    //    asset (assign_kind), over the aggregate bbox of its SAT sprites — it
    //    disambiguates identical entities by the world_pos read from RAM (the
    //    game profile). The renderer draws them in the sprite lane.
    //    `entity_instances` lists EVERY located instance (with or without an
    //    asset) for the Lab's authoring overlay (box + id). Empty without a
    //    profile / without VSRAM (stock core).
    const AytherSpriteSub* entity_subs           = nullptr;
    uint32_t               entity_sub_count      = 0;
    const EntityInstance*  entity_instances      = nullptr;
    uint32_t               entity_instance_count = 0;

    // -- C-S2 animations: HD frames in phase ---------------------------------
    //    For every on-screen occurrence whose clip (anim_group_id) has an HD
    //    animation defined and whose pose (hash) is in the sheet: the sheet
    //    frame (UV sub-rect) at the observed bbox (Pop) or at the tweened
    //    transform (Level 1). The renderer draws them with
    //    VkSprite::draw_anim.
    const AnimHdFrame* anim_frames      = nullptr;
    uint32_t           anim_frame_count = 0;

    // -- Per-frame occurrences + catalog telemetry (authoring / Lab / overlay) -
    const AytherTileOccurrence*   tile_occs        = nullptr;  uint32_t tile_occ_count   = 0;
    const AytherSpriteOccurrence* sprite_occs      = nullptr;  uint32_t sprite_occ_count = 0;
    const AytherAudioOccurrence*  audio_occs       = nullptr;  uint32_t audio_occ_count  = 0;
    uint32_t unique_tile_count   = 0;
    uint32_t unique_sprite_count = 0;
    uint32_t unique_audio_count  = 0;

    // -- Raw sound-chip bus writes this frame (FM YM2612 + PSG SN76489), in
    //    temporal order — the basis of command-based audio events (workspace
    //    Audios). Replay-stable as a command sequence (the cycle timestamp is
    //    not — see AytherAudioWrite). Empty with a stock core (no id 0x109).
    //    Valid until the next step(); the motor owns the backing buffer.
    const AytherAudioWrite* chip_writes      = nullptr;
    uint32_t                chip_write_count = 0;

    // -- Per-event audio substitution (C-A3b): the mask of channels muted this
    //    frame by active ASSIGNED events, and the list of active subs (so
    //    playback fires the HD asset in sync). Only with the preview active
    //    (set_audio_substitution_preview) over the analysed take; 0 / empty
    //    otherwise.
    uint32_t audio_mute_mask = 0;
    const AytherAudioActiveSub* audio_active_subs      = nullptr;
    uint32_t                    audio_active_sub_count = 0;

    // -- A/B/Window plane coverage: nametable cells with tile != 0.
    //    For the Plane A / Plane B / Window lanes of the Edit timeline.
    //    Derived from the VDP regs (0x101) + VRAM; 0 if the core does not
    //    expose them.
    uint32_t plane_a_count = 0;
    uint32_t plane_b_count = 0;
    uint32_t plane_w_count = 0;   ///< Window (HUD/static readout), reg $3

    // -- Per-tile elements of each plane (Phase 2, Layers panel) — deduplicated
    //    by pattern content. For listing/naming/previewing the tiles of Plane
    //    A/B/Window. Empty if the core does not expose VRAM/regs.
    const PlaneTileOccurrence* plane_tile_occs      = nullptr;
    uint32_t                   plane_tile_occ_count = 0;

    // -- VISIBLE cells of the planes with their screen position (Phase 2c) —
    //    one per appearance (not deduplicated). The Lab uses them for the
    //    selection outline and for background picking (viewport↔Layers sync).
    //    Empty without VSRAM.
    const PlaneCellHit*        plane_cells      = nullptr;
    uint32_t                   plane_cell_count = 0;

    // -- Whole-plane scroll of this frame (VDP px, 0..1023) — the input to the
    //    BackgroundStitcher (Case B/C): it gives the camera position in level
    //    space so tiles can be accumulated over a take. H sampled at line 0; V
    //    global (col 0). The Window does not scroll → 0. 0 if the core does not
    //    expose VSRAM.
    // -- R-5: the per-element SCENE — the input to our own renderer -----------
    //    Published only with scene compose ON (set_scene_compose / env
    //    AYTHER_SCENE_COMPOSE): the single ordered list from R-3 + the raw
    //    state the indexed pipeline needs (the fork's VRAM/CRAM, host
    //    word-swapped view) + the backdrop. With this the renderer COMPOSES the
    //    scene and the emulator frame stops being the canvas (it remains the
    //    source of truth for what is there, and for the hashers). Empty with the
    //    compose off or without the forked core.
    const SceneElement* scene       = nullptr;
    uint32_t            scene_count = 0;
    const uint8_t* scene_vram = nullptr;  size_t scene_vram_size = 0;
    const uint8_t* scene_cram = nullptr;  size_t scene_cram_size = 0;
    uint16_t scene_backdrop   = 0;   ///< background colour (packed CRAM, reg7&0x3F)
    uint8_t  scene_left_blank = 0;   ///< reg 0 bit 5: leftmost 8 px to the backdrop
    /// R-5: the frame is NOT faithfully composed from the scene → the renderer
    /// falls back to the emulator blit (the R-1 hybrid). bit0 = writes with a
    /// visual effect mid-screen (the fork's 0x10E signal); bit1 = Animation dim
    /// active (an effect of the produce over the fb, not modelled yet);
    /// bit2 = per-line/per-cell hscroll with real variation (sub-tile shear not
    /// modelled until the pipeline draws strips — R-7).
    uint8_t  scene_dirty      = 0;

    int16_t plane_hscroll[3] = {0, 0, 0};   ///< [0]=A [1]=B [2]=Window
    int16_t plane_vscroll[3] = {0, 0, 0};
    uint16_t plane_wpx = 0;   ///< width of plane A/B in px (scroll wraps mod this)
    uint16_t plane_hpx = 0;   ///< height of plane A/B in px
    /// PER-COLUMN vscroll (the VDP's VS mode, reg 11 bit 2). The clouds of the
    /// GA title scroll in 16 strips of 16 px with their own phase — the global
    /// vscroll above is only column 0. [0]=A · [1]=B; column s covers the
    /// screen at x∈[s·16, s·16+16). With vs_two_cell=false the columns replicate
    /// the global value anyway (a consumer may skip checking the flag), but the
    /// flag is the signal that the game REALLY scrolls per column (the Custom
    /// lane anchors per strip only then).
    bool    vs_two_cell = false;
    int16_t plane_vscroll_col[2][20] = {};

    // -- Camera in LEVEL space (EM-1): unwrapped scroll ACCUMULATED per plane
    //    ([0]=A · [1]=B; the Window is fixed). SEQUENTIAL tracking: valid while
    //    the frames advance one at a time (live game / linear playback); a
    //    discontinuity (seek/scrub/catch-up) re-anchors the camera at 0 and the
    //    flag drops until the next sequential frame. The level_x of a cell =
    //    plane_cam_x + screen_x — CONSTANT under scroll (the source-side
    //    background identity EM-1 asks for; the input to widescreen EM-8).
    int32_t plane_cam_x[2] = {0, 0};
    int32_t plane_cam_y[2] = {0, 0};
    bool    plane_cam_valid = false;

    // -- Per-plane SCREEN signature (Picture · CU001) ------------------------
    //    Identity of the screen the background layers form, so a static screen
    //    (title, menu, legal screen) can be recognised and replaced whole by an
    //    HD asset. Sprites do NOT take part: this comes out of the plane
    //    pick-list.
    //
    //    It accumulates per PLANE ([0]=A · [1]=B · [2]=Window) with a
    //    COMMUTATIVE combination, so the signature of any layer mask is the SUM
    //    of its planes — a Picture of "A+B" is `sig[0]+sig[1]`, without walking
    //    the cells again (see `screen_signature`).
    //
    //    The per-cell term uses the position in CELLS (screen_x >> 3), which
    //    makes it invariant to the sub-cell phase of the scroll: a title that
    //    wobbles a few px does not change the signature. If the screen scrolls a
    //    whole cell the signature does change — but that is no longer a Picture,
    //    it is a Panorama.
    //
    //    It is 0 without the forked core (with no VSRAM there is no plane
    //    pick-list).
    uint64_t screen_plane_sig[3]   = {0, 0, 0};
    uint32_t screen_plane_cells[3] = {0, 0, 0};

    //    The Picture in effect this frame (0 = none) + how much coverage it
    //    matched with, so the authoring UI can show the margin: a score right at
    //    the threshold warns that two screens are overlapping.
    uint64_t screen_match_id    = 0;
    float    screen_match_score = 0.0f;
    float    screen_match_extra = 0.0f;
    /// Mechanism 2: Pictures whose CONTENT is present this frame (>=60% of
    /// their distinct patterns, per declared layer, at any position). Invariant
    /// to scroll and to element movement — what an Acetate gate needs during an
    /// animated intro.
    uint64_t screen_presence_ids[8] = {};
    uint32_t screen_presence_count  = 0;
    /// The asset of the current Picture, full screen (0 or 1 entries).
    const AytherSpriteSub* screen_subs = nullptr;
    uint32_t               screen_sub_count = 0;

    // -- KINEMATIC (CU004): the ordered sequence in progress ------------------
    //    `screen_subs` is SHARED: when a Kinematic is in effect and its step
    //    carries its own asset, that quad is the one emitted there — the two are
    //    not emitted together. An opaque full-screen quad on top of another
    //    leaves the result at the mercy of lane order, which is what the ladder
    //    avoids.
    uint64_t kinematic_id    = 0;   ///< kinematic in progress (0 = none)
    uint32_t kinematic_step  = 0;   ///< current step within the sequence
    uint32_t kinematic_steps = 0;   ///< length of the sequence (0 = none)

    // -- The step's VIDEO -----------------------------------------------------
    //    When the step's asset is an `.ivf`, it does NOT come out through
    //    `screen_subs`: it comes out here, as already-decoded pixels, and
    //    `screen_sub_count` stays at 0.
    //
    //    The reason for the separate path is hard: `AytherSpriteSub` has no time
    //    field (adding one breaks the FFI ABI) and its `asset_path` is the KEY
    //    of the VkSprite texture cache, where a texture enters the deferred
    //    staging-release list and after a few frames stops accepting uploads
    //    SILENTLY. A video routed through there would freeze on its first frame
    //    with no error at all. See ayther_video.h.
    //
    //    The session produces pixels, the renderer uploads them. That way the
    //    decoder stays on the Vulkan-free side and can be verified headless.
    /// The decoder's THREE I420 PLANES, without a copy (null = no video). The
    /// conversion to RGB is done by the fragment shader; this used to carry a
    /// BGRA8 converted on the CPU, which was 62% of the per-frame cost.
    /// The strides are NOT the width: libvpx aligns the rows.
    const void* video_y = nullptr;
    const void* video_u = nullptr;
    const void* video_v = nullptr;
    uint32_t    video_y_stride = 0, video_u_stride = 0, video_v_stride = 0;
    uint32_t    video_w = 0, video_h = 0;
    uint32_t    video_frame = 0;         ///< decoded index within the clip
    /// Changes ONLY if the content changed. The renderer skips the re-upload
    /// when it did not: without this the full memcpy is paid for every interface
    /// frame while paused.
    uint64_t    video_seq = 0;

    // -- PANORAMA (CU003): camera ANCHORED BY CONTENT -------------------------
    //    Camera position in level PIXELS, voted by the visible cells against the
    //    declared strip. Unlike `plane_cam_*` (which is relative and re-anchors
    //    on every seek), this one is ABSOLUTE with respect to the strip and
    //    survives a scrub, a savestate load and a scene cut — it is what allows
    //    the HD texture to be drawn in the right place while the artist scrubs.
    //
    //    `panorama_votes` / `panorama_cells` give the confidence: few votes over
    //    many cells = the camera could not be fixed (a strip of pure repeated
    //    sky has no unusual hashes to anchor on).
    int32_t  panorama_cam_x = 0, panorama_cam_y = 0;
    /// Plane mask of the Kinematic's VIDEO (bit0=A · bit1=B · bit2=Window) —
    /// the UNION of the masks of its Pictures. With a composed scene the video
    /// is drawn INLINE in the pass of the highest plane of this mask, so the
    /// Window and the game's LIVE Sprites compose on top instead of being
    /// covered. 0 = no known mask ⇒ the long-standing behaviour (full screen).
    uint8_t  video_plane_mask = 0;

    /// Decision 3: 1 = the video goes at the FRONT position (the z of the VDP
    /// foreground, above the pri-1 elements and above everything HD) instead of
    /// the z of its plane.
    ///
    /// It comes from the VDP DATA, not from a checkbox the author has to
    /// remember to tick: if the live cells of the planes the video covers are
    /// mostly pri-1, the Picture is a foreground —the full-screen flash of a
    /// cutscene— and covering it with the video at the background z would leave
    /// it behind what it is precisely meant to cover.
    ///
    /// It only means something with `video_plane_mask != 0`: with no mask the
    /// video already goes full screen through the global lane, which is the old
    /// degradation.
    uint8_t  video_front = 0;

    /// Phase 0: the LOGICAL WIDTH of the widescreen, in emulator pixels.
    /// 0 = no widening (the native frame occupies the whole canvas, as always).
    /// With a value, the native frame is CENTRED and the extended area is left
    /// on both sides: 398 px with square pixels over 224, 427 preserving the
    /// displayed 4:3 — `widescreen_target_width()` computes both.
    ///
    /// The art on the sides does NOT come from the nametable: reading past it
    /// returns art from another section of the level (the nametable wraps every
    /// 512 px). It comes from the Panorama STRIP, which is what each position
    /// showed while it was on screen — which is why widening is, in practice,
    /// letting the strip draw beyond the edge.
    uint32_t wide_w = 0;
    uint64_t panorama_id    = 0;      ///< the anchored Panorama (0 = none)
    uint32_t panorama_votes = 0;      ///< visible cells that voted for the winner
    uint32_t panorama_cells = 0;      ///< visible cells that could vote
    /// EM-8.1: what fraction (0-100) of the plane's visible cells the strip
    /// EXPLAINS at the anchored position. `panorama_valid` is the yes/no; the
    /// percentage matters because the EXTENDED area demands more of it than the
    /// native one — the native area corrects itself with the live cells the
    /// strip did not claim, and the extended one has nothing to correct itself
    /// with. Below the floor the strip is cropped to the native width and the
    /// sides are left empty.
    uint32_t panorama_cover = 0;
    /// What fraction (0-100) of the positions of the ANCHORED strip has a single
    /// hash. It is published because without this number the symptom —100 %
    /// coverage with the strip showing a different section of the level— reads
    /// as a defect of the widening, which is where an afternoon was lost. It is
    /// DIAGNOSTIC and not a gate: measured, neither of the two strips in the
    /// corpus reaches 1 % cleanliness (Golden Axe included, which extends
    /// WELL), so ambiguity on its own does not separate the good case from the
    /// bad one.
    uint32_t panorama_clean = 0;
    bool     panorama_valid = false;
    /// The quads of the HD strip for this frame: one SPAN per horizontal run of
    /// contiguous cells that belong to the Panorama, with its UV sub-rect within
    /// the strip. It is deliberately not a single full-screen quad — see the
    /// long note at the emission site (ayther_session.cpp): the per-cell crop IS
    /// the mask, and that is why a foreground element of the OTHER plane is not
    /// erased.
    const AytherSpriteSub* panorama_subs = nullptr;
    uint32_t               panorama_sub_count = 0;
    /// Plane of the strip ANCHORED this frame (0=A · 1=B): the renderer draws
    /// the quads INLINE in the pass of that plane (correct z — the strip
    /// replaces cells of ITS layer, it does not cover what goes in front).
    uint8_t                panorama_plane = 0;
    /// Q2.6 tint of the quad (stride 3, the same E1 channel as the sprites): the
    /// PER-CHANNEL ratio of the live CRAM against the reference of the
    /// definition, with the lines weighted by what they contributed at
    /// definition time. It follows fades and also COLOUR CHANGES — a sunset
    /// turning orange tints the strip, it does not merely darken it — and it can
    /// brighten (>64), for sunrises and flashes. When the reference has no
    /// chromatic signal it falls back to the luma scalar. null with no strip.
    const uint8_t*         panorama_sub_tint = nullptr;

    /// Signature of the layer mask `plane_mask` (bit0=A · bit1=B · bit2=Window)
    /// and, optionally, how many cells compose it.
    uint64_t screen_signature(uint8_t plane_mask,
                              uint32_t* out_cells = nullptr) const noexcept {
        uint64_t s = 0; uint32_t n = 0;
        for (int p = 0; p < 3; ++p)
            if (plane_mask & (1u << p)) { s += screen_plane_sig[p]; n += screen_plane_cells[p]; }
        if (out_cells) *out_cells = n;
        return s;
    }


    // -- CRT / post-process params the Lua script produced this frame ----------
    AytherShaderParams shader_params{};

    // -- Timing: the motor measures its own per-stage cost ---------------------
    double   emu_fps    = 0.0;
    float    tile_ms    = 0.0f;
    float    sprite_ms  = 0.0f;
    float    audio_ms   = 0.0f;
    float    drc_ratio  = 1.0f;   ///< audio dynamic-rate-control ratio (1.0 = neutral)
    uint64_t frame_index = 0;
    /// The core's TIMING fps (fixed per region, ~59.92/49.7) — the basis of the
    /// temporal term of the Acetates (`frame_index * drift / fps`).
    /// NOT to be confused with `emu_fps`, which is the runner's MEASURED
    /// throughput and varies with the machine: using it would break replay
    /// determinism.
    double   fps_timing = 0.0;
};

// ---------------------------------------------------------------------------
// Substitutable subsystems
// ---------------------------------------------------------------------------
//
// The list of what AYTHER can replace, and the unit in which it is switched on
// and off. The ORDER is a contract: the core holds the same list in
// `ayther_subsystem_name()` —the one that travels in the manifest's
// `[systems]`— and a test compares the two name by name. Two parallel lists
// with nothing tying them together drift apart silently, and then somebody
// switches off "music" and loses the interface.
enum class Subsystem : uint8_t {
    Sprites = 0,   ///< loose sprites by hash
    Metasprites,   ///< poses / sets of sprites (CU-AN)
    Tiles,         ///< plane tiles by hash
    Planes,        ///< whole planes (backgrounds, strips)
    Ui,            ///< the game's interface (Characters, character sets)
    Music,         ///< audio on the Music bus
    Sfx,           ///< audio on the Effects bus
    Shaders,       ///< output effects (CRT/LCD)
    Count
};
constexpr uint32_t kSubsystemCount = static_cast<uint32_t>(Subsystem::Count);
constexpr uint32_t subsystem_bit(Subsystem s) {
    return 1u << static_cast<uint32_t>(s);
}

// ---------------------------------------------------------------------------
// Logical audio buses
// ---------------------------------------------------------------------------
//
// The category of a sound is NOT deduced: the motor identifies timbres, and
// nothing in the chip says whether something is music, an effect or a voice. It
// arrives AUTHORED — the "Type" of each Sequence in Mix — or it does not arrive
// at all.
//
// `Unclassified` is not a lazy default but a real state: an old Sequence said
// nothing. What IS a product decision is where anything WITHOUT a Sequence
// lands (a loose per-signature assignment, which today is the most used path):
// it goes to **Effects**, which is what a loose sound is until somebody says
// otherwise. See `bus_of_signature`.
enum class AudioBus : uint8_t {
    Unclassified = 0,
    Music,
    Sfx,
    Voice,
    Count
};
constexpr uint32_t kAudioBusCount = static_cast<uint32_t>(AudioBus::Count);

/// Describes whether the active pack provides a subsystem.
///
/// Three states preserve the distinction between an explicit absence and
/// missing metadata. A frontend can therefore avoid presenting an unknown
/// capability as either supported or unavailable.
enum class SubsystemAvailability : uint8_t {
    Unknown = 0,  ///< No pack is active, or the pack does not declare this subsystem.
    Present,      ///< The active pack explicitly provides this subsystem.
    Absent,       ///< The active pack explicitly declares this subsystem absent.
};

// ---------------------------------------------------------------------------
// AytherSession
// ---------------------------------------------------------------------------
/// @brief Single-owner facade for deterministic engine orchestration.
///
/// A session owns the emulator host, Rust core handles, optional pack state,
/// scripting, rewind, recording, and audio runtime. Create instances through
/// create(); construction is transactional and returns a typed error.
///
/// @par Thread safety
/// Not thread-safe. Drive the complete lifecycle and every mutating operation
/// from one owning thread.
///
/// @par Borrowed results
/// Views returned by frame-producing operations remain valid only until the
/// next operation that advances, resets, rewinds, reloads, or destroys the
/// session. Copy any data that must cross that boundary.
class AytherSession {
public:
    // -- Configuration passed to create() --------------------------------------
    struct Config {
        std::string core_path;             ///< libretro core DLL (the emulator)
        std::string rom_path;              ///< the ROM — BYOR (Bring Your Own ROM)
        std::string pack_path;             ///< optional .ay HD pack ("" = none)
        /// Production trust registry (TOML) that vouches for `pack_path`.
        ///
        /// Empty keeps the authoring path, which accepts the development
        /// signature -- and which an optimized build refuses. So a release
        /// build with no registry can open NO pack at all: unsigned is refused
        /// and the development key is refused. A frontend shipping production
        /// content has to name its registry here.
        std::string trust_registry;
        bool        enable_audio = true;   ///< open the SDL audio device + AudioPlayer
        /// When pack_path is empty, derive "<core stem>.ay" next to the core
        /// (player convention). The Lab disables this: a project session must
        /// not pick up stray dev packs sitting next to the core.
        bool        derive_core_pack = true;
        /// Libretro core options as `(key, value)` pairs. They are applied once,
        /// before the core reads its configuration during initialization.
        ///
        /// Changing an option requires a new session. A live setter would imply
        /// behavior the underlying core cannot provide safely.
        ///
        /// Available keys are core-specific. Discover them through
        /// core_options_declared() instead of maintaining a frontend list.
        std::vector<std::pair<std::string, std::string>> core_options;
        /// Optional user-supplied IPS/BPS patch. The patch is applied to the ROM
        /// buffer in memory and never modifies the source file. Empty means no
        /// patch.
        ///
        /// Patch failure aborts session creation; silently starting unpatched
        /// would violate the caller's requested configuration.
        std::string patch_path;
    };

    // -- Lifecycle (no-throw) --------------------------------------------------
    // Opens the core + ROM, creates every motor handle (runner, hashers,
    // substitutors, script, optional pack + audio) wrapped in unique_handle.
    // On failure returns an Error (bad core/ROM/pack) — never throws.
    static Result<std::unique_ptr<AytherSession>> create(const Config& cfg);

    ~AytherSession();                                  ///< RAII frees every handle
    AytherSession(const AytherSession&)            = delete;
    AytherSession& operator=(const AytherSession&) = delete;
    AytherSession(AytherSession&&) noexcept;
    AytherSession& operator=(AytherSession&&) noexcept;

    /// Returns the options declared by the loaded core as `(key,
    /// "Description; a|b|c")` pairs. An empty result means the core declared
    /// none; it does not establish a universal lack of option support.
    std::vector<std::pair<std::string, std::string>> core_options_declared() const;

    // -- Content: the HD pack (hot-reloadable) ---------------------------------
    Result<void> set_pack(const std::string& pack_path);  ///< load / replace the pack
    Result<void> reload_pack();                           ///< re-open the current path (hot-reload)
    bool         has_pack() const noexcept;

    // -- Input: per port, libretro JOYPAD button bitmask -----------------------
    void set_input(int port, uint16_t buttons) noexcept;
    void set_input(int port, engine::InputState input) noexcept {
        set_input(port, input.bits());
    }

    // -- Frame stepping --------------------------------------------------------
    // Advance exactly one emulation frame and run the full deterministic
    // pipeline: run_frame -> hash tiles/sprites/audio -> Lua on_frame -> apply
    // overrides -> resolve substitutions -> HD audio out. Returns this frame's
    // data boundary (see FrameView lifetime note).
    const FrameView& step();

    // -- Determinism: savestate round-trip (the foundation of .arp recordings) -
    size_t       serialize_size() const;
    Result<void> serialize(std::vector<uint8_t>& out) const;
    Result<void> unserialize(const std::vector<uint8_t>& in);
    void         reset();

    // -- Rewind + fast-forward (R6) --------------------------------------------
    // enable_rewind allocates a zstd-compressed ring of `seconds` of savestates
    // (captured each step() while enabled; zero cost when off). rewind_step()
    // walks the emulation back one frame (false when the buffer can't go back).
    void   enable_rewind(bool on, int seconds = 10);
    // Rewind one frame: restores the previous state and re-renders it, returning
    // that frame's view (null when the buffer can't go back — disabled / empty).
    const FrameView* rewind_step();
    bool   rewind_enabled()      const noexcept;
    size_t rewind_frames()       const noexcept;   ///< states currently buffered
    size_t rewind_memory_bytes() const noexcept;   ///< compressed buffer size
    // Fast-forward multiplier (frontend reads speed() and steps N times/frame).
    void   set_speed(float mult) noexcept;
    float  speed() const noexcept;
    /// Sets the global device-output mute. This is a no-op when the session was
    /// created without audio. The setting is session-local.
    void   set_audio_muted(bool m) noexcept;
    bool   audio_muted() const noexcept;
    /// Replaces the persistent set of individually muted audio identities.
    /// Passing an empty set restores all identities. This policy is independent
    /// of global output mute and replacement-audio suppression.
    void   set_audio_mute_hashes(const uint64_t* hashes, uint32_t n) noexcept;

    /// "In context" preview of a game audio (Layers panel): it locates, through
    /// the `.arp` (v7), the first frame where that recorded `hash` played,
    /// re-simulates a short stretch and captures the MIX of that moment, playing
    /// it as a one-shot — WITHOUT moving the playhead (it serialises/restores
    /// state + cursor) and WITHOUT touching the mute. It does NOT isolate the
    /// sound: replay audio is not byte-reproducible (the FM phase diverges after
    /// a save/load), so filtering by hash is no use; the real mix of that frame
    /// is played (as when scrubbing there). The capture needs no device
    /// (testable headless); it only plays if the session opened audio.
    /// Returns the stereo frames captured (0 = the hash does not appear / no
    /// v7).
    size_t preview_audio(const AytherRecording& rec, uint64_t hash);
    /// Audio preview AT A GIVEN FRAME (the playhead): it captures the MIX of
    /// that moment and plays it as a one-shot, WITHOUT moving the playhead.
    /// Unlike `preview_audio(rec, hash)`, it does NOT search by hash — the
    /// frontend is already stopped at the sound's frame (Layers panel) and the
    /// REPLAY's audio hashes do not match the recorded ones (the FM phase
    /// diverges after the load), so locating by recorded hash does not work from
    /// the UI. `frame` is clamped to the take. Returns the stereo frames
    /// captured (0 = invalid frame / no v7).
    size_t preview_audio_at(const AytherRecording& rec, uint32_t frame);
    /// AUDIBLE preview of the ORIGINAL audio of a span [start, end] (one-shot) —
    /// the play button of the Sequence timeline: it re-simulates the span and
    /// plays its mix. Isolation: `member_sigs` (the Sequence's signatures) =
    /// PER EVENT (dynamic: each frame only the channels with an active member
    /// event play — an unrelated hit sharing a channel no longer leaks through);
    /// `solo_mask` = the per-channel fallback; 0/null = the complete mix. Capped
    /// at 60 s. It does not move the playhead. It only plays with an open
    /// device.
    /// `foreign_rec` (2026-08-22): the rec is NOT the analysed take (the
    /// original ▶ in the Sequence header, from ANOTHER take). It forces the
    /// re-simulation of THAT rec with PER-CHANNEL isolation — the mirror and the
    /// per-event isolation come from the analysis of the loaded take and would
    /// lie about a different one.
    void   preview_audio_span(const AytherRecording& rec, uint32_t start, uint32_t end,
                              uint32_t solo_mask = 0,
                              const std::vector<uint64_t>* member_sigs = nullptr,
                              bool foreign_rec = false);
    /// TRANSPORT state (the app sets it per frame): the assigned HD sounds
    /// (per-event/Sequence substitution) only TRIGGER while playing — scrubbing
    /// while paused marks without playing. The transition to paused CUTS the
    /// WHOLE gameplay: the HD sounds in flight, the frame staging and the
    /// original/router/SF2 PCM already queued in the continuous streams (the DRC
    /// cushion used to keep playing after the button). Explicit authoring
    /// previews are not cut. A non-sequential seek (scrub) also cuts the HD
    /// sounds. History: without the cut, the one-shot at normal speed overlapped
    /// (echo) and kept playing with the playhead stopped (2026-07-23).
    void   set_transport_playing(bool playing) noexcept;
    /// AUDIBLE audio output (the app sets it per frame): with false, produce
    /// DISCARDS the emulator PCM instead of sending it to the device and does
    /// not trigger subs/HD — the INTERNAL produces of loading a take/pose
    /// (seeks, invalidates, regenerations) used to "squeal" a frame of audio
    /// (report 2026-07-24). The app turns it on only while PLAYING or with the
    /// user SCRUBBING a timeline. Default true (the runtime/Play do not touch
    /// it); independent of the global mute (device gain) and of replay_quiet.
    void   set_audio_audible(bool on) noexcept;
    /// MP4 EXPORT audio: writes the mix of [start, end) to a S16/44.1k WAV.
    /// `hd=false`: the pure ORIGINAL chip mix (the Original version of the
    /// video).
    /// `hd=true`: the original with the dynamic playback MUTES applied per frame
    /// (substituted Sequences + per-signature assignments + disabled occurrences
    /// + manual) and the current HD assets (audio_seq_subs +
    /// audio_event_assign) MIXED on top at each anchor with their gain — "as it
    /// sounds in Mix" (the audio_sub_preview gate is forced during the capture).
    /// It requires a prior analyze_audio_events for hd. The cut on absence
    /// (~1 s) is NOT replicated: it is a heuristic of the LIVE detector; the
    /// replay/pack model lets the whole window play. Hard cap ~15 min.
    /// It does not move the playhead. Blocking (it re-simulates the range
    /// once).
    bool export_mixdown_wav(const AytherRecording& rec, uint32_t start,
                            uint32_t end, const char* wav_path, bool hd);
    /// Exports to WAV (S16 stereo 44100) the emulator audio of the window
    /// [start_frame, end_frame] (+ `tail` frames of tail for SFX that decay;
    /// 0 = exactly the span) — the timing REFERENCE for the artist to make the
    /// HD version (handoff, Audio workspace C-A4). Re-simulated without moving
    /// the playhead; the replay audio is not byte-exact but serves as a guide.
    /// Isolation as in preview_audio_span: `member_sigs` = PER EVENT (Sequences
    /// — only the member events play even when another sound shares a channel);
    /// `solo_mask` = the per-channel fallback; 0/null = the complete mix.
    /// Returns true if it wrote the file.
    bool   export_audio_event_wav(const AytherRecording& rec, uint32_t start_frame,
                                  uint32_t end_frame, const char* wav_path,
                                  uint32_t tail = 20, uint32_t solo_mask = 0,
                                  const std::vector<uint64_t>* member_sigs = nullptr);
    /// Mix (the Channel Detail dialog): re-simulates [start, start+win) with
    /// ONLY the channels of `solo_mask` audible (bits 0-5 FM · 6-9 PSG; the rest
    /// are muted with the fork's per-channel mute during the window) and copies
    /// the captured S16 stereo 44100 PCM into `out`. Returns the stereo frames
    /// (0 = invalid). It does not move the playhead and does not play; the
    /// capture is capped at ~10 s.
    size_t capture_channel_pcm(const AytherRecording& rec, uint32_t start,
                               uint32_t win, uint32_t solo_mask,
                               std::vector<int16_t>& out);
    /// Like capture_channel_pcm but isolating PER EVENT instead of per channel:
    /// each frame it lets only the channels where one of the `member_sigs` is
    /// active play. It copies the S16 stereo 44100 PCM into `out`; returns the
    /// frames.
    ///
    /// It exists for the A/B of the SoundFont Library: comparing a preset
    /// against "the original" requires the original to be THE SAME NOTES.
    /// Isolating per channel also dragged in everything sharing that channel —on
    /// the Mega Drive, the whole music— so a scattered timbre was compared
    /// against a complete passage and it looked as if the preset was not
    /// playing.
    size_t capture_events_pcm(const AytherRecording& rec, uint32_t start,
                              uint32_t win,
                              const std::vector<uint64_t>& member_sigs,
                              std::vector<int16_t>& out);
    /// Exports to WAV the audio of [start, end] (+ `tail` frames of tail) with
    /// ONLY the channels of `solo_mask` audible — the per-sound sample of the
    /// DAW package (Mix). Re-simulated without moving the playhead; capped at
    /// 600 frames (~10 s). Returns true if it wrote the file.
    bool   export_channel_wav(const AytherRecording& rec, uint32_t start,
                              uint32_t end, uint32_t solo_mask, uint32_t tail,
                              const char* wav_path);
    /// Plays raw S16 stereo 44100 PCM (one-shot) — the Channel Detail dialog
    /// plays the already-captured PCM without re-simulating again. No-op with no
    /// open audio device.
    void   play_pcm(const int16_t* pcm, size_t stereo_frames);
    void   stop_audio_preview();
    /// True while the audio preview is still playing (the dialog button toggles
    /// Play/Stop from this and returns to Play when it ends).
    bool   audio_preview_playing() const;

    // -- Audio events from chip commands (C-A2, Audio workspace) ---------------
    // It analyses a take by replaying it frame by frame (a sequential
    // replay_seek = produce every frame) and feeding the AudioEventDetector with
    // the FM/PSG write log (FrameView.chip_writes). It produces per-channel
    // activity blocks with a stable SIGNATURE (the same SFX = the same
    // signature), replay-deterministic (they do not depend on the PCM).
    // Recording-centric like the rest of the Lab. The analysis is silent (it
    // does not play) and leaves the replay cursor at the end of the take — the
    // caller re-seeks its playhead afterwards. It requires a core with the
    // instrumented log (id 0x109); with a stock core it returns 0. Returns the
    // number of events.
    uint32_t analyze_audio_events(const AytherRecording& rec);
    /// Events from the last analyze_audio_events (valid until the next analysis
    /// or clear). The pointers are stable as long as nothing is re-analysed.
    const AytherAudioEvent* audio_events() const noexcept;
    uint32_t                audio_event_count() const noexcept;
    void                    clear_audio_events() noexcept;

    // -- Per-event audio substitution (C-A3b) ---------------------------------
    // It assigns an HD asset to an event SIGNATURE: ALL events with that
    // signature are substituted — their channels are muted during their window
    // [start,end] and playback fires the asset
    // (FrameView.audio_active_subs). The mute is applied by the motor (id 0x10D,
    // replay-safe). It only takes effect with the PREVIEW active, while playing
    // the take analysed with analyze_audio_events. The logic of *which* channel
    // to mute is deterministic and verifiable headless; firing the loose HD
    // audio (decoding OGG/FLAC) is done by the Lab's playback layer.
    void     assign_audio_event(uint64_t signature, const char* asset_path);
    void     unassign_audio_event(uint64_t signature);
    void     clear_audio_event_assignments() noexcept;
    uint32_t audio_event_assignment_count() const noexcept;
    /// The HD asset assigned to an event signature ("" if there is none). So
    /// the panel can show the current assignment of the selected event.
    std::string audio_event_asset(uint64_t signature) const;
    /// One per-event audio substitution: signature → asset + the channels
    /// involved. duration/looping ≠ 0 = a SEQUENCE entry (Mix): a window
    /// relative to the trigger with range-mute + HD (see audio_events.toml).
    struct AudioEventSub {
        uint64_t    signature;
        std::string asset;
        uint32_t    channels;
        uint32_t    duration_frames = 0;
        bool        looping         = false;
        /// Frames the HD may continue after end_frame (0 = exact cut).
        /// UINT32_MAX = unlimited/unauthored (legacy: the non-loop drains in
        /// full) — the pack writer only writes it when it is finite.
        uint32_t    tail_frames     = UINT32_MAX;
        /// FADE-OUT frames after end_frame (0 = no fade; the tail policy
        /// governs). An alternative to `tail`, not cumulative.
        uint32_t    fade_frames     = 0;
        /// F3: match rule + timbre identity (the pack writer only writes them
        /// with a rule other than exact — legacy stays byte-identical).
        AudioMatchRule match_rule       = AudioMatchRule::kExact;
        uint64_t       match_instrument = 0;
        uint8_t        match_pitch      = kAudioNoPitch;
    };
    /// The list of assignments (signature, asset, channels) — for the .ay
    /// delivery. The channels come from the analysed events or, failing that,
    /// from the loaded map.
    std::vector<AudioEventSub> audio_event_subs() const;
    /// F3: the MATCH rule of a per-signature assignment — opt-in, persisted in
    /// audio_events.toml (`match`). kExact deletes the rule (back to legacy).
    /// The timbre identity is taken from the analysed events or from what the
    /// live detector learned; without it the rule cannot be built (and
    /// kInstrumentPitch additionally requires a note) → false and nothing
    /// changes. It reaches ALL the paths that match by exact signature today:
    /// live, replay, bare masks and export. (Authored Sequences carry their rule
    /// in AudioSeqSub itself — set_audio_sequence_subs.)
    bool set_audio_event_match_rule(uint64_t signature, AudioMatchRule rule);
    /// The current rule of an assignment + the persisted identity (kExact if
    /// there is none).
    AudioMatchRule audio_event_match_rule(uint64_t signature,
                                          uint64_t* instrument = nullptr,
                                          uint8_t* pitch = nullptr) const noexcept;
    /// F3: the TIMBRE identity of a signature — from the analysed events (Mix)
    /// or from what the live detector learned (Capture). false = still unknown
    /// (play the passage / analyse the take first).
    /// With this the Lab builds rules for Sequences (whose trigger is not a
    /// per-signature assignment).
    bool audio_signature_identity(uint64_t signature, uint64_t* instrument,
                                  uint8_t* pitch) const noexcept;
    /// Enables/disables per-event substitution during playback (the Lab turns it
    /// on in the Audio workspace). Off by default → normal playback.
    void     set_audio_substitution_preview(bool on) noexcept;

    // -- LIVE audio substitution (runtime, C-A4 step 3) -----------------------
    // It enables per-event substitution in real time (Ayther Play / live game,
    // NOT replay): each frame feeds a detector with the chip writes, and the
    // channels whose signature is assigned are muted + fire their HD. There is
    // an inherent 1-frame lag (the writes only exist after run_frame). The
    // assignments come from the pack's audio_events.toml (loaded with
    // set_pack).
    void     set_audio_runtime_substitution(bool on) noexcept;
    /// Phase 3: Assets BYPASS in a live workspace — the detector and the
    /// bookkeeping (windows/instances/edges) keep running but the mask drops to
    /// 0 (the original game plays) and no HD fires. Coming back from a bypass
    /// re-enters by the same path as resuming from a pause: the emulated clock
    /// offset, without waiting for a new key-on. Different from
    /// set_audio_runtime_substitution(false), which is the EXPLICIT shutdown
    /// (a workspace change) and discards every instance.
    void     set_audio_live_bypass(bool bypass) noexcept;
    /// Audio channels keyed on RIGHT NOW (only with runtime substitution
    /// active). For tooling / a future live visualisation. Returns the number
    /// written into `out`.
    uint32_t audio_live_active(AytherAudioActive* out, uint32_t cap) const;
    /// MANUAL per-channel mute (the Audio workspace timeline): a u16 mask
    /// (bits 0-5 FM, 6-9 PSG) applied ALWAYS in produce (ORed with the
    /// substitution mute). For auditing individual channels. 0 = nothing muted.
    void     set_audio_manual_mute(uint32_t mask) noexcept;
    /// DYNAMIC per-INSTRUMENT mute (the Sounds panel, Mix): unlike
    /// set_audio_manual_mute (per channel, unconditional for the WHOLE frame),
    /// this mute only applies on the frames where an audio_events event
    /// (analyze_audio_events) whose `instrument` is in the set FALLS — so an
    /// instrument that rotates channels does not mute UNRELATED sounds using
    /// that channel in other stretches.
    /// SILENCING IS SILENCING: the mute also reaches what REPLACES that sound —
    /// the HD asset (by signature or by Sequence) and the SoundFont timbre.
    /// Until 2026-07-28 it was "orthogonal to substitution" and it looked as
    /// though the speaker icon switched off the original and left the
    /// replacement playing, which is exactly the opposite of what the artist
    /// asks for.
    /// nullptr/0 = clear.
    /// The window extends ~15 frames (RELEASE TAIL): the key-off does not
    /// silence the FM, it puts it into release, and closing the mute there
    /// uncovered that tail as a click. The tail yields to an unrelated event on
    /// the same channel; the window of its own does not. Measured in
    /// tools/mute_silence_probe.
    void     set_audio_instrument_mute(const uint64_t* instruments, uint32_t n) noexcept;
    /// Telemetry of the mute over SUBSTITUTION: HD triggers that did not play
    /// because they were silenced, and streams that had to be CUT because the
    /// artist silenced while the asset was already playing. Both look identical
    /// from outside ("the replacement is not heard") but the second is the one
    /// that was missing: without the cut, silencing halfway through a long asset
    /// does nothing until the next repetition. Optional pointers. Oracle:
    /// tools/mute_replacement_probe.
    void     audio_mute_stats(uint64_t* hd_muted, uint64_t* hd_cut) const noexcept;
    /// Anchors (window starts) of the Sequence sub `key` in the analysed take —
    /// the joint table with claiming. Diagnostic.
    std::vector<uint32_t> audio_seq_anchors(uint64_t key);
    /// Telemetry of the transactional FALLBACK: occurrences where the ORIGINAL
    /// played because the assigned HD could not (a missing/broken asset or a
    /// failed start), and SDL start failures with the asset already ready. The
    /// observable of the rule "assigned ≠ playable": it grows and the game is
    /// heard = the fallback works; it grows in silence = there is a mute outside
    /// the handshake.
    void     audio_fallback_stats(uint64_t* fallbacks,
                                  uint64_t* start_fails) const noexcept;
    /// Telemetry of the live RESUME: streams re-armed with an offset after a
    /// pause or an Assets bypass, expired instances discarded (they were not
    /// restarted from scratch), and accumulated offset frames — the observable
    /// of "resuming continues, it does not restart".
    void     audio_resume_stats(uint64_t* resumed, uint64_t* finished,
                                uint64_t* offset_frames) const noexcept;
    /// Phase 0 telemetry: active/started mixer voices and the accumulated/
    /// maximum placement lateness in samples (a sustained 0 = the phase is
    /// exact; growing = some path is scheduling against an already-flushed
    /// block).
    void     audio_unified_stats(uint64_t* voices, uint64_t* started,
                                 uint64_t* skew, uint64_t* max_skew) const noexcept;
    /// F4: classification of the live match, per frame-occurrence. `exact` = the
    /// active signature is assigned (or belongs to a Sequence); `rule` = resolved
    /// by a match rule (F3 — fragmentation COVERED by the rule); `variant` = no
    /// match but the SAME instrument as an assigned one (the fragmented
    /// signature of the issue: "that sound" is authored and the original plays
    /// anyway); `unmatched` = a sound outside the authoring.
    void     audio_live_match_stats(uint64_t* exact, uint64_t* rule,
                                    uint64_t* variant,
                                    uint64_t* unmatched) const noexcept;
    /// Clears the counters and the live-match log (fine measurement windows —
    /// the bounded log filled up with the notes of the previous passage). What
    /// was LEARNED (signature→instrument) is preserved.
    void     audio_live_match_reset() noexcept;
    /// A (bounded) log of distinct active signatures WITHOUT a match, with their
    /// history — the data that turns "sounds leak through in the transition"
    /// into a list of concrete signatures with instrument and channel.
    struct AudioLiveUnmatched {
        uint64_t signature     = 0;
        uint64_t instrument    = 0;   ///< 0 = still unknown
        uint64_t first_frame   = 0;
        uint64_t frames_active = 0;
        uint8_t  chip = 0, channel = 0;
        bool     variant = false;     ///< the same instrument as an assigned one
    };
    size_t   audio_live_unmatched(AudioLiveUnmatched* out, size_t cap) const;
    /// Why an HD asset is not ready ("missing"/"empty"/"unsupported"/"corrupt")
    /// or nullptr if it is ready / was never attempted. For the per-assignment
    /// state in the Lab. It triggers no IO.
    const char* audio_asset_error(const char* asset_path) const;
    /// Live SFX streams = the one-shot HD sounds playing RIGHT NOW (the trigger
    /// of a substituted Sequence, in replay or live). It is the observable of
    /// "did the HD actually fire?" without listening: 0 with the gate closed,
    /// >0 while it plays. audio_health (MCP) consumes it — a silent substitution
    /// is diagnosed by looking at this number, not at the mixdown.
    size_t   audio_sfx_count() const noexcept;
    /// SF2 synthesiser telemetry: synthesis frames advanced, cuts from a frame
    /// JUMP (a seek or a catch-up switch off every note in flight), note_on/
    /// note_off emitted, notes not fired because the timbre was silenced, and
    /// frames without emulator PCM (the synthesiser falls back to an estimated
    /// length). "It sounds degraded" looks identical from outside whichever of
    /// those it comes from, and each one is fixed differently. Optional
    /// pointers.
    void     synth_stats(uint64_t* ticks, uint64_t* jumps, uint64_t* note_on,
                         uint64_t* note_off, uint64_t* muted,
                         uint64_t* no_pcm) const noexcept;

    // -- SOUNDFONT RE-SYNTHESIS ----------------------------------------------
    /// One timbre → preset assignment. `patch` is the detector's `instrument`:
    /// the hash of the FM patch without frequency, without channel and WITHOUT
    /// VOLUME — a TIMBRE identity. Measured: 30 timbres cover 60 s of music, so
    /// a whole soundtrack is 10-30 assignments, not thousands of events.
    struct InstrumentAssign {
        uint64_t    patch;
        const char* soundfont;   ///< basename; the pack carries it trimmed
        uint16_t    bank, preset;
        int8_t      transpose;
        float       gain;
    };

    /// Replaces the assignment catalogue. The SoundFonts are loaded from the
    /// pack by basename; the missing ones are ignored (that timbre plays with
    /// its chip, which is the right degradation).
    void set_instrument_assigns(const InstrumentAssign* a, uint32_t n);

    /// Cuts the notes in flight and empties what is queued. The motor already
    /// does it on frame jumps; this is for the cuts the frontend knows about and
    /// the motor does not (closing a take, changing project).
    void synth_panic() noexcept;

    // -- PER-VOICE CHANNEL ROUTER --------------------------------------------
    /// It inverts the substitution model: instead of covering the chip with a
    /// mask derived from event windows, the chip is left MUTE and everything
    /// heard is produced by a 10-voice router, where each voice takes its
    /// channel from the chip's own key-on until the end of its tail.
    ///
    /// That makes structural what used to be tuning: there is no window left to
    /// get wrong, so the gaps between notes and the seam between Sequences
    /// cannot happen.
    ///
    /// ON BY DEFAULT since 2026-07-28. It sat behind a switch while the old path
    /// was the production one; the switch was removed once it was clear the
    /// subtractive model never fully gets fixed — its two known leaks were not
    /// corrected: they stopped being possible.
    ///
    /// Measured cost (tools/fm_resynth_spike, Golden Axe): envelope correlation
    /// 0.9906 and 0.343 ms of the 16.7 per frame. At sample level it does NOT
    /// null out — two different YM2612 emulations never null out — so the
    /// verdict was made by ear.
    ///
    /// AYTHER_VOICE_ROUTER=0 restores the whole old path without recompiling:
    /// the emergency exit while the router is new.
    /// Phase 0: asks for WIDESCREEN. `logical_w` in emulator pixels
    /// (0 = off, the default). The native frame is centred and the extended
    /// area is left on both sides, filled by the Panorama strip.
    ///
    /// The width comes from `widescreen_target_width()`: 398 with square pixels
    /// over 224 px, 427 preserving the displayed 4:3. What the pack default
    /// should be is still open (see docs/WIDESCREEN.md#open-decisions).
    void set_widescreen(uint32_t logical_w) noexcept;
    uint32_t widescreen() const noexcept;

    /// EM-8.2: the widescreen gate, from the text of `widescreen.toml`. The pack
    /// declares HOW FAR to widen and UNDER WHICH CONDITION; the first rule that
    /// holds wins.
    ///
    /// The gate is not a refinement: the extended area comes from the strip, and
    /// the strip only exists where the game HAS TRAVELLED. Measured with
    /// `widescreen_spike`: on a still take the drawable run is 0 on all four
    /// sides. In a menu, widening does not show the level — it shows the void.
    /// That is why the gate is mandatory for widening to be useful.
    ///
    /// Empty text, or text without `[[widescreen]]`, DISARMS the gate and hands
    /// control back to `set_widescreen()` — it does not switch widening off. The
    /// difference matters: already-baked packs declare nothing.
    void set_widescreen_gate(const std::string& toml);
    /// Whether the pack is currently expressing an opinion about the width.
    bool widescreen_gated() const noexcept;
    /// The EFFECTIVE width of the last produced frame: the gate's if it had an
    /// opinion, otherwise the requested one. It is what travels in
    /// `FrameView.wide_w`.
    uint32_t widescreen_effective() const noexcept;

    void set_voice_router(bool on) noexcept;
    bool voice_router() const noexcept;

    /// Router telemetry: router frames advanced, chip frames consumed (with
    /// catch-up there are several per tick), primings after a seek, and blocks
    /// where the resampler had insufficient input. That last one is the pacing
    /// detector that was missing through ten root causes. Optional pointers.
    /// `substituted`: key-ons that did NOT fall back to copy — i.e. voices the
    /// router silenced or replaced. It is the only way to SEE from outside that
    /// a mute reached the router: with the chip mute, audio_mute_mask is always
    /// 0x3FF and says nothing.
    void voice_router_stats(uint64_t* ticks, uint64_t* chip_frames,
                            uint64_t* primes, uint64_t* starved,
                            uint64_t* substituted = nullptr) const noexcept;
    /// Dynamic mute by exact OCCURRENCE (keys chip<<56|channel<<48|start, the
    /// ones from the Lab's audio_event_key): the channel is silenced during the
    /// window of each marked occurrence. Sequences DISABLED with the eye icon —
    /// neither their HD nor their ORIGINAL sound must be heard; other
    /// appearances of the same sound keep playing. nullptr/0 = clear.
    /// The HD is silenced by the motor: the app additionally excluding the sub
    /// is defence in depth, not the mechanism — the mute holds equally for an
    /// asset replacing that occurrence from ANOTHER Sequence or assignment.
    void     set_audio_occurrence_mute(const uint64_t* keys, uint32_t n) noexcept;

    // -- SEQUENCE substitution (a group of events → 1 HD, Audio workspace) ----
    // A Sequence groups several channels under a single HD, triggered by the
    // TRIGGER signature (trigger_signature — the event of the Sequence with the
    // lowest start_frame, tie-broken by chip/channel) with a window RELATIVE to
    // each occurrence of that signature (duration_frames = the authored span of
    // the group or asset_frames, whichever is larger) — the SAME model
    // pack_bake.cpp uses for the exported pack (audio_events.toml →
    // core/src/audio_event.rs), so the Lab preview matches what the real game
    // does: it retriggers on EVERY repetition of the trigger signature, not only
    // the first (2026-07-22, reported bug: with a fixed [start,end] range per
    // Sequence only the 1st occurrence played).
    // It only takes effect with the substitution preview active and an open
    // audio device.
    struct AudioSeqSub {
        uint64_t    trigger_signature = 0;   ///< signature that opens the window at each occurrence
        uint32_t    duration_frames   = 1;   ///< window RELATIVE to the start of the trigger
        /// Span of the Sequence's EVENTS (without the length of the HD): the
        /// SEGMENTATION step of the trigger occurrences. A music loop repeating
        /// every `span` with a longer HD re-anchors on EVERY pass (the HD
        /// restarts, like the game) — segmenting by duration_frames swallowed
        /// the alternate repetitions ("the 1st and the 3rd play, not the 2nd or
        /// the 4th", report 2026-07-23). 0 = use duration_frames.
        uint32_t    span_frames       = 0;
        float       gain              = 1.0f;  ///< HD volume (0..2)
        uint32_t    channel_mask = 0;   ///< 0-5 FM · 6-9 PSG · 10-17 PCM (fallback if signatures is empty)
        uint64_t    key          = 0;   ///< Sequence id (one-shot dedup)
        std::string asset;              ///< HD path (empty = no substitution)
        /// Signatures of the MEMBER events: inside the window ONLY the active
        /// event whose signature is here is muted (over its own span), not the
        /// whole channel — other sounds sharing the channel keep playing (report
        /// 2026-07-23). Empty = the old behaviour (channel_mask).
        std::vector<uint64_t> signatures;
        /// HEAD — the signatures that start on the SAME frame as the trigger
        /// (the trigger included). With ≥ 2, a majority of the head anchors even
        /// when the trigger is a variant (see audio_seq_anchor.h).
        std::vector<uint64_t> head_signatures;
        bool                  looping = false;   ///< continuation on a tie
        /// End-of-window policy. `tail_frames` = UINT32_MAX means "unauthored"
        /// (legacy: the non-loop drains in full); `fade_frames` > 0 wins over the
        /// tail — they are alternatives.
        uint32_t    tail_frames = UINT32_MAX;
        uint32_t    fade_frames = 0;
        /// F3: the trigger's MATCH rule — with kInstrument, any voice of the
        /// SAME timbre anchors the Sequence (the fragmentation of the
        /// transition: the same SFX with the chip in another state is ANOTHER
        /// signature). The identity arrives AUTHORED (persisted), not inferred.
        AudioMatchRule match_rule       = AudioMatchRule::kExact;
        uint64_t       match_instrument = 0;
        uint8_t        match_pitch      = kAudioNoPitch;
        /// The BUS whatever plays in this Sequence belongs to — the "Type" the
        /// author chose in Mix. It is not deduced (the motor identifies timbres,
        /// not categories), so it arrives authored or it does not arrive.
        /// `AudioBus::Unclassified` = the author did not say.
        AudioBus       bus = AudioBus::Unclassified;
    };
    void     set_audio_sequence_subs(std::vector<AudioSeqSub> subs);
    const std::vector<AudioSeqSub>& audio_seq_subs() const noexcept;   // diagnostic
    /// Plays an audio file (HD) as a one-shot — the ISOLATED preview of a
    /// Sequence (the play button of its timeline). It does not move the playhead
    /// and does not touch the take. It only plays with an open audio device. An
    /// empty path is a no-op.
    void     preview_asset_file(const char* path, float gain = 1.0f);
    /// Duration of an HD disk asset IN game FRAMES (at timing_fps) — to size the
    /// span of the Sequence timeline when the HD is longer than the events. 0 if
    /// it cannot be read/decoded. It needs no audio device.
    uint32_t audio_asset_frames(const char* path) const;

    // -- audio_events.toml persistence (C-A5) ---------------------------------
    // It serialises the signature→asset assignments (+ the channel mask of the
    // detected events, for the runtime) to TOML text; and reloads them. The Lab
    // writes the text into the .ay / project and reads it back on load. The
    // events themselves are not saved (they are re-derivable by re-analysing the
    // take, deterministically).
    std::string audio_events_toml() const;
    void        load_audio_events_toml(const char* text);
    /// Loads the assignments from the current pack's `audio_events.toml`
    /// (runtime). The Lab uses the PROJECT's TOML; the runtime uses the PACK's —
    /// which is why this is explicit (it is not done in set_pack, so the
    /// project's are not trampled).
    void        load_audio_events_from_pack();

    // -- Recording + replay (R7) -----------------------------------------------
    // record_start() snapshots the current state as the take's initial frame and
    // logs each subsequent step()'s port-0 input. take_recording() moves out the
    // finished take (input stream + initial state).
    void            record_start();
    void            record_stop();
    bool            recording()        const noexcept;
    size_t          recorded_frames()  const noexcept;
    AytherRecording take_recording();

    /// Renders `frame` of `rec` re-simulating the minimum: it starts from the
    /// live cursor if it is ahead, from the nearest cached keyframe ≤ frame, or
    /// from the initial state; it runs the rest with a "bare" run_frame and
    /// caches keyframes along the way. Deterministic scrubbing (lab.md §7.3),
    /// now without the O(frame) per click.
    /// `quiet` (a user scrub): the short fast-forward DISCARDS the PCM of the
    /// intermediate frames instead of keeping it — keeping it is only correct
    /// for playback catch-up; in a scrub it queues audio at 1× and playback
    /// drifts away from the playhead (report 2026-07-21). The visible frame
    /// keeps its audio (the scrub blip).
    const FrameView* replay_seek(const AytherRecording& rec, uint32_t frame,
                                 bool quiet = false);

    /// Empties the replay cache (keyframes + cursor). Call it when an
    /// AytherRecording object is reused with DIFFERENT content (loading another
    /// take, a split): the pointer does not change and replay_seek's identity
    /// check does not detect it.
    void            replay_reset();

    /// Invalidates the replay cursor so the NEXT replay_seek re-renders even to
    /// the SAME frame (replay_seek normally early-returns there). For re-applying
    /// changes that only affect the render — e.g. the layer mask — over a paused
    /// frame.
    void            replay_invalidate();
    /// How many RUNTIME keyframes (replay_keys, raw) the playback/seek of the
    /// current take accumulated. With BAKED keyframes in the take it is always 0
    /// by design (R7e: the baked ones cover the range).
    size_t          replay_key_count() const;

    /// Progress of a chunked seek (replay_seek_chunk).
    struct SeekStep {
        const FrameView* view = nullptr;  ///< the target frame (only with done=true)
        bool             done = false;    ///< true once the target is reached
        float            progress = 0.0f; ///< 0..1 while done==false
    };

    /// Frames replay_seek(rec, frame) would re-simulate (the distance to the
    /// best starting point). 0 = immediate. The frontend uses this to choose
    /// between a direct seek and a chunked seek with a loader.
    uint32_t        replay_seek_cost(const AytherRecording& rec, uint32_t frame) const;

    /// Chunked seek: it re-simulates ≤ `budget` frames per call towards `frame`,
    /// so the UI does not freeze on long jumps (a cold take). The caller pumps it
    /// every frame and shows the progress until done. The result is identical to
    /// replay_seek; it only spreads the cost across UI frames.
    SeekStep        replay_seek_chunk(const AytherRecording& rec, uint32_t frame,
                                      uint32_t budget);

    /// R7e migration: bakes keyframes into a take WITHOUT them, chunked
    /// (≤ budget frames per call) so it does not freeze. Pump it until done; when
    /// it finishes, `rec.keyframes` is populated and the caller re-saves the
    /// .arp. A no-op (done) if the take already has keyframes or is shorter than
    /// one interval.
    SeekStep        replay_bake_step(AytherRecording& rec, uint32_t budget);

    /// .arp v8 migration: re-bakes the sprite hash HISTORY of an old take
    /// (rec.hash_algo < kSpriteHashAlgo) with the CURRENT hasher, chunked
    /// (≤ budget frames per call — it produces EVERY frame, more expensive than
    /// the bare warm-up of R7e). Pump it until done; when it finishes
    /// rec.sprite_hashes/hash_offsets are rebuilt, rec.hash_algo is updated, and
    /// if the take had no keyframes those get baked too (the sweep captures them
    /// for free) — the caller re-saves the .arp. Silent (replay_quiet) and with
    /// the Lab's masks/overrides stashed: the history records the COMPLETE game.
    SeekStep        replay_rebake_history_step(AytherRecording& rec, uint32_t budget);

    /// Phase C: splits `rec` into head=[0,frame) and tail=[frame,end). It
    /// re-simulates [0,frame) from initial_state to capture the tail's savestate
    /// (the state PRE-frame `frame` — replay_seek+serialize would leave it
    /// post-frame). It CLOBBERS the live emulator state: the caller must re-seek
    /// afterwards. false if frame==0, frame>=frame_count() or the
    /// (de)serialisation fails.
    bool split_recording(const AytherRecording& rec, uint32_t frame,
                         AytherRecording& head, AytherRecording& tail);

    /// Destructive cut (Trim): `out` = the sub-take [begin, end) rebased to 0.
    /// With begin>0 it re-simulates to capture the initial savestate (the same
    /// post-frame path as split_recording) and CLOBBERS the live emulator state:
    /// the caller must re-seek afterwards. false if the range is invalid or the
    /// (de)serialisation fails.
    bool crop_recording(const AytherRecording& rec, uint32_t begin, uint32_t end,
                        AytherRecording& out);

    // -- Advanced mode (Lab): Work RAM + cheats -------------------------------
    // Passive reads of the core's Work RAM (the 68k's 64KB on the Genesis) for
    // the Memory Explorer; GG/PAR cheats via retro_cheat_set (GPX implements
    // them).
    const uint8_t* work_ram(size_t* size) const;
    void cheat_set(unsigned index, bool enabled, const char* code);
    void cheat_reset();

    /// VDP VRAM (64KB: tiles + tilemaps + SAT) — null if the core does not
    /// expose it (it requires the _vram fork). A passive read (multi-space map).
    const uint8_t* video_ram(size_t* size) const;
    /// VDP CRAM (128 bytes = 64 nine-bit colours as host-endian u16, GPX layout
    /// 0000BBB0GGG0RRR0) — null without the fork. A passive read.
    const uint8_t* color_ram(size_t* size) const;
    /// Content signature of palette line `line` (0-3) over the marked `slots`
    /// (a bitmask; 0xFFFF = the whole line), from the live CRAM — it wraps
    /// ayther_palette_signature (the runtime function). 0 without CRAM.
    uint64_t       palette_signature(uint8_t line, uint16_t slots) const;
    /// The 32 VDP registers (reg[0x20]) — null without the fork. For deriving
    /// the plane name-table bases (tilemap viewer, M9.3).
    const uint8_t* vdp_regs(size_t* size) const;
    /// RAW list of the sprites the VDP parsed this frame (the fork's ids
    /// 0x10B/0x10C): 8-byte entries {yr u16, xr u16, attr u16, w u8, h u8} (raw
    /// SAT values: yr/xr with a +128 offset; attr = tile|flips|pal|pri). The
    /// authoritative source of "what the VDP drew" (robust to mid-frame SAT
    /// rewrites). count = entries. nullptr if the core does not expose it.
    /// Diagnostics/probes.
    const uint8_t* parsed_sprites_raw(uint8_t* count) const;

    /// Resolves an arbitrary export of the LOADED core (e.g. the fork's
    /// ayther_recompose_frame — the recomposition of the frame from the final
    /// VDP state, for the fidelity spike of our own renderer). nullptr if the
    /// core does not export it (stock). The caller casts to the real type.
    void* core_export(const char* name) const;

    // -- E-7: the VDP's native layers, in ONE call ---------------------------
    // Plane B, plane A, the window and the sprites exactly as the VDP draws them
    // —with the frame's raster effects already applied— plus the composite,
    // which is the whole frame and serves to verify the extraction without a
    // second render.
    //
    // WHY THIS IS NOT WHAT THE RENDERER USES. The renderer's layer pipeline
    // recomposes on the GPU PER CELL (R-2/R-3), which is what allows a tile to
    // be substituted by an HD asset. This returns already-rasterised BITMAPS: it
    // neither replaces nor competes with that. It serves what the cell cannot —
    // composing outside the 4:3 window, extracting a clean layer to author it,
    // and being the CPU reference the GPU is validated against.
    //
    // The buffers belong to the session and are reused between frames: the
    // caller does NOT free them and they are valid until the next call. `pitch`
    // is `width` (tightly packed).
    struct Layers {
        const uint16_t* bg_b      = nullptr;   ///< RGB565, width*height
        const uint16_t* bg_a      = nullptr;
        const uint16_t* window    = nullptr;
        const uint16_t* sprites   = nullptr;
        const uint16_t* composite = nullptr;   ///< the whole frame
        uint32_t width = 0, height = 0;
        bool ok() const { return composite != nullptr; }
    };

    /// Extracts the layers of the LAST produced frame. Call it after
    /// step()/seek.
    ///
    /// It returns `ok() == false` quietly when the core cannot: a stock core, no
    /// capability, no subscription, or a graphics mode the recomposition does not
    /// support. The reason is left in `layers_error()`, and it is logged ONCE per
    /// reason — not once per frame, which is the difference between a diagnostic
    /// and flooding the log at 60 Hz.
    ///
    /// TRANSACTIONAL (the same criterion applied to video): either all five
    /// layers are there or none is. A composite is never published without its
    /// layers: suppressing the original for an HD that will then be unable to
    /// draw is exactly the defect that was corrected in the audio.
    Layers recompose_layers();

    /// Why the last call could not proceed, as text ready for the log ("the
    /// game is not in graphics mode 5", "double interlace"…). An empty string if
    /// the last one succeeded or if it was never called.
    const char* layers_error() const noexcept;

    /// R-5: an alias — the definition lives at namespace level (FrameView
    /// publishes the scene and is declared before this class).
    using SceneElement = ayther::SceneElement;

    /// Builds the inventory of the LAST produced frame (it reads the live
    /// FrameView — call it after produce/seek; valid until the next step). It
    /// joins the plane cells (the scroll-aware walk from Phase C) with the
    /// hasher's sprites (+ the pattern from the fork's parsed SAT) and notes
    /// which element an already-resolved HD asset replaces. Returns the total
    /// emitted. Empty without the forked core (with no VRAM there are neither
    /// cells nor a parsed SAT).
    size_t scene_inventory(std::vector<SceneElement>& out) const;
    /// Telemetry of the framebuffer judge of the last `scene_inventory`
    /// (sprites judged · discarded · opaque samples · hits).
    void scene_judge_stats(uint32_t* occs, uint32_t* dropped,
                           uint32_t* opaque, uint32_t* hits) const;

    /// R-4: an alias — the definition is at namespace level (next to
    /// SceneElement, whose hiding identity it shares).
    using HiddenElement = ayther::HiddenElement;

    /// R-4: hides ELEMENTS of the inventory, unified — the session routes by
    /// layer to the two existing per-element channels (3=Sprite → composed
    /// hiding; 0-2 → plane tiles). It applies in the PRODUCE of the same frame
    /// (without the latency of the core's suppression channel) and does not
    /// trample the Lab's eye toggles (it unions, it does not replace).
    /// nullptr/0 = clear. The inventory reflects the state in
    /// SceneElement.hidden. Known limit: within the planes, channel 0x105 hides
    /// the GRAPHIC in A/B/W alike (refining it per plane is possible — the mask
    /// is already per plane — but it is out of scope for R-4).
    void set_hidden_elements(const HiddenElement* els, uint32_t n);

    /// R-6: assigns EFFECTS to inventory elements by (layer, hash) — Q2.6 tint,
    /// opacity, outline. It replaces the complete list (nullptr/0 = clear). The
    /// inventory publishes them in SceneElement.fx_* and the compose applies them
    /// per quad; frames that fall back to the blit do not show them (the same
    /// treatment as visibility — transitions, documented in R-5).
    /// An alias in the class, following the usual pattern:
    using ElementEffect = ayther::ElementEffect;
    void set_element_effects(const ElementEffect* fx, uint32_t n);

    /// runtime_enhancement: marks inventory elements to be ENHANCED in software
    /// (EPX over indices) by (layer, hash). It replaces the COMPLETE list of the
    /// Lab source (nullptr/0 = clear); the pack source (load_pack_into) lives
    /// separately and is unioned in. Its own channel and not ElementEffect: the
    /// `element_effect` MCP tool replaces the entire effects list and would
    /// trample the policy. An element claimed by HD is not enhanced
    /// (SceneElement.fx_enhance = 0: the asset won); the master hd_on gate
    /// switches it off in the "Original" side of the A/B and in the export
    /// without HD.
    using EnhancedElement = ayther::EnhancedElement;
    void set_enhanced_elements(const EnhancedElement* els, uint32_t n);


    /// Mask of visible VDP layers (the A/B/Window/Sprites bits of
    /// AYTHER_LAYER_*). The fork's renderer reads it per line: 0xFF = everything
    /// visible. It allows layers to be isolated in the viewport (authoring in
    /// Edit). No-op with a stock core (no 0x102 write id).
    void set_layer_mask(uint8_t mask);

    /// Dim the NON-sprite layers to 25% in the visible frame (the fork's id
    /// 0x108). With `on`, `produce_frame` emits the non-sprite pixels at 25%
    /// (visual prominence for the sprites — the Animation viewport). Independent
    /// of the layer mask (use 0xFF so the backgrounds are rendered and then
    /// dimmed). No-op with a stock core. Produce-only (the bare re-simulation
    /// runs without the dim).
    void set_layer_dim(bool on);


    /// SAT slots to hide (a bitmask of up to 128 bits = 16 bytes; bit i = slot
    /// i). Applied ONLY to the visible frame (the bare re-simulation runs with
    /// complete sprites → no divergence). For hiding individual sprites by hash
    /// in Edit. `n` is clamped to 16. No-op with a stock core (no 0x103 id).
    void set_sprite_suppress(const uint8_t* bits, size_t n);

    /// COMPOSED per-HASH sprite hiding (Pose): the visible frame A is produced
    /// COMPLETE (stable occurrences/lists, without the effects of the VDP line
    /// budget), a 2nd render B suppresses the slots of those hashes (mapped from
    /// A's occurrences, same frame — no lag), and the published base takes B ONLY
    /// within the rect of each hidden occurrence: what is underneath stays
    /// visible exactly under the hidden sprite and nothing else changes. The
    /// translucent "ghost" is drawn by the frontend on top (a Vulkan layer).
    /// Transient — it is not serialised. nullptr/0 = clear.
    void set_sprite_hidden(const uint64_t* hashes, uint32_t n);

    /// Tile cells to hide (a 512-byte mask = 64x64 cells of 8px, stride 64
    /// columns; bit `ty*64+tx`). The frontend maps the hidden tile hash → cells
    /// of the visible frame (`fv.tile_occs`); the cell is painted with the VDP
    /// backdrop (revealing the background). Applied ONLY to the visible frame
    /// (produce-only) → the bare re-simulation is untouched. `n` is clamped to
    /// 512. No-op with a stock core (no 0x104 id).
    void set_tile_suppress(const uint8_t* bits, size_t n);

    /// PLANE tiles to hide, by hash (those of `fv.plane_tile_occs`). Unlike
    /// `set_tile_suppress` (per screen cell), this hides the tile's GRAPHIC
    /// wherever it appears in its plane, independently of the scroll: the core
    /// (id 0x105) skips those cells in `render_bg_m5/_vs` and reveals the plane
    /// behind. The session maps hash → (plane, pattern, palette) using the
    /// occurrences already seen and builds the mask applied ONLY to the visible
    /// frame (produce-only). Phase 2b of the Layers panel. No-op with a stock
    /// core (no 0x105).
    void set_plane_tile_hidden(const uint64_t* hashes, size_t n);

    /// Decodes a VDP tile pattern (8×8, 4bpp) into BGRA (out = 8*8*4 = 256
    /// bytes, stride 8 px) applying the CRAM palette + flips. For the preview of
    /// a plane tile in the Layers panel. No-op (it leaves `out` untouched) if the
    /// core does not expose VRAM/CRAM. `pattern` 0..2047, `pal` 0..3.
    void decode_plane_tile(uint16_t pattern, uint8_t pal, bool hflip, bool vflip,
                           uint8_t* out_bgra) const;

    /// The RGBA variant for the background EXPORT: like decode_plane_tile but in
    /// RGBA order and with colour 0 TRANSPARENT (alpha 0) — the VDP semantics
    /// (index 0 shows the plane behind / the backdrop), which the per-layer PNG
    /// needs so plane A does not cover B on re-import.
    void decode_plane_tile_rgba(uint16_t pattern, uint8_t pal, bool hflip, bool vflip,
                                uint8_t* out_rgba) const;

    /// The 68k view of Work RAM. On little-endian hosts GPX stores the array
    /// word-swapped (READ_BYTE uses addr^1) — verified empirically against
    /// Sonic 2 ($FFFE24/25 = the timer's seconds/frames; the maper_probe spike).
    /// These accessors return the bytes AS THE 68k SEES THEM, so the Lab's
    /// addresses match the documented ones (RetroAchievements, Data Crystal).
    /// u16/u32 in the bus's big-endian. Out of range → 0.
    uint8_t  ram_u8 (uint32_t off) const noexcept;
    uint16_t ram_u16(uint32_t off) const noexcept;
    uint32_t ram_u32(uint32_t off) const noexcept;

    /// Poke (Mapper M5): writes `len` bytes into the 68k view starting at `off`
    /// and marks the session DIRTY — a write outside the input stream breaks
    /// replay determinism, so REC is blocked until a clean state is reached again
    /// (unserialising a marker, or a reset, both of which clear the flag).
    /// false: no RAM or out of range.
    bool poke(uint32_t off, const uint8_t* data, size_t len);
    bool dirty() const noexcept;          ///< altered by a poke
    void clear_dirty() noexcept;

    // -- Z80 RAM -------------------------------------------------------------
    //
    // The 8 KB the 68k sees at 0xA00000-0xA01FFF. Several games leave the id of
    // the track to play there, and that is why a tool that wants to trigger a
    // sound by id needs to be able to look at it — when the slot is not in work
    // RAM, there was nowhere to search.
    //
    // Empty with a stock core: the region belongs to the fork (ABI 1.9), and
    // saying "there is none" is different from saying "it is all zeros".
    const uint8_t* z80_ram() const noexcept;
    size_t         z80_ram_size() const noexcept;
    /// Writes into the Z80 RAM. `false` = no region or out of range.
    ///
    /// IT IS A RACE with the Z80 running, and that is why it dirties the session
    /// just like `poke`: what is written may last a frame. To leave a sound id
    /// where the Z80 reads it that is enough; for anything else, it is not.
    bool z80_poke(uint32_t off, const uint8_t* data, size_t len);

    // -- PLAYER cheats (EM-7.3) ----------------------------------------------
    //
    // Not to be confused with the Mapper's poke, which is an authoring tool: the
    // modder knows which address they are touching and why; the player has a
    // nine-letter string they copied from somewhere.
    //
    // THEY ARE RE-APPLIED PER FRAME, and that is what makes them work: the game
    // rewrites those addresses all the time. Writing once is enough for what the
    // game never touches again; for everything else one has to insist.
    //
    // The session is left DIRTY, as with any poke: a write outside the input
    // stream breaks replay determinism. Playing with cheats and recording a take
    // for authoring do not mix, and the flag says so.

    /// Adds an already-decoded cheat (`ayther_core::cheat_code`). It applies
    /// from the next frame on.
    void add_cheat(uint32_t address, uint16_t value);
    /// Removes them all. What they already wrote is NOT undone: the game keeps
    /// the lives it was given, which is what the player expects.
    void clear_cheats() noexcept;
    uint32_t cheat_count() const noexcept;

    // -- Scripting -------------------------------------------------------------
    Result<void> load_script(const std::string& lua_source, const char* chunk_name);

    // -- Live authoring (Lab) --------------------------------------------------
    // Assign an HD asset to a sprite hash. The mapping persists across frames
    // (re-applied after Lua overrides) so the artist sees it live in the
    // viewport — provided the asset is resolvable by the renderer (i.e. present
    // in the loaded pack). Loose on-disk files are recorded for the eventual
    // pack build but do not render until packed.
    /// `ref_rgb` (optional): the E1 chromatic reference captured at assignment
    /// time (the RGB average of the sprite's CRAM line, 3 bytes) — the per-channel
    /// tint follows the palette's COLOUR changes. null = no reference → grey
    /// peak-hold.
    void assign_sprite(uint64_t hash, const std::string& asset_path,
                       const uint8_t* ref_rgb = nullptr);
    void unassign_sprite(uint64_t hash);
    void clear_assignments();   ///< clears sprite + tile + audio + plane + Mode 3 kind assignments
    // TRANSIENT pose substitution (the authoring model: every element is a
    // Pose). Each entry: the member hashes + relative offsets (rel_x/rel_y,
    // parallel; empty = legacy per-set matching) + the asset (an authored HD or a
    // snapshot of the original) + the hd flag. With rel, the pose only matches
    // with the members at their EXACT offsets (1:1 bbox, one sub per instance —
    // no stretching). The regions of poses with hd=true additionally clear the
    // loose sprites inside. It is NOT serialised to any .toml. It replaces the
    // complete set; empty = off.
    /// Per-variant asset candidate (step 2): the configuration (palette/flip,
    /// -1 = any) `asset` was authored for. The motor picks the one closest to the
    /// observed variant of the anchor. `slots`/`sig` = identity by palette
    /// CONTENT (a bitmask of the marked slots + the xxh3 signature of the stable
    /// content); sig 0 = no signature.
    struct PoseVariant {
        int8_t palette = -1, hflip = -1, vflip = -1;
        uint16_t slots = 0;
        uint64_t sig   = 0;
        std::string asset;
    };

    struct PosePreview {
        std::vector<uint64_t> hashes;
        std::vector<int16_t>  rel_x, rel_y;   ///< per-member offsets ("" = legacy)
        /// Size in PX of each member (parallel to hashes; empty = unknown): the
        /// off-screen tolerance of an ABSENT member needs ITS real dimensions —
        /// without them the motor approximates them with those of the first
        /// visible member and the match fails when an 8×8 head leaves through an
        /// edge.
        std::vector<int16_t>  dim_w, dim_h;
        /// SAT flips observed per member at capture time (bit0 = hflip ·
        /// bit1 = vflip, parallel to hashes; empty = a legacy pose). The resolver
        /// breaks ties between geometrically ambiguous PARTIAL instances by
        /// agreement (occ.flip == member_flip ^ arrangement).
        std::vector<uint8_t>  mem_flips;
        uint16_t              bbox_w = 0, bbox_h = 0;  ///< capture size in px (the legacy anti-giant guard)
        std::string           asset;
        /// Wardrobe: the tint mask of the BASE asset ("" = no mask). The motor
        /// attaches it to the sub only when the chosen asset IS `asset` (a
        /// variant candidate is an authored recolour and does not carry it).
        std::string           mask;
        bool                  hd = false;     ///< true = authored HD; false = snapshot
        /// The facing `asset` is drawn in relative to the captured one (the
        /// presentation flip in Pose): the motor XORs it onto the detected
        /// mirror.
        bool                  flip_h = false, flip_v = false;
        // Step 2: per-variant candidates. Empty = a single `asset`. With
        // candidates, the motor picks the one closest to the observed variant.
        std::vector<PoseVariant> candidates;
        /// Authored reference of the E1 tint: the RGB average (0-255 per channel)
        /// of the pose's CRAM line when it was captured — "how it looks normally".
        /// The motor tints the HD per channel live/ref (following fades AND colour
        /// flashes). {0,0,0} = no reference → scalar peak-hold (classic E1).
        uint8_t               ref_rgb[3] = { 0, 0, 0 };
        /// E1 reference PER palette LINE (0-3) for poses with MIXED palettes —
        /// the motor emits one quad per line group and each tints against the
        /// reference of ITS line. A line of {0,0,0} = no reference (the anchor's
        /// group falls back to `ref_rgb`; the others to the peak-hold of their
        /// line).
        uint8_t               ref_line[4][3] = {};
    };
    void set_pose_preview(const std::vector<PosePreview>& poses);

    /// LIVE in-betweens (§6.1/6.2): one transition per entry — an empty `from`
    /// = the wildcard "from any pose". The asset strings must match those of the
    /// pose channel (project paths). Apply it only WHEN the pool CHANGES (the
    /// TweenPlayer keeps its overrides between frames, unlike the pose set, which
    /// is re-injected per frame).
    struct TweenPreview {
        std::string from;                 ///< source asset ("" = wildcard)
        std::string target;               ///< destination asset
        std::vector<std::string> frames;  ///< ordered drawings
        uint32_t    ticks = 3;            ///< game frames per drawing
    };
    void set_tween_preview(const std::vector<TweenPreview>& tweens);
    // Look up the current assignment for a hash ("" if none).
    const char* assignment_for(uint64_t hash) const noexcept;
    // Enumerate all (hash, asset_path) assignments, sorted by hash (for the
    // Deliver workspace to serialise into a pack). R8.
    std::vector<std::pair<uint64_t, std::string>> assignments() const;
    /// Like assignments() but with the E1 chromatic reference per entry — for
    /// persisting sprites.toml and baking the [[sub]] blocks with `ref`.
    struct SpriteAssignment { uint64_t hash; std::string asset; uint8_t ref_rgb[3]; };
    std::vector<SpriteAssignment> sprite_assignments() const;

    // Tile + audio HD assignments — same model as sprites (persist across frames,
    // applied after Lua overrides). Enumerated by the Deliver workspace to bake
    // tile_substitutions.toml / audio_substitutions.toml into a pack.
    void assign_tile(uint64_t hash, const std::string& asset_path);
    void unassign_tile(uint64_t hash);
    const char* tile_assignment_for(uint64_t hash) const noexcept;
    std::vector<std::pair<uint64_t, std::string>> tile_assignments() const;

    void assign_audio(uint64_t hash, const std::string& asset_path);
    void unassign_audio(uint64_t hash);
    const char* audio_assignment_for(uint64_t hash) const noexcept;
    std::vector<std::pair<uint64_t, std::string>> audio_assignments() const;

    // Plane-tile HD assignments (Phase 2c) — the same model as tiles, but the
    // identity is the content hash of the plane tile (plane+pattern+palette).
    // The replacement is resolved scroll-aware in produce_frame (an overlay by
    // position). Deliver bakes `plane_tile_substitutions.toml`.
    void assign_plane(uint64_t hash, const std::string& asset_path);
    void unassign_plane(uint64_t hash);
    const char* plane_assignment_for(uint64_t hash) const noexcept;
    std::vector<std::pair<uint64_t, std::string>> plane_assignments() const;

    // -- Plane SETS (Paint Phase C): HD substitution per multi-tile ELEMENT ---
    // A set = a group of plane tiles with relative offsets in CELLS (the Paint
    // Elements catalogue). The matcher runs in the plane scan: for every
    // appearance of the anchor (member[0]) it verifies the rest at their offsets;
    // if ALL are there → ONE overlay of the asset stretched to the bbox (the
    // lo/hi lane according to the VDP priority of the anchor) and the member
    // tiles are suppressed by identity (the same 1-frame latency as channel
    // 0x105).
    struct PlaneSetMember { uint64_t hash; int16_t cx, cy; };
    /// `ref_rgb` (3 bytes, optional): the E1 tint reference — the RGB average
    /// 0-255 of the element's CRAM line "as it looks normally" (captured when it
    /// was created, the same contract as the poses' `ref`). With a reference, the
    /// set's quad is tinted live/ref per channel and follows the palette fades;
    /// nullptr/{0,0,0} = no tint (the previous behaviour).
    void define_plane_set(uint64_t id, uint8_t plane, uint16_t w_cells,
                          uint16_t h_cells, const PlaneSetMember* members,
                          uint32_t member_count, const std::string& asset_path,
                          const uint8_t* ref_rgb = nullptr);
    void undefine_plane_set(uint64_t id);
    void clear_plane_sets();

    // -- ANIMATION: a SEQUENCE of plane sets with its own clock ---------------
    //
    // WHAT IT ADDS OVER PLANE SETS. A set already substitutes by hash: graphic A
    // → asset A. That is enough when the game already animates and each phase has
    // its own hash. Animation is for what THAT cannot do: letting the HD have
    // MORE phases than the original (the game alternates A·B and the artist wants
    // a cycle of six drawings), or run at a different cadence. That is why it is
    // a PLAYER WITH ITS OWN CLOCK and not a follower of the content — a follower
    // could not show a step that is not on screen.
    //
    // HOW IT IS WIRED. It does not duplicate the matcher: when ANY step of the
    // sequence matches, that position is taken over by the Animation and the
    // asset drawn there is that of the step CURRENT BY CLOCK, not of the one
    // that matched. The framing (the quad's w/h) is still that of the set that
    // matched — the steps of a flicker share a footprint; if they do not, the art
    // of each step has to be composed over that same footprint.
    struct PlaneSequenceStep {
        uint64_t    set_id   = 0;    ///< the Object (plane set) it references
        const char* asset    = nullptr;  ///< HD of THIS step (nullptr = the set's)
        uint16_t    duration = 0;    ///< game frames it is held (0 = default)
    };
    /// Declares an Animation. Fewer than two steps is rejected: one step is just
    /// the set on its own, with nothing to cycle.
    void define_plane_sequence(uint64_t id, const PlaneSequenceStep* steps,
                               uint32_t step_count);
    void undefine_plane_sequence(uint64_t id);
    void clear_plane_sequences();

    /// The frontend's HD mode (default ON). It gates the set matcher: since that
    /// suppresses the original tiles, with HD off it would leave holes. The flag
    /// is read PER PRODUCE, so it also holds for the compose's `bare` re-render
    /// and for export_frame. The Lab and the runtime write it from their own
    /// Original↔HD toggle.
    void set_hd_enabled(bool on) noexcept;
    bool hd_enabled() const noexcept;

    // -- PER-SUBSYSTEM original/HD routing ------------------------------------
    //
    // `hd_enabled` is the light switch for the whole house; these are the
    // switches for each room. It is the single point where it is decided which
    // substitution applies, and it exists for what the original/AYTHER comparison
    // and the profiles need: switching off the HD sprites while keeping the
    // music, or the other way round, without restarting anything.
    //
    // It is read PER PRODUCE, like `hd_enabled`: the change is visible on the
    // next frame and the return to the original is immediate — there is no state
    // to rebuild, because the original never went away (the originals keep being
    // drawn; what is switched off is the replacement).
    //
    // Default: ALL on. A session without a frontend managing them behaves as it
    // always has.
    void set_subsystem_enabled(Subsystem s, bool on) noexcept;
    bool subsystem_enabled(Subsystem s) const noexcept;

    /// The complete state as a mask (bit i = `Subsystem(i)` on), so it can be
    /// serialised as session configuration and restored exactly.
    uint32_t subsystems_enabled_mask() const noexcept;
    void     set_subsystems_enabled_mask(uint32_t mask) noexcept;

    /// Does the loaded pack carry this subsystem? See `SubsystemAvailability`:
    /// the third state (Unknown) is what avoids asserting "it does not carry it"
    /// about a pack that simply does not declare it.
    SubsystemAvailability subsystem_availability(Subsystem s) const noexcept;

    // -- Safe degradation on pack errors --------------------------------------
    //
    // The fallback already prevents a broken asset from cutting the session
    // short: the original is heard and that is that. What is missing here is
    // ESCALATION — a pack with many broken assets retries each one, every frame,
    // and pays for the full resolution of something already known not to work.
    //
    // DISTINCT ASSETS are counted, not occurrences: one broken file that plays a
    // thousand times is one problem; twelve distinct files is a badly assembled
    // pack.

    /// Subsystems the MOTOR switched off by itself after repeated failures
    /// (bit i = `Subsystem(i)`). Deliberately separate from
    /// `subsystems_enabled_mask`: "the user switched it off" has to be
    /// distinguishable from "it switched off after failures", because only the
    /// second gives the user something to be told.
    uint32_t auto_disabled_subsystems() const noexcept;

    /// What has to be said to the user, already written. "" = nothing to say.
    ///
    /// The Engine composes it and not the frontend because whoever knows WHAT
    /// happened is whoever counted it; the frontend decides WHERE and WHEN to
    /// show it. It names the pack: with the assets named by hash, without that
    /// there is no way back to the project that baked it.
    std::string degradation_message() const;

    /// Try again: it reactivates what switched itself off and forgets the
    /// failures. The user asks for it (the panel) — the motor does not retry on
    /// its own, because trying again what already failed twelve times is exactly
    /// what the escalation exists to avoid.
    void clear_auto_disabled() noexcept;

    // -- Remastering profiles -------------------------------------------------
    //
    // A profile is a NAMED PRESET of what the routing and the buses already let
    // you toggle: which subsystems get substituted and which buses play. It does
    // not multiply the material — it filters what the pack already carries.
    // Without that, a pack with four profiles would weigh four times as much,
    // which is the "combinatorial complexity of assets per profile" the issue
    // notes as a risk.
    //
    // The list comes from the pack and always carries "original" first.

    /// How many profiles the loaded pack offers. 0 with no pack.
    uint32_t profile_count() const noexcept;
    /// Id / name of profile `i`. An empty string when out of range.
    std::string profile_id(uint32_t i) const;
    std::string profile_name(uint32_t i) const;

    /// Applies profile `id`: it sets the subsystem mask and the bus mutes in one
    /// go. `false` = that profile does not exist in this pack, and then NOTHING
    /// is touched — applying "the closest thing" would leave the user looking at
    /// something they did not ask for with nothing saying so.
    bool set_profile(const std::string& id);

    /// The ACTIVE profile, or "" if the current state is none of them.
    ///
    /// Empty is a legitimate result and it is half the contract: as soon as
    /// somebody touches an individual toggle, the state stops being the one the
    /// profile describes. Continuing to say "enhanced" would lie about what is
    /// being seen — the "custom" profile in the issue's scope is not declared, it
    /// is REACHED, and this is how it is detected.
    std::string active_profile() const;

    /// The profile the pack applies when loaded without another being requested.
    /// `set_pack` calls it by itself; it is exposed so a frontend can offer
    /// "back to the default".
    bool apply_default_profile();

    // -- Pack validation ------------------------------------------------------
    //
    // Can this pack run with THIS session? It is answered BEFORE loading it and
    // without opening it, so a pack from another game or in a newer format
    // cannot take anything down: it never gets opened.
    //
    // It returns a list and not a yes/no because there are two different things:
    // a critical incompatibility (another game, an Engine that does not exist
    // yet) and an optional degradation (a subsystem this build does not know).
    // With a boolean, either the second is rejected or the first is accepted.
    struct PackFinding {
        bool        error = false;   ///< false = warning
        std::string code;            ///< stable, so decisions need no prose
        std::string message;         ///< for the user
    };
    /// Context taken from the session (loaded ROM, platform, core) plus whatever
    /// the caller wants to specify. What is unknown is reported as unverified
    /// instead of being assumed fine.
    std::vector<PackFinding> validate_pack(const std::string& pack_path) const;

    // -- Level analysis of an audio asset -------------------------------------
    //
    // What an author needs BEFORE publishing: whether the asset clips, whether it
    // will get lost under the game, and how much it would need correcting. It is
    // measured over the PCM that goes to the mix (not over the container's bytes:
    // a 22 kHz mono OGG sounds different from what its header says) and the file
    // is NEVER touched — in AYTHER every correction is playback gain.
    //
    // Cached by path and invalidated by the hot-reload, so the panel can ask for
    // it per frame.
    using AssetLevel = AudioAssetLevel;   // audio_asset_level.h
    const AssetLevel& audio_asset_level(const std::string& abs_path) const;

    /// The asset ENVELOPE — `bins` interleaved (min, max) pairs in -1..1, for
    /// drawing the waveform. Empty = it could not be decoded.
    ///
    /// Min and max rather than a single absolute value: drawn only from the
    /// maximum, a waveform does not show asymmetry, and asymmetry is where DC
    /// offset and one-sided clipping become visible.
    const std::vector<float>& audio_asset_waveform(const std::string& abs_path,
                                                   uint32_t bins) const;

    // -- Logical audio buses --------------------------------------------------
    //
    // Volume and silence PER CATEGORY. A sound's bus comes from the "Type" of its
    // Sequence; a loose per-signature assignment lands in **Effects**.
    //
    // The difference from the per-subsystem routing, which is easy to confuse:
    //
    //   · switching off the Music SUBSYSTEM = "do not substitute the music" →
    //     the game's ORIGINAL music plays;
    //   · muting the Music BUS              = "I do not want music" → neither the
    //     HD nor the original plays.
    //
    // They are different intentions and that is why they are different
    // controls.
    //
    // The volume scales the HD. Turning the ORIGINAL partly down requires a
    // source with gain in the voice router (which today only knows how to copy or
    // mute), so for now the original responds to the bus MUTE and not to the
    // volume — it is written here so nobody discovers it by listening.
    void  set_bus_volume(AudioBus bus, float gain) noexcept;
    float bus_volume(AudioBus bus) const noexcept;
    void  set_bus_muted(AudioBus bus, bool muted) noexcept;
    bool  bus_muted(AudioBus bus) const noexcept;

    // -- PICTURE (CU001): a complete static screen → one HD asset -------------
    // A cell of the Picture is its ABSOLUTE position in the screen grid (8 px
    // column/row, like `screen_plane_sig`) plus the tile hash: a Picture does not
    // scroll, so the position IS part of the identity.
    struct ScreenCell { uint64_t hash; uint8_t plane; uint8_t col, row; };

    /// Declares a Picture. Recognition is by COVERAGE, not by equality:
    ///   `min_match` — the MINIMUM fraction of the declared cells that has to be
    ///                 present at its exact position (0.92 is reasonable).
    ///                 Without tolerance, a single animated cell —a flickering
    ///                 flame, a blinking "PRESS START"— would drop the whole
    ///                 Picture.
    ///   `max_extra` — cells of the frame (within the mask) that are NOT in the
    ///                 Picture, as a fraction of the declared ones. It is the
    ///                 indispensable guard: without it, a screen that is a
    ///                 SUPERSET (a menu drawn over the title) would match the
    ///                 title with perfect coverage.
    /// The asset is drawn FULL SCREEN in its own lane, below the plane overlays
    /// — so a Prop or a glyph authored over that same screen is still seen on
    /// top. Since it is opaque and covers everything, nothing needs to be
    /// suppressed.
    void define_screen(uint64_t id, uint8_t plane_mask,
                       const ScreenCell* cells, uint32_t cell_count,
                       float min_match, float max_extra,
                       const std::string& asset_path);
    void undefine_screen(uint64_t id);
    void clear_screens();

    // -- KINEMATIC (CU004): an ordered sequence of Pictures --------------------
    // What it adds over individual Pictures is NOT the drawing —the Picture
    // already does that— but the ORDER: it disambiguates two identical screens
    // appearing in different cutscenes, and it provides the cancellation
    // semantics of the spec (if the player presses Start and the game jumps to a
    // menu, the sequence is cut and the new screen is re-evaluated, with no
    // waiting).
    //
    // It can be ENTERED at any step, not only the first: the position comes from
    // the CONTENT of the screen, so a scrub into the middle of the cutscene lands
    // where it should instead of having to play from the beginning.
    //
    // `gap_frames` is the tolerance of frames without a confirmed Picture before
    // cancelling. It CANNOT be 0: `screen_match_id` drops to 0 for one frame on
    // EVERY clean Picture transition (the hysteresis requires two frames to
    // confirm the next one), and for several more if there is a fade in between.
    /// One step: the Picture expected, and optionally the asset that replaces it
    /// when the sequence goes through here (empty = use the Picture's own).
    /// `video_offset`: which frame of the clip THIS step starts at, when the
    /// asset is an `.ivf`. It is what lets one video cover several steps and lets
    /// entering mid-sequence land on the right shot — the position comes from the
    /// CONTENT (which step matched) plus this offset, not from a counter. Ignored
    /// if the asset is not a video.
    struct KinematicStep {
        uint64_t    screen_id;
        const char* asset;
        uint32_t    video_offset = 0;
    };
    /// Settings for the Kinematic's MEDIA — everything that is not the sequence
    /// of steps. They travel together in a struct and not as loose parameters
    /// because the media will keep growing (volume, ducking…) and each new field
    /// would be another broken signature in the three callers.
    struct KinematicMedia {
        /// The video REPEATS if it is shorter than the stretch, instead of
        /// holding its last frame. It is an authoring decision, not a default: a
        /// background clip (rain, fire) wants to loop and a narrated scene does
        /// not — rewinding it halfway would be a defect. That is why the motor
        /// holds unless told otherwise.
        bool        loop  = false;
        /// The video's AUDIO track, as a separate asset (IVF is video only).
        /// nullptr/"" = the Kinematic plays with the game audio.
        const char* audio = nullptr;
        /// Volume of the Kinematic's track (1 = original).
        float       gain = 1.0f;
        /// Volume of the game's SOUNDTRACK WHILE the Kinematic runs
        /// (1 = untouched, 0 = mute). It is a ducking with its own lifetime: it
        /// is applied on entry and returned to 1 on exit. Without this, a
        /// narrated scene played over the game music.
        float       game_gain = 1.0f;
    };
    void define_kinematic(uint64_t id, const KinematicStep* steps, uint32_t step_count,
                          uint32_t gap_frames = 12,
                          const KinematicMedia* media = nullptr);
    void undefine_kinematic(uint64_t id);
    void clear_kinematics();

    // -- PANORAMA (CU003): the level strip of one layer → an HD texture -------
    // A cell of the strip is its position in LEVEL space (8 px cells, the same
    // space the stitcher reconstructs) plus the tile hash.
    struct PanoramaCell { uint64_t hash; int32_t lx, ly; };

    /// Declares a Panorama: the reconstructed strip of ONE layer.
    ///
    /// ANCHORING is by CONTENT, not by the accumulated camera (`plane_cam_*`):
    /// that one is *relative* — it re-anchors at 0 on any seek/scrub, and in the
    /// Lab the artist scrubs all the time, so the panorama would end up at the
    /// wrong offset until playing forward. Instead, every visible cell whose hash
    /// is in the strip says where the camera is (`cam_px = lx*8 - screen_x`); the
    /// mode of those votes fixes it exactly, and that holds equally after a seek,
    /// a savestate load or a scene cut.
    ///
    /// Only the RARE hashes of the strip vote (≤ `kPanoramaRare` appearances):
    /// the same idea as the least frequent anchor of the set matcher — the sky
    /// tile that appears 500 times contributes no information and does cost. The
    /// rarity is computed here, at declaration time, not per frame.
    void define_panorama(uint64_t id, uint8_t plane,
                         int32_t origin_x, int32_t origin_y,
                         uint16_t w_cells, uint16_t h_cells,
                         const PanoramaCell* cells, uint32_t cell_count,
                         const std::string& asset_path);
    void undefine_panorama(uint64_t id);
    void clear_panoramas();
    /// Maximum appearances in the strip for a hash to serve as an anchor.
    static constexpr uint32_t kPanoramaRare = 8;
    /// The MINIMUM fraction of the voters that has to back the winner for the
    /// camera to be declared anchored. A rare hash may repeat in another section
    /// of the level, so a thin majority is a dubious anchor: without this floor
    /// it was published as valid anyway and the renderer drew the strip in the
    /// wrong place. Below it, `panorama_valid` stays false — "I do not know where
    /// I am" is a useful answer; an invented position is not.
    static constexpr uint32_t kPanoramaMinVotePct = 50;

    // -- C-S2 animations (Components): HD playback in phase --------------------
    // An "Action" (clip = anim_group_id) is drawn in HD synchronised to the game:
    // for the pose the game shows this frame, the HD frame of the sheet at the
    // metasprite's bbox. Level 0 (Pop) or Level 1 (a geometric tween of the
    // transform between keyframes — a glide instead of a pop, with no extra
    // art).
    /// Defines (or replaces) the HD animation of a clip: sheet + pose→frame map +
    /// tween level. The Animations tab (C-S3) authors it; Deliver bakes
    /// `animations.toml`. Per-frame results in FrameView.anim_frames.
    void   define_animation(uint64_t clip_id, const std::string& sheet_asset,
                            const HdPose* poses, uint32_t pose_count, int tween_level);
    void   undefine_animation(uint64_t clip_id);
    void   clear_animations();
    size_t animation_count() const noexcept;
    /// The current definitions (authored + loaded from the pack), sorted by
    /// clip_id — Deliver bakes them into `animations.toml` (C-S4).
    std::vector<AnimationDef> animation_definitions() const;

    // -- C-A2 audio (Components): per-EVENT HD substitution --------------------
    // A sound with an attack and a tail (a jingle/voice/music) spans many batches
    // with changing hashes → it is substituted as a whole EVENT: a range-mute of
    // the emulator over [start,end] + the HD asset aligned to start_frame. The
    // event frames are frames OF THE TAKE → they apply during its replay; live
    // they do not match (a no-op). C-A1 limit: a DIFFERENT sound overlapping in
    // the mix is not separated (batch hashes are opaque).
    /// Runs the event detector over the take's audio history (.arp v7, CSR per
    /// frame) and resolves the substitution windows with the current
    /// assignments. Call it when loading/changing the take. Returns the number of
    /// events detected (it also leaves them in audio_events()).
    size_t resolve_audio_events(const AytherRecording& rec);
    /// Events detected by the last resolve_audio_events (so the Lab can
    /// list/assign). Cleared on the next resolve.
    const AytherAudioEvent* audio_events(size_t* count) const noexcept;
    /// Assigns an HD asset (WAV/OGG/FLAC from the pack) to an event SIGNATURE;
    /// "" unassigns. It persists across takes; it re-resolves the windows
    /// immediately.
    void assign_audio_event(uint64_t signature, const std::string& asset, bool looping);
    std::vector<AudioEventAssignment> audio_event_assignments() const;
    /// Resolved windows (assigned event → range + asset), for the timeline UI.
    /// Valid until the next resolve/assign.
    const AytherAudioEventSub* audio_event_subs(size_t* count) const noexcept;

    // -- Backgrounds (Components): plane stitcher + per-layer export -----------
    // While capture is ON, produce_frame accumulates the visible cells of planes
    // A/B in LEVEL SPACE (BackgroundStitcher + unwrapping of the VDP scroll — the
    // path validated by tools/background_spike). The capture expects SEQUENTIAL
    // frames (a linear pass over a take, or a live game): a scrub jump breaks the
    // unwrapping. Turning it on again starts a fresh capture.
    void   bg_capture(bool on);
    bool   bg_capturing() const noexcept;
    /// Level cells accumulated for the plane (0=A · 1=B). 0 without capture.
    size_t bg_cell_count(uint8_t plane) const noexcept;
    /// Exports each captured plane that has cells to a per-layer PNG
    /// (`bg_<A|B|W>_t<ox>x<oy>_<hash>.png`, the re-import index in the name) into
    /// `out_dir`. Returns the paths written ({} if nothing was captured).
    Result<std::vector<std::string>> export_backgrounds(const std::string& out_dir);
    /// Exports ONLY plane `plane` (0=A · 1=B · 2=W) of the stitcher to `path`
    /// (an exact PNG file; the caller chooses name and folder). An error if there
    /// is no active capture, the plane accumulated no cells, or the write fails —
    /// unlike export_backgrounds, here failure is NOT silent.
    Result<void> export_background_plane(uint8_t plane, const std::string& path);
    /// Stitcher bounds for `plane` in level CELLS: out = {min_x, min_y, max_x,
    /// max_y}. false without capture or with an empty plane. The definition of a
    /// Panorama MUST use these bounds (those of the exported PNG) — the extents
    /// of bg_cells() may differ by a row/column (partial edge cells) and that
    /// offset stretches and shifts the asset when drawing.
    bool bg_bounds(uint8_t plane, int32_t out[4]) const;
    /// The plane's accumulated cells as PANORAMA CELLS: (hash, lx, ly), ready for
    /// `define_panorama`. Different from what the stitcher exports to PNG: those
    /// are nametable codes (for redrawing the strip), this is the CONTENT
    /// identity by which the Panorama is recognised at runtime. Stable order (by
    /// row, then column) so the baked TOML is reproducible between sweeps.
    std::vector<PanoramaCell> bg_cells(uint8_t plane) const;

    // -- Mode 3 (RAM anchoring) ------------------------------------------------
    // PER-INSTANCE metasprite substitution: the TOML profile declares where the
    // entities live in work RAM (game_profile.rs); each frame produce_frame reads
    // their world_pos, projects them with the VDP camera (the path validated by
    // tools/mode3_spike) and assigns the SAT sprites to each instance. Results in
    // FrameView.entity_subs / entity_instances.
    /// Loads (or replaces) the anchor profile; "" unloads it (Mode 3 becomes a
    /// no-op).
    Result<void> load_game_profile(const std::string& toml_path);
    bool         has_game_profile() const noexcept;
    /// The HD asset for EVERY instance of the profile's `kind_name` kind (each in
    /// its own bbox). It persists across frames; "" unassigns that kind.
    void         assign_kind(const std::string& kind_name, const std::string& asset_path);
    // -- Animation clips (C-S1) ------------------------------------------------
    // Looping cycles the sprite hasher detected from the SAT slot histories,
    // consolidated into an ordered pose sequence + per-frame duration (the §4
    // "phase"). The Animación workspace reads these as a starting point for
    // authoring. Accumulated state (not per-frame); empty until warm-up.
    struct AnimClipFrame { uint64_t pose; uint16_t duration; };
    struct AnimationClip {
        uint64_t                  id;       // = anim_group_id (stable, matches occs)
        bool                      looping;
        std::vector<AnimClipFrame> frames;  // ordered (canonical: starts at min pose)
    };
    size_t                     animation_clip_count() const noexcept;
    std::vector<AnimationClip> animation_clips() const;
    /// Reset the animation detector (clear history/groups/clips). Call before a clip
    /// generation run so the result reflects only the recording scanned (C-S5).
    void                       reset_animation_detection() noexcept;

    // -- ACETATES the pack carries -------------------------------------------
    // The Custom layers with their own content that the artist composed in the
    // Compose workspace. The pack carries them now (before, they died in the Lab:
    // they were persisted in the project and the bake did not write them).
    //
    // The session READS and OFFERS them; it does not build the stack. The layer
    // stack is FRONTEND state —the Lab owns it, the renderer receives it as a
    // parameter— so deciding when and in what order to build it belongs to
    // whoever draws: Ayther Play builds its own when opening the pack, the Lab
    // already has the project's. Putting the stack in here would move that
    // decision to the motor.
    //
    // `content.asset` is the pack's ENTRY NAME (a content id), not a disk path —
    // it is resolved against the pack like any other asset. Empty if the pack
    // does not carry acetatos.toml (packs older than this feature).
    struct PackOverlay {
        std::string         name;
        bool                visible = true;
        AytherLayerContent  content;
    };
    const std::vector<PackOverlay>& pack_overlays() const noexcept;

    // -- E-2: AYTHER ABI v1 subscriptions -------------------------------------
    // A diagnostic of the contract with the core: what was requested, what ended
    // up ACTIVE and what the binary supports. With a core without the ABI all
    // three are 0 — which is how "there is no ABI" is distinguished from "there
    // is an ABI and it activated nothing", two situations that look the same from
    // outside (nothing is observed) and are fixed differently.
    //
    // `active` only means something AFTER the first frame: the subscriptions take
    // effect at the core's frame boundary.
    void ayther_subscriptions(uint32_t* requested, uint32_t* active,
                              uint32_t* supported) const noexcept;
    /// True if the loaded core negotiated ABI v1 (the gate of the new paths).
    bool has_ayther_abi() const noexcept;

    /// The negotiated ABI (`AYTHER_ABI_VERSION_MAJOR/MINOR`; 0 without the ABI)
    /// and the `build_id` the core declares ("" without the ABI). It is what a
    /// frontend shows so one knows WHAT IT IS RECORDING AGAINST: a take replayed
    /// with another core may diverge, and without this there is no way to say
    /// which one it was.
    uint32_t    ayther_abi_version() const noexcept;
    const char* ayther_build_id()   const noexcept;

    /// `SYSTEM` (ABI 1.5; see docs/EMULATOR_EXTENSION_ABI.md#observation-regions):
    /// what the core says about the content —
    /// VDP mode (4/5, 0 while it has not chosen), h40, interlace, S/H, region and
    /// the viewport of the emitted frame with its offset (Game Gear: 160×144 at
    /// (48,24)). It refreshes per frame; `ok` = the core provides it.
    ///
    /// ABI 1.10: `viewport_w/h`, `interlace` and `h40` describe the EMITTED frame
    /// (h40 can no longer contradict the viewport); `vdp_mode` and
    /// `shadow_highlight` come from the registers. `geometry_pending` = the
    /// registers have already changed the geometry and the next frame applies it:
    /// whoever wants the "final" geometry waits until it clears; whoever draws
    /// the received frame uses the viewport as-is. With a core < 1.10 it is
    /// always false.
    struct SystemInfo {
        bool     ok = false;
        uint8_t  system_hw = 0, region_pal = 0, vdp_mode = 0, interlace = 0;
        uint8_t  h40 = 0, shadow_highlight = 0;
        uint16_t lines_per_frame = 0;
        uint16_t viewport_x = 0, viewport_y = 0, viewport_w = 0, viewport_h = 0;
        bool     geometry_pending = false;
    };
    SystemInfo system_info() const noexcept;

    // -- Read-only introspection (identity / pacing for the frontend) ----------
    const char* game_id()    const noexcept;   ///< pack-reported game id ("" if no pack)
    double      timing_fps() const noexcept;   ///< core timing fps (drives frontend pacing)
    /// Audio telemetry: flush frames with a backlog < 1/4 of the target
    /// (accumulated starvation) and the current DRC ratio. For diagnosing DELIVERY
    /// degradation with HD active (the Lab's audio_health MCP tool).
    uint64_t    audio_starved_frames() const noexcept;
    float       audio_drc_ratio() const noexcept;
    float       audio_backlog_avg() const noexcept;   ///< frames (EMA)

    /// Why it did NOT play. `audio_starved_*` measures DELIVERY —whether the PCM
    /// arrives late— but says nothing when the PCM is not even sent, which is the
    /// case of "nothing is heard". There are four paths to silence and from
    /// outside they look identical: the output is inaudible (an internal produce,
    /// or a paused replay), the motor is in warm/bake, audio is disabled in the
    /// config, or it is simply muted. Counting them separately turns the symptom
    /// into an answer.
    /// `flushed` = frames in which the PCM DID reach the device.
    void audio_gate_counts(uint64_t* flushed, uint64_t* inaudible,
                           uint64_t* quiet, uint64_t* disabled) const noexcept;
    /// Pause telemetry: stereo frames DISCARDED by the transport cuts (staging +
    /// the emulator backlog + synth) and how many cuts discarded something. It
    /// verifies that pausing really does cut — a cut with 0 frames discarded and
    /// audible sound is a stream outside the cut.
    void audio_pause_stats(uint64_t* cut_frames,
                           uint64_t* cuts) const noexcept;
    /// The current audible-output flag (the frontend writes it per frame).
    bool audio_audible() const noexcept;

    // -- Borrowed motor resources the frontend reads (valid while owned) -------
    // The active HD pack. The typed value hides the raw core handle and is
    // invalidated by set_pack()/reload_pack(). An empty view means no pack.
    engine::PackView pack()        const noexcept;
    // Emulator work RAM (read-only) for inspection overlays (e.g. Sonic XY).
    const uint8_t* work_ram()      const noexcept;
    size_t         work_ram_size() const noexcept;
    // Export the tile hasher's catalog to a TOML file (authoring convenience).
    void           dump_tile_catalog(const char* path) const;

private:
    AytherSession();                 ///< use create()
    const FrameView& produce_frame();///< run+build a frame (shared by step/rewind)
    void           replay_capture_key(uint32_t key);  ///< stash a replay keyframe (R7d)
    /// R7e: the best starting point for a seek to `target` (the greatest of: the
    /// initial state, a runtime keyframe, a baked .arp keyframe). It decompresses
    /// the baked one into kf_scratch if that wins; it leaves `state` pointing at
    /// the raw state. Returns the frame.
    uint32_t       replay_start(const AytherRecording& rec, uint32_t target,
                                const std::vector<uint8_t>*& state);
    /// The starting frame only (without decompressing) — for replay_seek_cost.
    uint32_t       replay_start_frame(const AytherRecording& rec, uint32_t target) const;
    /// Captures the MIX of a short window starting at frame `f` (re-simulated
    /// from the best keyframe) and plays it as a one-shot WITHOUT moving the
    /// playhead. The shared helper of preview_audio (which locates `f` by recorded
    /// hash) and preview_audio_at (which uses the playhead directly). Returns the
    /// stereo frames captured.
    size_t         capture_audio_window(const AytherRecording& rec, uint32_t f);
    /// The capture core: re-simulates `win` frames from `f` accumulating the MIX
    /// in cap_pcm, restoring the playhead. It plays a one-shot if `play`. Shared
    /// by capture_audio_window (preview) and export_audio_event_wav (handoff).
    /// `mute_mask` (optional): channels silenced DURING the captured window
    /// (bits 0-5 FM · 6-9 PSG) — capture_channel_pcm isolates a channel with it.
    /// `max_samples` (optional): the cap of the captured buffer (0 = the ~10 s
    /// default; capture_channel_pcm raises it to cover the whole take).
    /// `member_sigs` (optional): DYNAMIC per-event isolation — each frame of the
    /// window lets only the channels with an active event of those signatures
    /// play (it requires analyze_audio_events; without an analysis it falls back
    /// to `mute_mask`).
    /// `dynamic_mute`: applies PER FRAME the dynamic playback mask
    /// (dynamic_audio_mute_at — per-event/Sequence subs + instrument + occurrence
    /// + manual) — the "muted original" of the MP4 export mixdown.
    size_t         capture_pcm_span(const AytherRecording& rec, uint32_t f,
                                    uint32_t win, bool play, uint32_t mute_mask = 0,
                                    size_t max_samples = 0,
                                    const std::vector<uint64_t>* member_sigs = nullptr,
                                    bool dynamic_mute = false);
    struct Impl;                     ///< pimpl: owns the unique_handle<>s + runner + audio
    std::unique_ptr<Impl> impl_;
};

}  // namespace ayther
