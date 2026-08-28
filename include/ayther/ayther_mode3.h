#pragma once
// ---------------------------------------------------------------------------
// ayther_mode3.h — Mode 3 (RAM anchoring) resolver: metasprite substitution
// **per entity instance**.
//
// ## What Mode 3 is
//
// Mode 2 (define_metasprite, ayther_session.h) groups SAT sprites by
// anim_group_id and substitutes the whole bounding box with one HD asset. It
// cannot tell two *identical* on-screen entities apart (two of the same enemy,
// Sonic + Tails): their sprites share hashes/anim-groups, so a purely visual
// split guesses. Mode 3 removes the guess by reading each entity's **world
// position** from game RAM — every instance has a distinct world_pos, so the SAT
// sprites split exactly, and each instance gets its HD asset placed at its own
// bbox.
//
// ## How it plugs in
//
//   AytherSession::produce_frame(), once per frame:
//     1. reads the foreground plane camera scroll from the VDP (already computed
//        for the Fase 2c plane resolver): Hscroll table (VRAM) + VSRAM.
//     2. calls Mode3Resolver::resolve(work_ram, scroll, plane size, sprite_occs).
//     3. publishes resolver.subs() on the FrameView (a per-instance HD lane), and
//        resolver.instances() for authoring overlays (the Lab draws each anchored
//        entity's box + id).
//
// The resolver owns the Rust game profile (game_profile.rs, via the
// ayther_game_profile_* FFI) and does the geometry through
// ayther_game_profile_assign — the exact path validated against Sonic 2 EHZ in
// tools/mode3_spike (VDP scroll read == RAM camera; Sonic/Tails split by
// world_pos).
//
// ## Integration status
//
// The Rust profile implementation, FFI, engine resolver, session wiring, and
// root CMake target are integrated. The design mirrors the existing
// SpriteSubstitutor/metasprite flow and publishes results through FrameView.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "ayther_core_ffi.h"   // AytherGameProfile, AytherSpriteOccurrence, AytherSpriteSub
#include "ayther_result.h"     // ayther::Result / Error

namespace ayther {

// One anchored entity located on screen this frame: its stable id (the RAM
// object struct address), its anchor kind, and the bbox aggregated from the SAT
// sprites the profile claimed for it. Distinct instances of the same kind get
// distinct ids — the disambiguation Mode 2's clustering cannot produce.
struct EntityInstance {
    uint64_t id           = 0;
    int32_t  kind         = -1;   ///< anchor-kind index (ayther_game_profile_kind_*)
    int16_t  screen_x     = 0;    ///< bbox top-left in screen px (min over claimed sprites)
    int16_t  screen_y     = 0;
    uint16_t w_px         = 0;    ///< bbox width  in screen px
    uint16_t h_px         = 0;    ///< bbox height in screen px
    uint16_t sprite_count = 0;    ///< SAT sprites this instance claimed
};

// ---------------------------------------------------------------------------
// Mode3Resolver — owns the game profile, resolves per-instance substitutions.
// Single-owner, driven from the emulation thread (like AytherSession). Movable,
// non-copyable. All results are borrowed and valid only until the next
// resolve()/load_profile() (same lifetime rule as FrameView).
// ---------------------------------------------------------------------------
class Mode3Resolver {
public:
    Mode3Resolver() noexcept;
    ~Mode3Resolver();
    Mode3Resolver(const Mode3Resolver&)            = delete;
    Mode3Resolver& operator=(const Mode3Resolver&) = delete;
    Mode3Resolver(Mode3Resolver&&) noexcept;
    Mode3Resolver& operator=(Mode3Resolver&&) noexcept;

    // -- Profile: where entities live in RAM ----------------------------------
    /// Load a TOML anchor profile (game_profile.rs format). Replaces any current
    /// profile; "" clears it (Mode 3 becomes a no-op). Error on a bad file.
    Result<void> load_profile(const std::string& toml_path);
    /// Loads a profile from TOML stored inside a pack. An empty string clears
    /// the current profile, matching `load_profile`.
    Result<void> load_profile_str(const std::string& toml);
    bool         has_profile() const noexcept;

    // -- HD assets per anchor kind --------------------------------------------
    /// Draw `asset_path` for every INSTANCE of the anchor kind named `kind_name`
    /// (each at its own bbox). Persists across frames; "" clears that kind.
    void assign_kind(const std::string& kind_name, const std::string& asset_path);
    void clear_assignments() noexcept;

    // -- Per-frame resolve ----------------------------------------------------
    /// Read the entities from `ram` via the profile, assign this frame's SAT
    /// `sprites` to them using the foreground plane camera `scroll_x/y` and size
    /// `plane_w/h` (both read from the VDP by AytherSession), and build the
    /// per-instance bboxes + HD substitutions. No profile → results are cleared.
    void resolve(const uint8_t* ram, size_t ram_size,
                 int32_t scroll_x, int32_t scroll_y, int32_t plane_w, int32_t plane_h,
                 const AytherSpriteOccurrence* sprites, uint32_t sprite_count);

    // -- Results (borrowed; valid until the next resolve()/load_profile()) -----
    const EntityInstance*  instances()      const noexcept;
    uint32_t               instance_count() const noexcept;
    /// One HD quad per instance whose kind has an assigned asset, positioned at
    /// the instance bbox. Reuses AytherSpriteSub → feed straight to the renderer.
    const AytherSpriteSub* subs()           const noexcept;
    uint32_t               sub_count()      const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ayther
