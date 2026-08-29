#pragma once
// ---------------------------------------------------------------------------
// ayther_components_toml.h — TOML round-trip of the Components layer:
// `animations.toml` (C-S4) and `audio_events.toml` (C-A4).
//
// Baking (bake_*) is called by the Lab's Deliver step while building the pack;
// parsing (parse_*) is called by AytherSession when loading a pack
// (load_pack_into), repopulating the AnimationPlayer / the per-event assignment
// mirror. Both sides live in the ENGINE (free functions, no UI and no Vulkan)
// so the round-trip is testable headless with strings.
//
// Formats:
//   animations.toml (engine-owned):
//     [[animation]]
//     clip  = "0x<16hex>"          # authoring handle (clip id)
//     sheet = "sheets/run.png"
//     tween = 1                     # 0 Pop · 1 geometric tween
//     [[animation.pose]]
//     pose   = "0x<16hex>"          # pose hash (stable identity)
//     src    = [x, y, w, h]         # sub-rect of the sheet (px)
//     anchor = [x, y, w, h]         # keyframe dst (Level 1)
//     ticks  = 6
//
//   audio_events.toml — the SAME schema the Rust core parses
//   (AudioSubstitutor::parse_events_toml, loaded by load_from_pack):
//     [[event]]
//     signature = "0x<16hex>"
//     asset     = "audio/music/zone1.ogg"
//     loop      = true              # optional (default false)
//
//   plane_sets.toml — Props (CU002) and Glyphs (CU005): HD substitution per
//   multi-tile plane ELEMENT. Until now the Paint catalogue existed only in the
//   authoring session (injected through the API), so the delivered `.ay` did
//   NOT reproduce any multi-tile substitution; this file closes that gap.
//     [[font]]
//     id = "0x<16hex>" · name = "HUD" · cell_w = 1 · cell_h = 2
//     [[set]]
//     id      = "0x<16hex>"        # pintar_element_id (deterministic per capture)
//     name    = "Chest"            # informational (overlay/debug)
//     type    = "utileria"         # utileria | glifo
//     plane   = 0                   # 0=A · 1=B · 2=Window
//     w_cells = 3 · h_cells = 2     # bbox
//     asset   = "cofre.png"         # basename (the bake routes it to the tier)
//     tiles   = "0x<hash>:cx,cy|…"  # members with a RELATIVE offset in cells
//     font    = "0x<16hex>" · ch = "A"    # type="glifo" only
//
//   The FLIPS observed at capture time are deliberately NOT baked: the plane
//   tile hash is flip-invariant and the matcher does not require them (a
//   mirrored prop matches all the same). They live only in
//   pintar_elements.toml, which uses them for the faithful export of the base
//   PNG.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ayther_animation.h"      // AnimationDef / AnimationPlayer
#include "ayther_audio_events.h"   // AudioEventAssignment / AudioEventSubstitution

namespace ayther {

// -- Props / Glyphs: plane sets in the pack (CU002 · CU005) ------------------

/// Member of a set: plane tile hash + RELATIVE offset in CELLS from the
/// top-left of the set. Mirrors `AytherSession::PlaneSetMember` (no flips: the
/// hash is flip-invariant and the matcher does not require them).
struct PackPlaneSetMember {
    uint64_t hash = 0;
    int16_t  cx = 0, cy = 0;
};

/// A multi-tile element of the Paint catalogue, exactly as it travels in the
/// pack.
struct PackPlaneSet {
    uint64_t    id      = 0;      ///< pintar_element_id (deterministic per capture)
    std::string name;             ///< informational (overlay / debug)
    std::string type;             ///< "utileria" | "glifo"
    uint8_t     plane   = 0;      ///< 0=A · 1=B · 2=Window
    uint16_t    w_cells = 0, h_cells = 0;
    std::string asset;            ///< basename inside the pack
    std::vector<PackPlaneSetMember> members;
    uint64_t    font_id = 0;      ///< glyph only
    std::string ch;               ///< glyph only: UTF-8 code point
    /// HUD RE-ANCHORING offset, in pixels. When the screen widens (16:9) the
    /// game keeps drawing its interface in the coordinates of the original
    /// screen, so a readout pinned to the left edge ends up floating towards
    /// the centre. The author looks, sees which Object fell outside the safe
    /// area, and enters here the values that bring it back in.
    /// (0,0) = untouched, which is the case for every Object that already
    /// lands correctly.
    int16_t     off_x = 0, off_y = 0;
    /// E1 tint reference (RGB 0-255 average of the element's CRAM line at
    /// capture time, the same `ref = "r,g,b"` dialect as the poses).
    /// {0,0,0} = no tint → omitted when baking (byte-identical to before).
    uint8_t     ref_rgb[3] = { 0, 0, 0 };
};

/// A game font: a grouper of glyphs, with no bitmap of its own.
struct PackPlaneFont {
    uint64_t    id = 0;
    std::string name;
    uint16_t    cell_w = 1, cell_h = 1;
};

/// Cap on members per set. It was 64, on the theory that "hundreds of cells =
/// a Picture" — the Golden Axe title logo refuted it: it TRANSLATES (rising
/// 1 px/frame), so a Picture cannot match it and it has to be an Object of 277
/// cells... which the bake used to skip SILENTLY. 512 gives it room; the
/// matcher cost (O(members × appearances), all present) stays bounded because
/// the anchored appearances of a large set are few. What is no longer silent:
/// exceeding it is a lint finding.
inline constexpr size_t kMaxPlaneSetMembers = 512;

// -- Baking (Deliver → pack) -------------------------------------------------
std::string bake_animations_toml(const std::vector<AnimationDef>& defs);
std::string bake_audio_events_toml(const std::vector<AudioEventAssignment>& assigns);
/// Sets with more than `kMaxPlaneSetMembers` members, with no members, or with
/// no asset are SKIPPED (the caller reports it). Fonts with no glyphs are
/// emitted anyway: they are cheap and they serve the charmap.
std::string bake_plane_sets_toml(const std::vector<PackPlaneSet>& sets,
                                 const std::vector<PackPlaneFont>& fonts);

// -- Parsing (pack → session) ------------------------------------------------
/// Defines in `into` every animation of the TOML (define() replaces by
/// clip_id). Returns how many animations were loaded.
size_t parse_animations_toml(const std::string& text, AnimationPlayer& into);
/// Assigns in `into` every event of the TOML (the engine's authoring mirror;
/// the Rust core parses the same file for its catalogue). Returns the count.
size_t parse_audio_events_toml(const std::string& text, AudioEventSubstitution& into);

/// MEMBER signatures per Sequence from audio_events.toml — the `members` of
/// each [[event]] (trigger signature → list of signatures). The runtime uses
/// them for SELECTIVE muting inside the window (only the active events of those
/// signatures, not the whole channel). Old packs without `members` → an empty
/// map, and the runtime falls back to the range-mute of `channels`.
std::unordered_map<uint64_t, std::vector<uint64_t>>
parse_audio_event_members(const std::string& text);
/// List of `field` signatures per [[event]] (`members`, `head`). The HEAD = the
/// signatures that start alongside the trigger: the runtime anchors the
/// Sequence by a majority of the head even when the trigger is a variant (see
/// audio_seq_anchor.h).
std::unordered_map<uint64_t, std::vector<uint64_t>>
parse_audio_event_sig_list(const std::string& text, const char* field);

/// `tail_frames` per [[event]] of audio_events.toml — how many frames the HD
/// may keep playing AFTER end_frame (0 = cuts exactly at the limit). ABSENT =
/// unlimited (legacy packs: the non-loop used to drain in full and changing
/// their sound on migration would be an audible regression) — that is why the
/// map only carries the signatures that declare the field, and the Deliver
/// writer ALWAYS writes it so the contract is explicit on a re-bake.
/// Same transport as `members`: the Rust catalogue parser ignores it.
std::unordered_map<uint64_t, uint32_t>
parse_audio_event_tails(const std::string& text);

/// Gain per signature. Absent = 1.0 (neutral), which is what the mixer applied
/// as a constant before the value travelled with the pack.
std::unordered_map<uint64_t, float>
parse_audio_event_gains(const std::string& text);

/// Loop region per signature, in asset FRAMES. Absent = the whole asset. An
/// invalid range (end <= start) is discarded while parsing.
std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>>
parse_audio_event_loops(const std::string& text);

/// `fade_frames` per [[event]] — how many frames the FADE-OUT lasts after
/// end_frame (absent / 0 = no fade: the tail policy governs). It is an
/// ALTERNATIVE to `tail_frames`, not cumulative: with a fade, the cut does not
/// interrupt the ramp. Same transport as `tail`: only the signatures that
/// declare it enter the map, and the Rust catalogue parser ignores it.
std::unordered_map<uint64_t, uint32_t>
parse_audio_event_fades(const std::string& text);

/// A PICTURE exactly as it travels in the pack: the whole screen of the layers
/// that compose it. The cell carries its ABSOLUTE position in the screen grid
/// (a Picture does not scroll) and the plane it came from.
struct PackScreenCell { uint64_t hash; uint8_t plane, col, row; };
struct PackScreen {
    uint64_t    id = 0;
    std::string name;
    uint8_t     plane_mask = 0x07;
    float       min_match = 0.92f, max_extra = 0.08f;
    std::string asset;
    std::vector<PackScreenCell> cells;
};

/// `screens.toml` — one [[screen]] per Picture. The cells go in a pipe-list
/// `hash:plane,col,row`, the same dialect as the rest of the pack.
std::string bake_screens_toml(const std::vector<PackScreen>& screens);
size_t parse_screens_toml(const std::string& text, std::vector<PackScreen>& out);

/// -- PANORAMA (CU003) --------------------------------------------------------
/// The level-wide strip of one layer. It has its own file and belongs neither
/// in `plane_sets.toml` nor in `screens.toml`, because of SIZE: a Sonic-style
/// level strip is ~36,000 cells (~1 MB), between 10 and 40 times everything
/// else in the catalogue put together. Putting it into the element catalogue
/// would mean renaming a glyph rewrites a megabyte, and opening the project
/// reparses it even when the artist is heading to another workspace.
///
/// `lx`/`ly` are the position in LEVEL SPACE and are SIGNED: the origin of the
/// strip is the minimum observed, so a cell seen before the origin comes out
/// negative. The unsigned `col`/`row` of PackScreenCell is no use here — it
/// would truncate the whole strip.
struct PackPanoramaCell { uint64_t hash; int32_t lx, ly; };
struct PackPanorama {
    uint64_t    id = 0;
    std::string name;
    uint8_t     plane = 0;
    int32_t     origin_x = 0, origin_y = 0;
    uint16_t    w_cells = 0, h_cells = 0;
    std::string asset;
    std::vector<PackPanoramaCell> cells;
};

/// `panoramas.toml` — one [[panorama]] per strip. The cells go in an ARRAY with
/// one entry PER ROW (`ly: lx:hash|lx:hash|…`) and not in a single pipe-list: a
/// million characters on one line breaks git diffs, which is the declared
/// criterion of this project's hand-written writers. The explicit per-cell `lx`
/// lets a row have gaps.
std::string bake_panoramas_toml(const std::vector<PackPanorama>& pans);
size_t parse_panoramas_toml(const std::string& text, std::vector<PackPanorama>& out);

/// -- KINEMATIC (CU004) -------------------------------------------------------
/// An ORDERED sequence of Pictures. Unlike the Panorama, it is a short list of
/// ids: it fits comfortably on one line and needs no dialect of its own. The
/// per-step asset is optional — empty = the Picture's own is used, which is the
/// normal case (what the Kinematic contributes is the ORDER, not another
/// drawing).
struct PackKinematicStep {
    uint64_t    screen_id = 0;
    std::string asset;
    /// Frame of the clip this step starts at, if `asset` is an `.ivf`.
    uint32_t    video_offset = 0;
};
struct PackKinematic {
    uint64_t    id = 0;
    std::string name;
    uint32_t    gap_frames = 12;   ///< frames tolerated without a confirmed Picture
    /// The video LOOPS if it is shorter than the stretch (instead of holding
    /// its last frame). An authoring decision: a background wants to loop, a
    /// narrated scene does not.
    bool        loop = false;
    /// AUDIO track of the video — a separate asset, because IVF is video only.
    /// Empty = the Kinematic plays with the game audio.
    std::string audio;
    float       gain = 1.0f;        ///< volume of the Kinematic track
    float       game_gain = 1.0f;   ///< volume of the game soundtrack WHILE it
                                    ///< runs (ducking, 1 = untouched)
    std::vector<PackKinematicStep> steps;
};

/// `kinematics.toml` — one [[kinematic]] per sequence. The steps go in a
/// pipe-list `0xid` or `0xid:asset`, the same compact dialect as the rest of
/// the pack.
std::string bake_kinematics_toml(const std::vector<PackKinematic>& kins);
size_t parse_kinematics_toml(const std::string& text, std::vector<PackKinematic>& out);

// ---------------------------------------------------------------------------
// instruments.toml — PER-TIMBRE re-synthesis: which chip voice is replaced by
// which preset of which SoundFont. It is an axis COMPLEMENTARY to the Sequence:
// the game keeps playing —its tempo, its cuts— and only the TIMBRE changes, so
// it cannot desynchronise.
//
// The same schema the Lab bakes and the Rust core reads
// (core/src/instrument_map.rs). It lives here so the pack has ONE reader and so
// it is testable without a session.
// ---------------------------------------------------------------------------
struct PackInstrument {
    uint64_t    patch = 0;       ///< game timbre (instrument id)
    std::string soundfont;       ///< basename; the pack carries it trimmed
    uint16_t    bank = 0, preset = 0;
    int8_t      transpose = 0;
    float       gain = 1.0f;
};

/// Parses `instruments.toml`. A timbre WITHOUT a soundfont is discarded: it
/// cannot be re-synthesised and keeping it would populate the catalogue with an
/// entry pointing at nothing. Returns how many remained.
size_t parse_instruments_toml(const std::string& text, std::vector<PackInstrument>& out);

/// ANIMATION: a sequence of plane sets (Objects) with its own clock.
/// It goes in `plane_sequences.toml` and not in `animations.toml`, which
/// already belongs to the ACTIONS (HD poses in phase): they are two different
/// things and sharing a file would mix them. The name ties it to
/// `plane_sets.toml`, which is what it cycles over.
struct PackPlaneSeqStep {
    uint64_t    set_id   = 0;
    std::string asset;         ///< "" = the set's own asset
    uint16_t    duration = 0;  ///< game frames (0 = the motor default)
};
struct PackPlaneSequence {
    uint64_t    id = 0;
    std::string name;
    std::vector<PackPlaneSeqStep> steps;
};
std::string bake_plane_sequences_toml(const std::vector<PackPlaneSequence>& seqs);
size_t parse_plane_sequences_toml(const std::string& text,
                                  std::vector<PackPlaneSequence>& out);

/// The step in effect at `t` frames from the anchor, where `t` has ALREADY been
/// reduced to the cycle (the caller applies the modulo against the sum of the
/// durations). `duration == 0` falls back to `default_dur`. Returns `n-1` if
/// `t` overruns — which cannot happen once the modulo is applied, but a step in
/// effect is better than none.
///
/// It is a FREE function and not a runtime method so the cadence can be tested
/// without an emulator: the Animation clock lives inside produce_frame, and
/// setting up a real case demands core + ROM + a frame that matches.
uint32_t plane_sequence_step_at(const uint16_t* durations, uint32_t n,
                                uint64_t t, uint16_t default_dur);
/// Sum of the durations (cycle length in game frames). 0 if `n == 0`.
uint32_t plane_sequence_total(const uint16_t* durations, uint32_t n,
                              uint16_t default_dur);

/// Reads `plane_sets.toml`. Returns the number of valid sets (with members and
/// an asset); invalid ones are discarded silently, like the rest of the pack
/// parsers. The fonts come out through `out_fonts` (which may be empty).
size_t parse_plane_sets_toml(const std::string& text,
                             std::vector<PackPlaneSet>& out_sets,
                             std::vector<PackPlaneFont>& out_fonts);

/// -- elements.toml -----------------------------------------------------------
///
/// The authorable Paint Identities in ONE document. They used to be five files,
/// and what separated them was not the Identity but the motor MECHANISM that
/// serves them: Picture, Panorama, Kinematic, Animation, Prop, Character and UI
/// are all the same family to the artist. Here the mechanism is the name of the
/// array and not a file.
///
/// The shape of each entry does NOT change: the document is the concatenation
/// of what used to come out separately. That is why `parse_elements_toml` can
/// reuse the same decoders, and an old pack is still read with the five
/// `parse_*_toml` above, which remain valid for LEGACY packs and for the
/// project files.
/// runtime_enhancement: an Identity marked "Enhance in software", already
/// EXPANDED to its motor identity (layer, hashes) — the runtime resolves
/// nothing again: the indexed compose enhances, per (layer, hash), whatever no
/// HD claimed. `layer` is the SceneElement layer: 0=B · 1=A · 2=W · 3=Sprite
/// (not the Paint plane, which is 0=A · 1=B · 2=Window!).
/// `[[enhance]]` blocks inside elements.toml; an old player ignores the unknown
/// array and degrades to the original — [[set]]/[[screen]] do not change.
struct PackEnhance {
    uint64_t    id = 0;            ///< id of the source Identity (informational)
    std::string name;
    uint8_t     layer = 3;         ///< SceneElement layer
    std::vector<uint64_t> hashes;  ///< pipe-list "0x…|0x…" in the TOML
    uint8_t     k = 255;           ///< strength 0..255; absent = 255 (omitted at the default)
};
std::string bake_enhance_toml(const std::vector<PackEnhance>& enh);
size_t parse_enhance_toml(const std::string& text, std::vector<PackEnhance>& out);

std::string bake_elements_toml(const std::vector<PackScreen>& screens,
                               const std::vector<PackPanorama>& pans,
                               const std::vector<PackKinematic>& kins,
                               const std::vector<PackPlaneSequence>& seqs,
                               const std::vector<PackPlaneSet>& sets,
                               const std::vector<PackPlaneFont>& fonts,
                               const std::vector<PackEnhance>& enh = {});

/// Reads the single document in one parse pass. Returns the total of valid
/// entries across every family. `enh` may be nullptr: a consumer that enhances
/// nothing does not need the list.
size_t parse_elements_toml(const std::string& text,
                           std::vector<PackScreen>& screens,
                           std::vector<PackPanorama>& pans,
                           std::vector<PackKinematic>& kins,
                           std::vector<PackPlaneSequence>& seqs,
                           std::vector<PackPlaneSet>& sets,
                           std::vector<PackPlaneFont>& fonts,
                           std::vector<PackEnhance>* enh = nullptr);

}  // namespace ayther
