#pragma once
// ---------------------------------------------------------------------------
// C declarations for symbols exported from the Rust ayther_core static lib.
// Keep this header in sync with core/src/lib.rs.
//
// Most of the type-safe surface now goes through cxx::bridge (core/src/ffi.rs,
// integrated via corrosion). The extern-C wrappers below remain ONLY for the
// zero-copy hot path (process_frame / update_ram / set_pack — raw pointers cxx
// does not bridge); keep them in sync with lib.rs by hand.
// ---------------------------------------------------------------------------
// `<stdint.h>` instead of `<cstdint>` and a guarded `extern "C"`: a step
// towards being includable from C, and from C++ nothing changes.
//
// NOTICE, so as not to half-promise: this header is **not pure C yet**. It uses
// `bool` and struct names without a `typedef`, so a `.c` will not compile it.
// The SDK's C API is `ayther_sdk.h` —that is the surface designed for C, pack
// reading included— and this is a shared contract header in C++.
// The `pack_read` example exposed it, by trying to use it from C.
#include <stdint.h>
#include <stddef.h>

#include <ayther/ayther_version.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------

/// Returns AYTHER_CORE_C_ABI_REVISION, not the AYTHER SemVer release number.
uint32_t ayther_core_version();

// ---------------------------------------------------------------------------
// Sonic 2 — 68000 work-RAM reads
// ---------------------------------------------------------------------------

/// Read player X/Y position.
/// Offsets in the 64 KB RAM block: X=0xB008, Y=0xB00C (Big-Endian i16).
bool ayther_sonic_read_xy(const uint8_t* ram, size_t size,
                           int16_t* out_x, int16_t* out_y);

/// Read player X/Y velocity (signed fixed-point subpixels/frame).
/// Offsets: VX=0xB014, VY=0xB018 (Big-Endian i16).
bool ayther_sonic_read_velocity(const uint8_t* ram, size_t size,
                                 int16_t* out_vx, int16_t* out_vy);

// ---------------------------------------------------------------------------
// TileHasher — opaque sprite fingerprinting engine
//
// Ownership: caller creates with _new(), must release with _free().
// Thread safety: NOT thread-safe; drive from one emulation thread.
//
// Pixel format values (RETRO_PIXEL_FORMAT_*):
//   0 = 0RGB1555 (legacy)
//   1 = XRGB8888
//   2 = RGB565   (Genesis Plus GX default)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Audio condition gate (the evaluator lives in the core)
// ---------------------------------------------------------------------------

struct AudioEventGate;   // opaque — do not dereference

/// Compiles the gate from the text of audio_events.toml. Returns NULL when
/// there is no condition at all (the normal case), so the caller saves the
/// per-frame query without having to ask anything.
AudioEventGate* ayther_audio_gate_new(const char* text);
void            ayther_audio_gate_free(AudioEventGate* g);

/// Signatures whose conditions are NOT met this frame — the ones that have to
/// play as the ORIGINAL. Writes up to `cap` and returns the total available.
uint32_t ayther_audio_gate_eval(const AudioEventGate* g,
                                const uint8_t* ram, size_t ram_len,
                                bool word_swapped, uint32_t frame,
                                uint64_t* out, uint32_t cap);

// ---------------------------------------------------------------------------
// EM-8.2 — the WIDESCREEN gate (the same path A as the audio one)
// ---------------------------------------------------------------------------

struct WidescreenGate;   // opaque — do not dereference

/// Compiles the gate from the text of widescreen.toml. Returns NULL when the
/// pack declares no `[[widescreen]]` — every pack baked so far — so the caller
/// saves the per-frame query AND does not switch off the Lab's manual
/// widescreen.
WidescreenGate* ayther_widescreen_gate_new(const char* text);
void            ayther_widescreen_gate_free(WidescreenGate* g);

/// The logical width of this frame. Writes `out_width` and returns true ONLY
/// if some rule matched; on false the caller keeps what it had.
bool ayther_widescreen_gate_eval(const WidescreenGate* g,
                                 const uint8_t* ram, size_t ram_len,
                                 bool word_swapped, uint32_t frame,
                                 uint32_t* out_width);

struct AytherTileHasher;  // opaque — do not dereference

/// Allocate a new TileHasher.  Free with ayther_tile_hasher_free().
AytherTileHasher* ayther_tile_hasher_new();

/// Destroy a TileHasher and release its memory.
void ayther_tile_hasher_free(AytherTileHasher* h);

/// Submit one video frame.
/// Returns the number of NEW unique tiles discovered in this frame.
uint32_t ayther_tile_hasher_process_frame(AytherTileHasher* h,
                                           const uint8_t*    pixels,
                                           uint32_t          width,
                                           uint32_t          height,
                                           size_t            pitch,
                                           uint32_t          pixel_format);

/// Total unique tiles accumulated since creation.
uint32_t ayther_tile_hasher_unique_count(const AytherTileHasher* h);

/// Dump the full catalog to a TOML file.  Returns true on success.
bool ayther_tile_hasher_dump_toml(const AytherTileHasher* h,
                                   const char*             path);

// ---------------------------------------------------------------------------
// AyArchive — opaque .ay pack filesystem  (v0.4.0)
//
// Format: ZIP container + manifest.toml + optional ED25519 signature.
//
// Ownership: caller opens with _open(), must release with _close().
// Thread safety: NOT thread-safe; use from one thread at a time.
// ---------------------------------------------------------------------------

struct AyArchive;  // opaque — do not dereference

/// Open a .ay pack file.  Returns NULL on error (bad signature, missing
/// manifest, corrupt ZIP).  Unsigned packs are accepted in Debug builds only.
/// Free the handle with ayther_pack_close().
AyArchive* ayther_pack_open(const char* path);

/// Destroy an AyArchive and release its memory.
void ayther_pack_close(AyArchive* pack);

/// Set the active region for transparent asset overrides.
/// E.g. "JP" → read("graphics/title.png") tries "locales/JP/graphics/title.png" first.
void ayther_pack_set_region(AyArchive* pack, const char* region);

/// Bitmask of included resolution tiers (bit t = tier t present; 0 = legacy
/// pack). Tiers: 0=HD 3x · 1=Full HD 4.5x · 2=2K 6x · 3=4K 9x · 4=8K 18x. The
/// index GROWS with resolution: `set_tier` scans upwards from the ideal, and an
/// out-of-order one would make it pick too low.
uint8_t ayther_pack_tiers(const AyArchive* pack);

/// Activates the tier for the display's `ideal` — the lowest included one >=
/// ideal, or the highest included one if there is none. Lookups resolve
/// `tiers/<active>/<name>` transparently. No-op on legacy packs.
void ayther_pack_set_tier(AyArchive* pack, int ideal);

/// Maps the output height (px) to the ideal tier and activates it:
/// <=720 HD · <=1080 Full HD · <=1440 2K · <=2160 4K · above that, 8K.
void ayther_pack_set_tier_for_height(AyArchive* pack, int out_height_px);

/// Return the size in bytes of a logical asset, or -1 if not found.
int64_t ayther_pack_file_size(const AyArchive* pack, const char* logical_path);

/// Read a logical asset into out_buf (capacity buf_cap bytes).
/// Returns bytes written, or -1 if path not found or buffer too small.
/// Pre-allocate using ayther_pack_file_size().
int64_t ayther_pack_read(const AyArchive* pack, const char* logical_path,
                          uint8_t* out_buf, size_t buf_cap);

/// Whether the entry can be read BY RANGE. Asking first is what allows a
/// strategy to be chosen: with streaming a video plays without being
/// materialised (RAM = one frame); without streaming it has to be read whole.
///
/// It is true only for `Stored` entries whose signed index carries per-chunk
/// hashes: packs older than that, and every deflated entry, return false.
/// How many entries the pack has, and the name of each (stable alphabetical
/// order). The name is COPIED into the caller's buffer: a borrowed pointer
/// would require knowing how long it lives, and that is the contract nobody
/// reads. Returns the bytes written, 0 if the index does not exist, and
/// NEGATIVE (the required length) if the buffer is too small.
uint32_t ayther_pack_entry_count(const AyArchive* pack);
int32_t  ayther_pack_entry_name(const AyArchive* pack, uint32_t i,
                                char* dst, uint32_t cap);

bool ayther_pack_entry_streamable(const AyArchive* pack, const char* logical_path);

/// Reads `len` bytes of an entry from `offset` without materialising it.
///
/// Returns the bytes written —which may be FEWER than `len` when the range
/// reaches the end of the entry— or -1 if the entry is not addressable by
/// range, if the range falls outside, or if a chunk fails verification against
/// the signed index. Nothing leaves here unverified: the unit of verification
/// is the chunk, not the entry, and that is what makes the partial read
/// cheap.
int64_t ayther_pack_read_range(const AyArchive* pack, const char* logical_path,
                               uint64_t offset, uint8_t* out_buf, size_t len);

/// Return the pack's game_id as a null-terminated string.
/// Valid for the lifetime of the AyArchive.  Do NOT free the pointer.
const char* ayther_pack_game_id(const AyArchive* pack);

// ---------------------------------------------------------------------------
// Compatibility validation, BEFORE opening the pack
// ---------------------------------------------------------------------------
//
// It returns a LIST of findings and not a boolean, because there are two
// different things a pack can have wrong:
//
//   · CRITICAL incompatibility (it belongs to another game, it asks for an
//     Engine that does not exist) → error: opening it would serve the wrong
//     content;
//   · OPTIONAL degradation (it brings a subsystem this build does not know, it
//     was baked with another core) → warning: run and say so.
//
// With a boolean, either the second is rejected —and a usable pack does not
// open— or the first is accepted, and the user sees another game's pack without
// knowing why.
//
// It does not open the pack: that is why an incompatible pack cannot take the
// session down.

typedef struct AytherPackReport AytherPackReport;

/// What the session has loaded. Null pointers and `has_rom = false` mean "not
/// known" — and that comes out as a WARNING in the report, so that "it was not
/// checked" does not read as "it is fine".
typedef struct {
    uint32_t    rom_crc32;
    bool        has_rom;
    const char* platform;        ///< "megadrive" · "segacd" · NULL
    const char* core_build_id;   ///< build_id of the fork's core · NULL
    const char* engine_version;  ///< NULL = the one from this build
    bool        release_build;   ///< in release, an unsigned pack is an ERROR
} AytherValidateCtx;

AytherPackReport* ayther_pack_validate(const char* path, const AytherValidateCtx* ctx);
uint32_t          ayther_pack_report_count(const AytherPackReport* r);
/// 0 = error · 1 = warning · 2 = recommendation · -1 out of range.
int32_t           ayther_pack_report_severity(const AytherPackReport* r, uint32_t i);
/// Stable code, so decisions need no prose parsing.
const char*       ayther_pack_report_code(const AytherPackReport* r, uint32_t i);
/// Human-readable message.
const char*       ayther_pack_report_message(const AytherPackReport* r, uint32_t i);
/// The only question that decides whether to start. Warnings are shown all the
/// same.
bool              ayther_pack_report_has_errors(const AytherPackReport* r);
void              ayther_pack_report_free(AytherPackReport* r);

/// The compatibility GRADE, derived from the same report above.
///
/// It exists alongside `ayther_pack_validate` because the question is
/// different: the report says WHAT is happening, the grade says WHAT TO DO. And
/// because the criterion for going from a list of findings to a verdict has to
/// be a single one — Play, the SDK and the Hub give the same answer because
/// they call in here.
typedef struct AytherCompat AytherCompat;

/// 0 exact · 1 with warnings · 2 experimental · 3 incompatible.
/// Best to worst, and the order is part of the contract.


/// EM-4.1: the SHAPE hash of a tile — invariant to brightness, sensitive to the
/// silhouette. It groups the variants a CONTENT-BASED fade produces (the game
/// writes tiles with darker indices), which are the ones that cost authoring
/// effort: a PALETTE fade is already grouped by the sprite hash on its own,
/// which is palette-blind.
///
/// It does not replace the identity hash: it accompanies it. One says "which
/// tile this is" and this one, "which family it belongs to".
uint64_t ayther_tile_shape_hash(const uint8_t* tile, uint32_t n);
/// Mean level of the OPAQUE pixels (0..15). Negative = fully transparent, which
/// is NOT the same as a black tile.
float    ayther_tile_mean_level(const uint8_t* tile, uint32_t n);
/// How much to dim the `referencia` asset to reproduce `tile`. Negative = one
/// of them has no opaque pixels.
float    ayther_tile_brightness_factor(const uint8_t* tile, const uint8_t* referencia);

/// EM-7.4: the USER's IPS/BPS patches, applied in RAM.
///
/// A fan translation or a romhack is a patch: it describes how to transform a
/// ROM the user already has, without carrying the game inside. That is why it
/// can be distributed where the ROM cannot, and why this is BYOR-safe.
///
/// The patch is applied to the BUFFER handed to the core, never to the file on
/// disk: the same doctrine as the rest of the project, and here it additionally
/// protects the user from losing their original ROM by trying a hack.
bool    ayther_is_rom_patch(const uint8_t* data, uint32_t size);
/// Bytes written, or negative: -1 args · -2 not a patch · -3 does not fit
/// (the required size is left in `out_needed`) · -4 failure (see the error).
int64_t ayther_apply_rom_patch(const uint8_t* rom, uint32_t rom_n,
                               const uint8_t* parche, uint32_t parche_n,
                               uint8_t* out, uint32_t out_cap,
                               uint32_t* out_needed);
/// The reason for the last failure. A "could not patch" with no reason leaves
/// the user unable to tell whether they downloaded the wrong patch or their ROM
/// is damaged.
uint32_t ayther_rom_patch_error(char* buf, uint32_t cap);

AytherCompat* ayther_pack_compat(const char* path, const AytherValidateCtx* ctx);
int32_t       ayther_compat_grade(const AytherCompat* c);
/// Never empty for a valid handle.
const char*   ayther_compat_reason(const AytherCompat* c);
/// What could NOT be verified — it is what separates "experimental" from
/// "exact".
uint32_t      ayther_compat_unverified_count(const AytherCompat* c);
const char*   ayther_compat_unverified(const AytherCompat* c, uint32_t i);
/// The whole verdict as JSON, with the report inside.
const char*   ayther_compat_json(const AytherCompat* c);
void          ayther_compat_free(AytherCompat* c);

// ---------------------------------------------------------------------------
// Remastering profiles
// ---------------------------------------------------------------------------
//
// A profile does NOT multiply the material: it filters what is already there.
// It declares which subsystems it enables and which buses it mutes; the assets
// are the same. Without that, a pack with four profiles would weigh four times
// as much.
//
// The list ALWAYS carries "original" first (implicit, undeclared and
// unremovable) and always has exactly one default: the caller does not have to
// defend against an empty list nor against two defaults.

/// How many profiles the pack offers. Never 0.
uint32_t ayther_pack_profile_count(const AyArchive* pack);

/// Field `field` of profile `i`: "id" · "name" · "description". The string is
/// valid until the next call to this or to `ayther_pack_meta_field` — they
/// share a buffer, because two with the same rule only add one more way to get
/// it wrong.
const char* ayther_pack_profile_field(AyArchive* pack, uint32_t i, const char* field);

/// Subsystems it enables (bit j = `ayther_subsystem_name(j)`). 0 on "original",
/// which is correct: it enables nothing.
uint32_t ayther_pack_profile_systems(const AyArchive* pack, uint32_t i);

/// Buses it mutes (0=unclassified · 1=music · 2=effects · 3=voices).
uint32_t ayther_pack_profile_muted_buses(const AyArchive* pack, uint32_t i);

/// The profile applied when loading the pack without asking for another.
uint32_t ayther_pack_default_profile(const AyArchive* pack);

/// Index of profile `id`, or -1 if the pack does not have it. It does not
/// return 0 because "does not exist" and "exists and enables nothing" are
/// different things.
int32_t ayther_pack_profile_index(const AyArchive* pack, const char* id);

// ---------------------------------------------------------------------------
// Pack credits and provenance, for Play and the Hub
// ---------------------------------------------------------------------------

/// Opaque handle with `credits.toml` already parsed. It is requested, queried
/// and released: the pack does not pay for the parse when nobody displays them.
typedef struct AytherCredits AytherCredits;

/// NULL if the pack carries no credits or if the file is broken — a pack
/// without credits is valid and one with an unreadable file still has to be
/// playable. What it must not do is show an invented attribution.
AytherCredits* ayther_pack_credits(const AyArchive* pack);

/// How many PEOPLE the pack credits (not how many assets).
uint32_t    ayther_credits_count(const AytherCredits* c);
const char* ayther_credits_author(const AytherCredits* c, uint32_t i);
/// Declared role, or an empty string (not NULL: a missing role is not out of
/// range).
const char* ayther_credits_role(const AytherCredits* c, uint32_t i);
/// The licences they contributed, comma-separated.
const char* ayther_credits_licenses(const AytherCredits* c, uint32_t i);
uint32_t    ayther_credits_assets(const AytherCredits* c, uint32_t i);

/// The attribution of asset `asset_id` — what Play shows for the asset it is
/// using. The id is the content name of the entry, without `assets/` and
/// without the tier prefix: the same drawing across four tiers is ONE asset and
/// has ONE provenance. NULL if that asset declares nothing.
const char* ayther_credits_attribution(const AytherCredits* c, const char* asset_id);

void ayther_credits_free(AytherCredits* c);

// ---------------------------------------------------------------------------
// Manifest metadata, queryable WITHOUT running the pack
// ---------------------------------------------------------------------------

/// The manifest SCHEMA version this build writes and understands.
uint32_t ayther_manifest_schema_supported(void);

/// The physical .ay container FORMAT this build writes and understands.
///
/// This is independent from the manifest schema: schema versions metadata,
/// while format versions the container and its root layout. Missing format and
/// schema declarations both mean version 1; a newer declaration is rejected.
uint32_t ayther_pack_format_supported(void);

/// The ENGINE version — the same one the validator compares the pack's
/// `engine_min` against. The core exposes it so the technical report does not
/// keep a second copy of the number: two copies drift apart on the first bump
/// and the report starts lying about which Engine baked the pack.
const char* ayther_engine_version(void);

/// The schema DECLARED by the pack (1 = baked before the field existed). A pack
/// with a schema HIGHER than the supported one does not open: tolerance of
/// unknown fields is data forward-compatibility, but opening a pack that says
/// it depends on something this build cannot read would be serving it half-way
/// and calling it success.
uint32_t ayther_pack_schema(const AyArchive* pack);

/// The subsystems AYTHER knows how to substitute, in CANONICAL ORDER. The index
/// is the contract with the Engine's `AytherSubsystem`, and there is a test
/// comparing the two lists name by name.
uint32_t    ayther_subsystem_count(void);
const char* ayther_subsystem_name(uint32_t index);

/// Mask of subsystems the pack DECLARES it carries (bit i = the pack carries
/// `ayther_subsystem_name(i)`).
///
/// **0 is ambiguous**: it has to be read together with
/// `ayther_pack_declares_systems`. A legacy pack declares nothing, and that is
/// not the same as declaring that it carries nothing — treating them alike
/// would make every old pack appear empty.
uint32_t ayther_pack_systems(const AyArchive* pack);
bool     ayther_pack_declares_systems(const AyArchive* pack);

/// A field of `[compat]` or of authoring, as a NUL-terminated string.
/// `field`: "rom_crc32" · "platform" · "core_min" · "license" · "contributors"
/// (the last one comma-separated). NULL = not declared, which is different from
/// declared empty.
///
/// The pointer lives until the next call on the SAME pack (it is cached inside
/// so as not to leak one allocation per query), so it has to be copied before
/// asking again.
const char* ayther_pack_meta_field(AyArchive* pack, const char* field);

/// The pack BUILD ID — it identifies ONE concrete bake.
///
/// It is NOT declared in the pack: it is DERIVED from the bytes of
/// `integrity.toml`, which is the set of hashes of everything inside. That is
/// why it cannot lie (a manifest field is editable; this is recomputed), two
/// identical bakes give the same id, and there is no circularity — declaring it
/// in the manifest was impossible, because integrity covers the manifest.
///
/// It is what makes a pack whose assets are named by hash diagnosable: the
/// error message carries `hash - game vN build XXXX` and the Lab's search
/// resolves that pair against the bake log.
///
/// EMPTY on legacy packs (no integrity.toml). Treat it as "unknown", not as an
/// id.
const char* ayther_pack_build_id(const AyArchive* pack);

// ---------------------------------------------------------------------------
// AytherPackWatcher — cross-platform pack hotreload watcher  (v0.9.5)
//
// Wraps a background OS file-change notifier:
//   Windows — ReadDirectoryChangesW
//   Linux   — inotify
//   macOS   — FSEvents
//
// poll() is non-blocking: drains a background-thread mpsc channel.
// Intended to be called once per frame (~16 ms period).
//
// Ownership: caller creates with _new(), must release with _free().
// ---------------------------------------------------------------------------

struct AytherPackWatcher;  // opaque — do not dereference

/// Start watching `path` for modifications/creations.
/// Returns NULL if the path's parent directory cannot be watched
/// (unsupported platform, bad path, permission error, etc.).
AytherPackWatcher* ayther_pack_watcher_new(const char* path);

/// Non-blocking poll.
/// Returns true if the watched file was created or modified since the
/// last call.  Drains all pending OS events so subsequent calls within
/// the same frame do not re-trigger.
/// Returns false if `w` is NULL.
bool ayther_pack_watcher_poll(AytherPackWatcher* w);

/// Destroy a watcher and release its memory.
void ayther_pack_watcher_free(AytherPackWatcher* w);

// ---------------------------------------------------------------------------
// ScriptEnv — sandboxed Lua 5.4 runtime
//
// API available to pack scripts:
//   ayther.version()           → string
//   ayther.log(msg)            → void
//   ayther.ram.read_u8(n)      → u8
//   ayther.ram.read_u16_be(n)  → u16   big-endian (68000 native)
//   ayther.ram.read_i16_be(n)  → i16   (player positions, velocity)
//   ayther.ram.read_u32_be(n)  → u32
//   ayther.pack.read(path)     → string|nil
//   ayther.pack.exists(path)   → bool
//   ayther.on_frame(fn)        → register a per-frame callback
//
// Ownership: caller creates with _new(), must release with _free().
// Thread safety: NOT thread-safe; drive from the emulation thread only.
// ---------------------------------------------------------------------------

struct AytherScriptEnv;  // opaque — do not dereference

/// Create a sandboxed Lua 5.4 ScriptEnv.  Returns NULL on failure.
AytherScriptEnv* ayther_script_new();

/// Destroy a ScriptEnv.
void ayther_script_free(AytherScriptEnv* env);

/// Link the loaded .ay pack so `ayther.pack.*` works.  Pass NULL to detach.
void ayther_script_set_pack(AytherScriptEnv* env, const AyArchive* pack);

/// Load and execute a Lua source string.  Returns true on success.
bool ayther_script_load_string(AytherScriptEnv* env,
                                const char*      source,
                                const char*      chunk_name);

/// Snapshot RAM and fire all ayther.on_frame callbacks.
/// Returns the count of callbacks that completed without error.
uint32_t ayther_script_on_frame(AytherScriptEnv* env,
                                 const uint8_t*   ram,
                                 size_t           ram_size);

// ---------------------------------------------------------------------------
// Tile substitution — POD structs  (v0.6.0)
// (Defined before the functions that reference them.)
// ---------------------------------------------------------------------------

/// xxHash3-64 tile occurrence in a single frame.
struct AytherTileOccurrence {
    uint64_t hash;
    uint32_t tile_x;  ///< tile-grid column (pixel_x = tile_x × 8)
    uint32_t tile_y;  ///< tile-grid row    (pixel_y = tile_y × 8)
};

/// Lua-registered tile override: replace hash with the given pack asset.
struct AytherTileOverride {
    uint64_t hash;
    char     asset_path[256];  ///< null-terminated logical path in the .ay pack
};

/// Resolved substitution instruction: blit asset at (tile_x, tile_y).
struct AytherTileSub {
    char     asset_path[256];
    uint32_t tile_x;
    uint32_t tile_y;
};

/// Push tile occurrences for the current frame so `ayther.tiles.list()`
/// returns correct data inside on_frame callbacks.
/// Call this BEFORE ayther_script_on_frame().
/// Also call ayther_script_update_audio() before on_frame for audio list.
void ayther_script_update_tiles(AytherScriptEnv*            env,
                                 const AytherTileOccurrence* occs,
                                 uint32_t                    count);

/// Read tile overrides registered by Lua `ayther.tiles.replace()`.
/// Fill out_buf with up to buf_cap entries.  Returns entries written.
uint32_t ayther_script_get_tile_overrides(const AytherScriptEnv* env,
                                           AytherTileOverride*    out_buf,
                                           uint32_t               buf_cap);

// Forward declarations required by the script API below (full definitions
// appear later in this header in the sprite/audio hasher sections).
struct AytherSpriteOccurrence;
struct AytherAudioOccurrence;

// ---------------------------------------------------------------------------
// Lua sprite API  (v0.9.3)
// ---------------------------------------------------------------------------

/// Push the current frame's sprite occurrences so `ayther.sprites.list()` returns
/// correct data inside on_frame callbacks.
/// Call this BEFORE ayther_script_on_frame() each frame.
void ayther_script_update_sprites(AytherScriptEnv*              env,
                                   const AytherSpriteOccurrence* occs,
                                   uint32_t                      occ_count);

/// Lua-registered sprite override: replace hash with the given pack asset.
struct AytherSpriteOverride {
    uint64_t hash;
    char     asset_path[256];
};

/// Read sprite overrides registered by Lua `ayther.sprites.replace()`.
/// Fill out_buf with up to buf_cap entries.  Returns entries written.
uint32_t ayther_script_get_sprite_overrides(const AytherScriptEnv* env,
                                             AytherSpriteOverride*  out_buf,
                                             uint32_t               buf_cap);

// ---------------------------------------------------------------------------
// Lua audio API  (v0.9.2)
// ---------------------------------------------------------------------------

/// Push the current tick's audio occurrences so `ayther.audio.list()` returns
/// correct data inside on_frame callbacks.
/// Call this BEFORE ayther_script_on_frame() each tick.
void ayther_script_update_audio(AytherScriptEnv*             env,
                                 const AytherAudioOccurrence* occs,
                                 uint32_t                     occ_count);

/// Lua-registered audio override: replace hash with the given pack asset.
struct AytherAudioOverride {
    uint64_t hash;
    char     asset_path[256];
};

/// Read audio overrides registered by Lua `ayther.audio.replace()`.
/// Fill out_buf with up to buf_cap entries.  Returns entries written.
uint32_t ayther_script_get_audio_overrides(const AytherScriptEnv* env,
                                            AytherAudioOverride*   out_buf,
                                            uint32_t               buf_cap);

/// Retrieve all tile occurrences from the most-recently-processed frame.
/// A 320×240 frame produces up to 1200 entries (40×30 tiles).
/// Returns the number of entries written to out_buf.
uint32_t ayther_tile_hasher_get_occurrences(const AytherTileHasher*  h,
                                             AytherTileOccurrence*    out_buf,
                                             uint32_t                 buf_cap);

// ---------------------------------------------------------------------------
// TileSubstitutor — hash-to-HD-asset mapping engine  (v0.6.0)
//
// Ownership: caller creates with _new(), must release with _free().
// ---------------------------------------------------------------------------

struct AytherTileSubstitutor;  // opaque — do not dereference

/// Create a TileSubstitutor.  Free with ayther_tile_sub_free().
AytherTileSubstitutor* ayther_tile_sub_new();

/// Destroy a TileSubstitutor.
void ayther_tile_sub_free(AytherTileSubstitutor* s);

/// Load the substitution catalog from `tile_substitutions.toml` in the pack.
void ayther_tile_sub_load_pack(AytherTileSubstitutor* s, const AyArchive* pack);

/// Load the catalog from a NAMED toml in the pack (Phase 2c: planes →
/// `plane_tile_substitutions.toml`). Same [[sub]] format.
void ayther_tile_sub_load_pack_named(AytherTileSubstitutor* s,
                                     const AyArchive* pack, const char* file);

/// Direct hash → asset lookup (override > catalog). Copies the NUL-terminated
/// path into `out` (cap bytes); returns true if there was an assignment. For
/// the scroll-aware resolver of plane tiles (which computes the position on its
/// own).
bool ayther_tile_sub_lookup(const AytherTileSubstitutor* s, uint64_t hash,
                            char* out, uint32_t cap);

/// EM-2: evaluates the catalogue conditions for this frame and fixes the
/// current asset per hash. Call it ONCE per frame BEFORE the lookups.
/// `ram` is the core's raw Work RAM; `word_swapped` declares whether it comes
/// with the Mega Drive `addr ^ 1` on LE hosts (for the 68k: true) — that way an
/// address from the TOML is the same one RetroAchievements / Data Crystal show.
/// A cheap no-op if the pack carries no conditions.
void ayther_tile_sub_begin_frame(AytherTileSubstitutor* s,
                                 uint64_t               frame_number,
                                 const uint8_t*         ram,
                                 uint32_t               ram_len,
                                 bool                   word_swapped);

/// Register a Lua-driven runtime override (hash → asset_path).
void ayther_tile_sub_add_override(AytherTileSubstitutor* s,
                                   uint64_t               hash,
                                   const char*            asset_path);

/// Clear all Lua-registered overrides.  Call at start of each frame.
void ayther_tile_sub_clear_overrides(AytherTileSubstitutor* s);

/// Resolve tile occurrences to HD substitution instructions.
/// Returns the number of entries written to out_buf.
uint32_t ayther_tile_sub_resolve(const AytherTileSubstitutor* s,
                                  const AytherTileOccurrence*  occs,
                                  uint32_t                     occ_count,
                                  AytherTileSub*               out_buf,
                                  uint32_t                     buf_cap);

// ---------------------------------------------------------------------------
// SpriteHasher — VDP SAT sprite detection  (v0.8.0)
//
// Reads raw VRAM (retro_get_memory_data(RETRO_MEMORY_VIDEO_RAM)) each frame,
// parses the Sprite Attribute Table, and produces position-independent
// xxHash3-64 fingerprints per sprite character.
//
// The SAT base is set by VDP register $5 and varies by game and by H32/H40
// mode, so it must not be hardcoded. Pass AYTHER_SAT_AUTODETECT to recover it
// from VRAM structure each frame (the production path).
//
// Ownership: caller creates with _new(), must release with _free().
// Thread safety: NOT thread-safe; drive from the emulation thread only.
// ---------------------------------------------------------------------------

struct AytherSpriteHasher;  // opaque — do not dereference

/// Pass as `sat_base` to auto-detect the SAT base from VRAM (recommended).
/// A fixed address (e.g. 0xD800) only matches a subset of games/modes.
#define AYTHER_SAT_AUTODETECT ((size_t)-1)

/// Allocate a new SpriteHasher.  Free with ayther_sprite_hasher_free().
AytherSpriteHasher* ayther_sprite_hasher_new();

/// Destroy a SpriteHasher.
void ayther_sprite_hasher_free(AytherSpriteHasher* h);

/// Process one frame of raw VRAM.
/// vram_size is typically 65536 (64 KB) for Mega Drive.
/// sat_base: pass AYTHER_SAT_AUTODETECT to derive it from VRAM (recommended),
/// or an explicit byte offset to force a specific SAT base.
/// Returns the number of NEW unique sprite patterns discovered this frame.
uint32_t ayther_sprite_hasher_process_vram(AytherSpriteHasher* h,
                                            const uint8_t*      vram,
                                            size_t              vram_size,
                                            size_t              sat_base);

/// Process the sprites the VDP actually parsed this frame (the fork captures them in
/// parse_satb — AYTHER_MEMORY_PARSED_SPRITES, id 0x10B). `sprites` = `count` records
/// of 8 bytes each (yr/xr/attr u16 LE + w/h u8). Authoritative "what was drawn",
/// robust to mid-frame SAT rewrites/base swaps (Aladdin's Sega-logo genie). Tiles
/// hashed from `vram`. count == 0 → returns 0 (caller falls back to autodetect).
uint32_t ayther_sprite_hasher_process_sprites(AytherSpriteHasher* h,
                                              const uint8_t*      sprites,
                                              size_t              count,
                                              const uint8_t*      vram,
                                              size_t              vram_size);

/// Total unique sprite patterns accumulated since creation.
uint32_t ayther_sprite_hasher_unique_count(const AytherSpriteHasher* h);

// ---------------------------------------------------------------------------
// Sprite substitution — POD structs  (v0.8.0)
// ---------------------------------------------------------------------------

/// Per-frame sprite occurrence from the SAT (position-independent hash).
///
/// Layout (v0.10/R1.5, 24 bytes, align 8) — link + palette consume the old pad:
///   hash(8) | anim_group_id(8) | w_tiles(1) | h_tiles(1) | screen_x(2) | screen_y(2)
///   | link(1) | palette(1)
///
/// anim_group_id: 0 = static sprite (no detected animation cycle).
///                non-zero = xxHash3-64 of the sorted set of sibling frame hashes
///                that cycle at the same SAT slot over a 64-frame rolling window.
/// link:    SAT link field (index of next sprite in the chain, 0–127) — the
///          strongest metasprite grouping hint.
/// palette: VDP palette index 0–3 — secondary grouping hint.
struct AytherSpriteOccurrence {
    uint64_t hash;
    uint64_t anim_group_id;  ///< animation cycle group (0 = none)
    uint8_t  w_tiles;        ///< sprite width  in tiles (1–4)
    uint8_t  h_tiles;        ///< sprite height in tiles (1–4)
    int16_t  screen_x;       ///< top-left X in screen pixels
    int16_t  screen_y;       ///< top-left Y in screen pixels
    uint8_t  link;           ///< SAT link field (metasprite grouping hint)
    uint8_t  palette;        ///< VDP palette index 0–3
    uint8_t  priority;       ///< VDP priority bit (0=low,1=high) — metasprite front/back
    uint8_t  slot;           ///< SAT slot index 0–79 (Ayther hide-by-hash)
    uint8_t  hflip;          ///< VDP h-flip (CU-AN-11: auto-mirror of the HD sheet)
    uint8_t  vflip;          ///< VDP v-flip
};

/// Resolved HD sprite substitution.
struct AytherSpriteSub {
    char    asset_path[256];  ///< logical path in the .ay pack
    int16_t screen_x;
    int16_t screen_y;
    uint8_t w_tiles;
    uint8_t h_tiles;
    /// EXACT destination size in pixels. The union bbox of a POSE is almost
    /// never a multiple of 8 (e.g. 29×64): truncating it to tiles used to
    /// squash the HD/snapshot when drawing it. 0 = derive from
    /// w_tiles/h_tiles×8 (tile-exact producers: per-sprite, plane tiles,
    /// Mode 3).
    uint16_t w_px;
    uint16_t h_px;
    /// The arrangement the POSE matched with: 0 = the captured one · bit0 = H
    /// mirror · bit1 = V mirror. The asset (snapshot / HD default) is the
    /// CANONICAL facing → the render draws it pre-flipped by these bits (the
    /// mirrored instance is seen in ITS direction). 0 on
    /// per-sprite/planes/Mode 3 and on poses with per-variant candidates.
    uint8_t mirror;
    /// Observed palette of the anchor (the per-sprite occurrence / the pose's
    /// anchor member): the E1 fade anchors here, not on the first occurrence of
    /// the bbox (an unrelated overlapping one — the rider on the Dragon — gave
    /// the wrong palette and the palette flash never modulated the HD).
    /// 0xFF = unknown (producers that do not fill it: plane tiles, Mode 3 →
    /// heuristic).
    uint8_t palette;
    /// The AUTHORED palette of the chosen candidate when it is not the observed
    /// one — the motor approximates the colour by tinting with the live CRAM
    /// observed/candidate ratio. 0xFF = no synthesis.
    uint8_t synth_pal;
    /// Authored reference of the E1 tint (RGB 0-255 average of the CRAM line
    /// when the pose was captured). {0,0,0} = no reference → scalar peak-hold.
    uint8_t ref_rgb[3];
    /// UV sub-rect of the asset (0..1; 0,0,1,1 = the whole quad). The per-
    /// palette-group quads of a mixed pose crop their portion of the asset.
    float u0, v0, uw, vh;
    /// Stable identity of the pose that emitted the sub (in-betweens
    /// §6.1/6.2): the TweenPlayer tracks instances with it. AT THE END of the
    /// struct (ABI: recompile core+engine+lab together when changing it).
    uint64_t pose_key;
    /// Wardrobe: path/asset id of the TINT mask of the pose's BASE asset
    /// ("" = no mask; white = the area follows the palette tint, black = luma
    /// only). Only the pose-sub fills it, and only when the chosen asset is the
    /// base one. AT THE END of the struct (the same ABI rule as pose_key).
    char mask_path[256];
};

/// Retrieve all sprite occurrences from the last processed VRAM frame.
/// Returns entries written to out_buf (up to buf_cap).
uint32_t ayther_sprite_hasher_get_occurrences(const AytherSpriteHasher* h,
                                               AytherSpriteOccurrence*   out_buf,
                                               uint32_t                  buf_cap);

// ---------------------------------------------------------------------------
// Animation clips (C-S1) — ordered pose sequence + per-frame timing per anim
// group. The hasher detects looping cycles from the SAT slot histories and
// consolidates each into an ordered clip (the §4 "phase" of componentes-plan).
// The Lab's Animación workspace reads these as a starting point for authoring.
// ---------------------------------------------------------------------------

/// One frame (pose) of a detected clip. 16 bytes, align 8: pose(8) | duration(2) | pad(6).
struct AytherAnimFrame {
    uint64_t pose;       ///< pose hash (== a sprite occurrence hash / metasprite member)
    uint16_t duration;   ///< frames held (ticks), in phase with the game
};

/// Number of detected animation clips in the hasher's latest recompute.
uint32_t ayther_sprite_hasher_clip_count(const AytherSpriteHasher* h);

/// Reset the animation detector (clear slot history / groups / clips). Call before a
/// clip-generation run so the result reflects only the recording scanned (C-S5).
void ayther_sprite_hasher_reset_animation_grouper(AytherSpriteHasher* h);

/// Read animation clip `index`. Writes `out_id` (= anim_group_id) and
/// `out_looping` (0/1), and up to `frames_cap` frames into `out_frames` in order.
/// Returns the clip's frame count (may exceed `frames_cap` — grow and retry), or
/// 0xFFFFFFFF if `index` is out of range. Any out pointer may be NULL to skip it.
uint32_t ayther_sprite_hasher_get_clip(const AytherSpriteHasher* h,
                                        uint32_t          index,
                                        uint64_t*         out_id,
                                        uint8_t*          out_looping,
                                        AytherAnimFrame*  out_frames,
                                        uint32_t          frames_cap);

// ---------------------------------------------------------------------------
// SpriteSubstitutor — hash-to-HD-asset mapping engine  (v0.8.0)
// ---------------------------------------------------------------------------

struct AytherSpriteSubstitutor;  // opaque — do not dereference

/// Allocate a new SpriteSubstitutor.  Free with ayther_sprite_sub_free().
AytherSpriteSubstitutor* ayther_sprite_sub_new();

/// Destroy a SpriteSubstitutor.
void ayther_sprite_sub_free(AytherSpriteSubstitutor* s);

/// Load the substitution catalog from `sprite_substitutions.toml` in the pack.
void ayther_sprite_sub_load_pack(AytherSpriteSubstitutor* s, const AyArchive* pack);

/// Register a runtime override (hash → asset_path).
void ayther_sprite_sub_add_override(AytherSpriteSubstitutor* s,
                                     uint64_t                 hash,
                                     const char*              asset_path);

/// Override CON la referencia cromática E1 autorada (): `ref_rgb` apunta a
/// 3 bytes (promedio RGB de la línea CRAM al asignar) o es null (= [0,0,0] →
/// el tinte cae al peak-hold escalar gris).
void ayther_sprite_sub_add_override_ref(AytherSpriteSubstitutor* s,
                                        uint64_t                 hash,
                                        const char*              asset_path,
                                        const uint8_t*           ref_rgb);

/// Clear all runtime overrides.  Call at the start of each frame.
void ayther_sprite_sub_clear_overrides(AytherSpriteSubstitutor* s);

/// Resolve sprite occurrences to HD substitution instructions.
/// Returns the number of entries written to out_buf.
uint32_t ayther_sprite_sub_resolve(const AytherSpriteSubstitutor* s,
                                    const AytherSpriteOccurrence*  occs,
                                    uint32_t                       occ_count,
                                    AytherSpriteSub*               out_buf,
                                    uint32_t                       buf_cap);


// ---------------------------------------------------------------------------
// PoseSetSubstitutor — substitution by POSE SIGNATURE (ANIMATED multi-sprite).
// A pose = a set of hashes; it only substitutes when ALL of them are present
// (unclaimed), over the bbox of the members, and it claims them. It resolves
// AFTER the metasprite and BEFORE the per-sprite. `pose_substitutions.toml`:
// [[pose]] with the COMPLETE model (hashes + asset, plus optional: rel/dims
// "x,y|…" = instanced matching, max_w/max_h = guard, flip = the asset's facing,
// ref = authored E1 tint "r,g,b", [[pose.variant]] = palette×flip candidates).
// An old pack (hashes/asset only) loads with the legacy per-set semantics.
// ---------------------------------------------------------------------------
struct PoseSetSubstitutor;  // opaque — do not dereference
PoseSetSubstitutor* ayther_pose_sub_new();
void                ayther_pose_sub_free(PoseSetSubstitutor* p);
/// Load `pose_substitutions.toml` from the pack. Returns the catalog entry count.
uint32_t ayther_pose_sub_load_pack(PoseSetSubstitutor* p, const AyArchive* pack);
/// LIVE preview (Animate): adds a pose override (a set of hashes → asset),
/// resolved with priority over the catalogue and preserved when a pack is
/// loaded.
/// `rel_x`/`rel_y` (n elements, parallel; may be null) = relative offsets of
/// each member → exact INSTANCED matching (1:1 bbox, one sub per instance).
/// `dim_w`/`dim_h` (n elements, parallel; may be null) = size in PX of each
/// member: the off-screen tolerance of an ABSENT member needs ITS real
/// dimensions (without them they are approximated with those of the first
/// visible member and the match fails at the edges). `base_mirror` = the facing
/// the asset is drawn in relative to the captured one (bit0 H · bit1 V, the
/// presentation flip in Pose): it is XORed onto the mirror of the detected
/// arrangement.
void     ayther_pose_sub_add_override(PoseSetSubstitutor* p, const uint64_t* hashes,
                                      const int16_t* rel_x, const int16_t* rel_y,
                                      const int16_t* dim_w, const int16_t* dim_h,
                                      const uint8_t* mem_flips,   // per-member SAT flips, or null
                                      uint32_t n, uint16_t max_w, uint16_t max_h,
                                      uint8_t base_mirror, const uint8_t* ref_rgb,
                                      const uint8_t* ref_lines,   // 12 B (4 lines × RGB) or null
                                      const char* asset,
                                      const char* mask);   // Wardrobe (null/"" = no mask)
/// Like the previous one but with per-variant CANDIDATES (step 2).
/// `default_asset` is the fallback; the candidates go in parallel arrays
/// (palette/hflip/vflip = int8, -1 = any) plus an array of pointers to paths.
/// On a match, the motor picks the candidate closest to the observed variant of
/// the anchor.
void     ayther_pose_sub_add_override_variants(
             PoseSetSubstitutor* p, const uint64_t* hashes,
             const int16_t* rel_x, const int16_t* rel_y,
             const int16_t* dim_w, const int16_t* dim_h,
             const uint8_t* mem_flips,   // per-member SAT flips, or null
             uint32_t n, uint16_t max_w, uint16_t max_h, uint8_t base_mirror,
             const uint8_t* ref_rgb,
             const uint8_t* ref_lines,   // 12 B (4 lines × RGB) or null
             const char* default_asset,
             const int8_t* var_pal, const int8_t* var_hf, const int8_t* var_vf,
             const uint16_t* var_slots,  // per-candidate slot bitmask, or null
             const uint64_t* var_sig,    // per-candidate content signature, or null
             const char* const* var_assets, uint32_t n_var,
             const char* mask);   // Wardrobe (null/"" = no mask)
/// The frame's live CRAM (packed words R0-2/G3-5/B6-8, ≥64 words) — per-line
/// stability tracking + latching of content signatures while in a stable state.
/// Call it every frame BEFORE ayther_pose_sub_resolve.
void     ayther_pose_sub_set_cram(PoseSetSubstitutor* p,
                                  const uint16_t* words, uint32_t n);
/// Content signature of one line — xxh3 of the raw (9-bit) colours of the
/// marked `slots`. The SAME function the runtime uses; the Lab calls it when
/// capturing the variant (authoring and runtime cannot diverge).
uint64_t ayther_palette_signature(const uint16_t* words, uint32_t n,
                                  uint8_t line, uint16_t slots);
/// Clears every live pose override (it does not touch the pack catalogue).
void     ayther_pose_sub_clear_overrides(PoseSetSubstitutor* p);
/// VISIBLE display area according to the live video mode (the frame's fb;
/// H32/H40 × V28/V30). It bounds the matching tolerance for off-screen members:
/// an absent one is only tolerated if its expected rect falls entirely outside
/// [0,w)×[0,h). 0 = ignored. Update it per frame (the mode can change).
void     ayther_pose_sub_set_screen(PoseSetSubstitutor* p, uint16_t w, uint16_t h);
/// Resolve pose-sets this frame. `claimed` (occ_count bytes, in/out) carries the
/// metasprite claims in and gets the pose-set claims OR'd in. Returns subs written.
uint32_t ayther_pose_sub_resolve(const PoseSetSubstitutor*     p,
                                 const AytherSpriteOccurrence* occs,
                                 uint32_t                      occ_count,
                                 uint8_t*                      claimed,
                                 AytherSpriteSub*              out_buf,
                                 uint32_t                      buf_cap);

// ---------------------------------------------------------------------------
// TweenPlayer v2 — in-betweens by TRANSITION (§6.1/6.2). It filters the
// already-resolved HD PER INSTANCE (tracks by pose_key + bbox centre): when a
// track's POSE changes towards a target with an authored transition, it plays
// the intermediate drawings in the first frames of the destination's hold.
// Ladder: exact pair (from→target) > wildcard (target) > direct pop.
// `tween_sequences.toml`: [[tween]] target / from (optional) / frames / ticks.
// ---------------------------------------------------------------------------
struct TweenPlayer;  // opaque — do not dereference
TweenPlayer* ayther_tween_new();
void         ayther_tween_free(TweenPlayer* p);
/// Load `tween_sequences.toml` from the pack. Returns the number of tween entries.
uint32_t     ayther_tween_load_pack(TweenPlayer* p, const AyArchive* pack);
/// Advance in-flight tween timers + expire stale instance tracks. Once per frame.
void         ayther_tween_begin_frame(TweenPlayer* p);
/// Given the resolved `target` HD and the sub's instance identity (pose_key +
/// bbox center in screen px), write into `out_buf` the HD to render this frame.
void         ayther_tween_resolve(TweenPlayer* p, const char* target,
                                  uint64_t pose_key, int32_t cx, int32_t cy,
                                  char* out_buf, uint32_t cap);
/// Drop all instance tracks (non-sequential seeks, pack/take loads) — without
/// this a replay scrub leaves ghost tweens.
void         ayther_tween_clear(TweenPlayer* p);
/// Live override (Lab channel, wins over the pack): from == NULL = wildcard.
void         ayther_tween_set_override(TweenPlayer* p, const char* from,
                                       const char* target, const char* const* frames,
                                       uint32_t n_frames, uint32_t ticks);
void         ayther_tween_clear_overrides(TweenPlayer* p);

// ---------------------------------------------------------------------------
// PackBuilder — assemble + sign a .ay pack in-process  (R8 Deliver)
//
// The Lab's Deliver workspace builds a signed pack without shelling out to the
// ay_pack CLI. Stage entries (manifest.toml, *_substitutions.toml, asset files),
// then finish() to write a dev-signed ZIP that AyArchive verifies on load.
//
// Ownership: caller creates with _new(), must release with _free().
// ---------------------------------------------------------------------------

struct AytherPackBuilder;  // opaque — do not dereference

/// Allocate a new pack builder. Free with ayther_pack_builder_free().
AytherPackBuilder* ayther_pack_builder_new();
void               ayther_pack_builder_free(AytherPackBuilder* b);

/// Stage an entry from raw bytes. Returns false on a null/invalid path
/// ("signature.bin" is reserved and rejected). Paths are normalised to '/'.
bool ayther_pack_builder_add_bytes(AytherPackBuilder* b, const char* path,
                                   const uint8_t* data, size_t len);

/// Stage an entry by reading source_fs_path into the pack at path_in_pack.
/// Returns false if the path is invalid or the source file can't be read.
bool ayther_pack_builder_add_file(AytherPackBuilder* b, const char* path_in_pack,
                                  const char* source_fs_path);

/// Number of entries staged (excludes the signature added by finish).
uint32_t ayther_pack_builder_count(const AytherPackBuilder* b);

/// Write the pack to out_path, dev-signing when `sign` is true. On failure
/// copies a message into err_buf (NUL-terminated, capped at err_cap) and
/// returns false.
bool ayther_pack_builder_finish(AytherPackBuilder* b, bool sign,
                                const char* out_path,
                                char* err_buf, size_t err_cap);

/// ASSET ID — the name a project file lives under inside the pack: the first 32
/// hex characters of the SHA-256 of its content, WITHOUT an extension.
///
/// It lives in the core and not here because it has to be the same digest that
/// verifies entries on read: two implementations of the same hash are silent
/// drift waiting to happen.
///
/// `out` needs 33 bytes (32 hex + NUL). Returns false if the file cannot be
/// read or the buffer is too small — that means "this asset does not go in",
/// never an empty name.
bool ayther_asset_id(const char* fs_path, char* out, size_t cap);

/// Same, but over bytes in memory: for content GENERATED by the bake (the
/// trimmed SoundFont), which has no file to come from.
bool ayther_asset_id_bytes(const uint8_t* data, size_t len, char* out, size_t cap);

// ---------------------------------------------------------------------------
// AudioHasher — PCM batch fingerprinting
//
// Fingerprints each retro_audio_sample_batch call using xxHash3-64 over the
// raw i16 stereo buffer.  Silent batches (all zeros) are skipped.
// Occurrence counts let the Lab "Audio" tab distinguish recurring SFX
// (high hits) from one-shot music frames (hits = 1).
//
// Call sequence per frame:
//   // inside retro_audio_sample_batch callback:
//   ayther_audio_hasher_process_batch(h, data, frames);
//
//   // once at the end of run_frame():
//   ayther_audio_hasher_end_tick(h);
//
// Ownership: caller creates with _new(), must release with _free().
// Thread safety: NOT thread-safe; drive from the emulation thread only.
// ---------------------------------------------------------------------------

struct AytherAudioHasher;  // opaque — do not dereference

/// Allocate a new AudioHasher.  Free with ayther_audio_hasher_free().
AytherAudioHasher* ayther_audio_hasher_new();

/// Destroy an AudioHasher.
void ayther_audio_hasher_free(AytherAudioHasher* h);

/// Fingerprint one stereo PCM batch from retro_audio_sample_batch.
/// data   — stereo-interleaved i16 buffer (L₀,R₀,L₁,R₁,…)
/// frames — number of stereo frames (libretro convention; buffer len = frames × 2)
/// Returns the xxHash3-64 of the batch, or 0 if silent (all zeros).
uint64_t ayther_audio_hasher_process_batch(AytherAudioHasher* h,
                                            const int16_t*     data,
                                            size_t             frames);

/// Snapshot this tick's occurrences and reset in-flight counters.
/// Call exactly once at the end of each run_frame() cycle.
void ayther_audio_hasher_end_tick(AytherAudioHasher* h);

/// Total unique PCM batch patterns accumulated since creation.
uint32_t ayther_audio_hasher_unique_count(const AytherAudioHasher* h);

// ---------------------------------------------------------------------------
// Audio substitution — POD structs
// ---------------------------------------------------------------------------

/// One raw write to a sound chip's bus this frame, in temporal (bus) order.
/// Surfaced by the Ayther fork (ids 0x109/0x10A) as the basis for command-based
/// audio identity (replay-stable, unlike PCM output — see audio_event.rs).
/// Layout-identical to the fork's AytherAudioWrite and RetroRunner::AudioWrite.
struct AytherAudioWrite {
    uint32_t cycle;  ///< CPU M-cycle timestamp within the frame (timing diverges across replay — do NOT use for identity)
    /// FM: the ALREADY-LATCHED REGISTER, 0x000-0x1FF (bit 0x100 = bank 1, i.e.
    /// channels 4-6). PSG: 0 — that chip is single-byte and the latch travels in
    /// the data.
    ///
    /// It is NOT the bus port index. It was until fork `3fc6ee89` (2026-08-11),
    /// which consolidated the audio telemetry: before that the core sent the raw
    /// byte with its port (0-3) and every consumer replicated the latch
    /// protocol. The three that did so —the detector, the router mirror and the
    /// re-synthesis spike— kept doing `addr & 3` after the change, and that left
    /// the Lab MUTE and the detector seeing not a single event for two days,
    /// without any oracle complaining: all three synthesise their test writes
    /// with the same convention as their consumer, so a change of convention
    /// leaves them passing green (2026-08-13).
    uint16_t addr;
    uint8_t  data;   ///< byte written to the chip bus
    uint8_t  chip;   ///< 0 = YM2612 (FM), 1 = SN76489 (PSG)
};

/// One active audio-event substitution this frame (C-A3b): an assigned event whose
/// window covers the current take frame. The motor mutes its channel; the playback
/// layer triggers `asset_path` (one-shot) when `is_start` to keep it in sync.
struct AytherAudioActiveSub {
    uint64_t    signature;   ///< event signature this asset is assigned to
    const char* asset_path;  ///< HD asset (session-owned; valid until reassign/clear)
    uint8_t     chip;        ///< 0 = FM, 1 = PSG
    uint8_t     channel;     ///< FM 0-5 | PSG 0-3
    uint8_t     is_start;    ///< 1 if the event starts on this frame (trigger the asset)
    uint8_t     _pad;
};

/// Per-tick PCM batch occurrence (after ayther_audio_hasher_end_tick).
struct AytherAudioOccurrence {
    uint64_t hash;
    uint32_t frame_count;  ///< stereo frames in the original batch
    uint32_t hits;         ///< times this hash appeared in the current tick
};

/// Resolved HD audio substitution: suppress the emulator batch, play asset.
struct AytherAudioSub {
    uint64_t hash;
    char     asset_path[256];  ///< logical path in the .ay pack (e.g. "audio/sfx/ring.wav")
    uint32_t frame_count;      ///< duration hint: stereo frames in the original batch
};

/// Fill out_buf with occurrences from the most-recently-completed tick.
/// Returns the number of entries written (up to buf_cap).
uint32_t ayther_audio_hasher_get_occurrences(const AytherAudioHasher*  h,
                                              AytherAudioOccurrence*    out_buf,
                                              uint32_t                  buf_cap);

// ---------------------------------------------------------------------------
// AudioEventDetector — audio events from chip commands  (C-A2)
//
// It detects the lifecycle (start/end) of each sound channel from the fork's
// raw write log (AytherAudioWrite), producing activity blocks with a stable
// SIGNATURE (replay-stable, unlike the PCM). It is fed per frame; at the end it
// is closed with _finish. See core/src/audio_event.rs.
//
// Ownership: the caller creates with _new(), releases with _free().
// ---------------------------------------------------------------------------
// Geometric tween (C-S1 Level 1) — glide the HD asset between keyframes.
// The bbox anchor a keyframe's HD is drawn at; interpolating it across the held
// ticks avoids the "pop" of one-HD-per-keyframe playback (modelo-de-autoria 2.3).
// ---------------------------------------------------------------------------
struct AytherTransform { float x, y, w, h; };   // == animation::Transform (16 B)

struct AytherGeometricTween;  // opaque — do not dereference

/// Build from parallel keyframe transforms + hold durations (n each); looping
/// wraps the last key to the first. NULL if n == 0. Free with the _free below.
AytherGeometricTween* ayther_geo_tween_new(const AytherTransform* transforms,
                                           const uint16_t* durs, size_t n, bool looping);
void            ayther_geo_tween_free(AytherGeometricTween* t);
uint32_t        ayther_geo_tween_duration(const AytherGeometricTween* t);
/// Interpolated transform at `tick` (wraps for a loop, clamps for a one-shot).
AytherTransform ayther_geo_tween_sample(const AytherGeometricTween* t, uint32_t tick);

// ---------------------------------------------------------------------------

struct AytherAudioEventDetector;  // opaque — do not dereference

/// The bit of a channel within the 32-bit mute mask:
///
///   bits  0-5   FM  (YM2612)   0-5
///   bits  6-9   PSG (SN76489)  0-3
///   bits 10-17  PCM (the Sega CD RF5C164) 0-7
///   bits 18-31  free — the core REJECTS them when writing region 0x10D
///
/// It lives here, in the contract, and not in every caller, because until
/// 2026-08-13 it was written by hand as `chip == 0 ? (1<<ch) : (1<<(6+ch))` in
/// more than twenty places across the Engine and the Lab. With a third chip
/// that form sends the eight PCM channels into the PSG branch —bits 6 to 13—,
/// trampling the PSG and overflowing the mask without anything failing.
///
/// It returns 0 for a chip that does not take part in the mask, which is a
/// legitimate answer: it means "this channel cannot be muted through here".
inline uint32_t ayther_chan_bit(uint8_t chip, uint8_t channel) {
    if (chip == 0 && channel < 6) return uint32_t(1u) << channel;
    if (chip == 1 && channel < 4) return uint32_t(1u) << (6 + channel);
    if (chip == 3 && channel < 8) return uint32_t(1u) << (10 + channel);
    return 0;
}

/// Every channel the mask can name (18 bits). It is the single "mute
/// everything": the previous value, 0x3FF, left the whole PCM chip playing.
inline constexpr uint32_t kAytherAllChannels = 0x3FFFFu;

/// How many channels the mask can name — the length of any array indexed by
/// `ayther_chan_index`.
inline constexpr int kAytherChanCount = 18;

/// The INDEX of a channel in the canonical order of the mask (FM 1-6 ·
/// PSG 1-4 · PCM 1-8), or -1 if the chip does not take part. It is the same
/// order as the bits, and therefore the same one the timeline lanes use: a
/// single place decides which row each channel goes in.
inline int ayther_chan_index(uint8_t chip, uint8_t channel) {
    if (chip == 0 && channel < 6) return channel;
    if (chip == 1 && channel < 4) return 6 + channel;
    if (chip == 3 && channel < 8) return 10 + channel;
    return -1;
}

/// Short chip name, for UI and export labels.
inline const char* ayther_chip_name(uint8_t chip) {
    return chip == 0 ? "FM" : chip == 1 ? "PSG" : chip == 3 ? "PCM" : "?";
}

/// One detected audio event: a channel's activity span with a stable signature.
struct AytherAudioEvent {
    uint64_t signature;    ///< stable hash of the channel's register snapshot at key-on
    /// Instrument identity: the patch WITHOUT frequency or channel (DAC = the
    /// signature). The same voice across notes/channels shares an instrument
    /// even when the signature differs — it groups "the same sound" for the DAW
    /// export (Mix).
    uint64_t instrument;
    uint32_t start_frame;  ///< frame of the key-on
    uint32_t end_frame;    ///< frame of the key-off (== start_frame for a 1-frame event)
    uint8_t  chip;         ///< 0 = YM2612 (FM), 1 = SN76489 (PSG), 3 = RF5C164 (Sega CD PCM)
    uint8_t  channel;      ///< FM 0-5 | PSG 0-3 | PCM 0-7
    /// MIDI note at key-on (255 = no pitch: DAC/PSG noise/fnum 0).
    /// Piano roll / MIDI (Mix).
    uint8_t  pitch;
    /// "Velocity" at key-on, MIDI scale 1-127 (0 = unknown: DAC and residuals).
    /// On FM it comes from the Total Level of the CARRIER operator —the chip
    /// has no velocity—; on PSG, from the attenuation.
    ///
    /// It occupies the byte that used to be `_pad`: sizeof is still 32 and no
    /// field moves, so the ABI does not change. It is the half of the
    /// information `instrument` deliberately leaves out: volume is not timbre
    /// identity.
    uint8_t  velocity;
};

// ---------------------------------------------------------------------------
// SoundFont — synthesis of the voice assigned to a game timbre
//
// The synthesiser lives on the RUST side: `ayther_engine` is a STATIC library,
// and an LGPL one (FluidSynth) would force dynamic distribution or shipping
// relinkable objects — the same boundary that led to choosing libvpx over
// FFmpeg. On the core side the FFI boundary already exists, so the question
// disappears.
// ---------------------------------------------------------------------------
struct AytherSf2;   // opaque — do not dereference

/// Opens a SoundFont from bytes. NULL on failure. Release with ayther_sf2_free.
AytherSf2* ayther_sf2_new(const uint8_t* data, size_t len, int32_t sample_rate);

/// Same, but it shares the parsed SoundFont between instances with the same
/// `key` (the hash of its path). The motor creates one instance PER TIMBRE so
/// it can BOOST its gain by scaling the buffer — CC 7 runs out at 127.
AytherSf2* ayther_sf2_new_shared(uint64_t key, const uint8_t* data, size_t len,
                                 int32_t sample_rate);
/// Releases the cached SoundFonts no instance uses any more.
void ayther_sf2_trim_cache(void);
void       ayther_sf2_free(AytherSf2* p);
void       ayther_sf2_program(AytherSf2* p, int32_t ch, int32_t preset);
/// MIDI Control Change. CC 7 = channel volume (0-127, default 100): that is
/// where the per-timbre GAIN travels — scaling the buffer would not work
/// because one SoundFont serves several timbres, each on its own channel.
void       ayther_sf2_control(AytherSf2* p, int32_t ch, int32_t cc, int32_t value);
void       ayther_sf2_note_on(AytherSf2* p, int32_t ch, int32_t key, int32_t vel);
void       ayther_sf2_note_off(AytherSf2* p, int32_t ch, int32_t key);
/// Cuts everything immediately — for the game's cuts and for seeks.
void       ayther_sf2_all_notes_off(AytherSf2* p);
/// INTERLEAVED f32 stereo: `out` must hold `frames * 2` floats. With a null
/// `p` it writes SILENCE (the caller queues the buffer anyway, and with garbage
/// it would play white noise).
void       ayther_sf2_render(AytherSf2* p, float* out, size_t frames);
/// Presets of an SF2 WITHOUT loading it into the synthesiser — for the
/// library. Returns the total (which may exceed `cap`).
uint32_t   ayther_sf2_list_presets(const uint8_t* data, size_t len,
                                   uint16_t* out_bank, uint16_t* out_preset,
                                   uint32_t cap);
/// Same but from a PATH and WITH NAMES, one line per preset:
/// `bank:preset|name`. Choosing a timbre means reading names — a list of "0:33"
/// cannot be browsed.
///
/// It takes the path and not a buffer because it reads ONLY the `pdta` chunk,
/// skipping through the RIFF headers: a library of 182 files, two of them
/// ~1 GB, cannot be browsed by loading each one whole. It works on partial SF2
/// files (it does not validate instruments). Returns the bytes written, 0 if it
/// does not fit or fails.
/// It also accepts `.sf3` (same pdta) and `.sfz` (one instrument = one line
/// `0:0|name`, without touching the samples).
size_t     ayther_sf2_preset_list(const char* path, uint8_t* out, size_t cap);

/// Normalises a SoundFont from DISK into flat SF2 in memory: `.sf2` passes
/// straight through, `.sf3` is decompressed (Vorbis samples → PCM) and `.sfz`
/// is converted (text + loose samples → one 0:0 preset). It is THE door through
/// which a new format enters — downstream (synthesiser, bake, pack) everything
/// is still flat SF2. Two calls like ayther_sf2_bake (`cap = 0` queries the
/// size); the result is cached by path, so the conversion is not paid for
/// twice. Returns 0 if it could not be converted.
///
/// Note: ayther_sf2_new/new_shared and ayther_sf2_bake already convert SF3 by
/// BYTE detection — this function is needed for `.sfz`, which needs the path
/// (its samples live next to the text file).
size_t     ayther_soundfont_normalize_file(const char* path,
                                           uint8_t* out, size_t cap);

/// The SoundFonts an `instruments.toml` references, one per line:
/// `basename|bank:preset,bank:preset,...`. Plain text on purpose: crossing a
/// vector of structs over the FFI for something consumed once at bake time is
/// not worth the complexity. Returns the bytes written.
size_t     ayther_instruments_soundfonts(const char* toml_text,
                                         uint8_t* out, size_t cap);

/// Bakes an SF2 trimmed to the requested `(bank, preset)` pairs. Returns the
/// bytes the result occupies; call with `cap = 0` to query the size.
///
/// It exists because a source SF2 can weigh hundreds of MB (a real collection
/// holds one of 988): nobody downloads that library to use 10-30 timbres — it
/// is DISTRIBUTION size (the pack already opens lazily, so residency stopped
/// being the reason). Measured: 0.3% of the original on a 97 MB file.
size_t     ayther_sf2_bake(const uint8_t* src, size_t src_len,
                           const uint16_t* banks, const uint16_t* presets,
                           uint32_t n, uint8_t* out, size_t cap);

AytherAudioEventDetector* ayther_audio_event_new();
void     ayther_audio_event_free(AytherAudioEventDetector* d);
/// Clock region for pitch decoding (0 = NTSC, 1 = PAL).
void     ayther_audio_event_set_pal(AytherAudioEventDetector* d, uint8_t pal);
/* Audio evidence for residual events: channels playing at the start of the
 * take (bits 0-5 FM, 6-9 PSG; the session measures it with a PCM probe). */
void     ayther_audio_event_set_initial_active(AytherAudioEventDetector* d, uint16_t mask);
void     ayther_audio_event_reset(AytherAudioEventDetector* d);
/// Ingest one frame's raw chip writes (bus order). `writes` is `n` AytherAudioWrite.
/// Equivalent to _process_frame_ex with no PCM events.
void     ayther_audio_event_process_frame(AytherAudioEventDetector* d, uint32_t frame,
                                          const AytherAudioWrite* writes, uint32_t n);

/// AytherPcmEvent::kind. Same values as ayther_audio_event_type_v1, so
/// translating from the core's event is the identity and cannot drift.
enum {
    AYTHER_PCM_KEY_ON  = 1,
    AYTHER_PCM_KEY_OFF = 2,
    AYTHER_PCM_PITCH   = 6,
    AYTHER_PCM_VOLUME  = 7
};

/// One typed event from the Sega CD RF5C164 () — the unpacked {reg, data}
/// of an `ayther_audio_event_v1` with source == PCM. This chip has no bus the
/// core exposes, so it never arrives as AytherAudioWrite.
///
/// `st`/`ls` locate the waveform in Wave RAM and are what says WHICH SAMPLE
/// plays; `fd` is playback rate and `env` volume, and neither identifies a
/// sound on its own. Layout-identical to audio_event::PcmEvent (10 bytes).
struct AytherPcmEvent {
    uint8_t  kind;     ///< AYTHER_PCM_*
    uint8_t  channel;  ///< 0-7
    uint8_t  env;      ///< envelope multiplier (volume)
    uint8_t  pan;
    uint8_t  st;       ///< ST register: Wave RAM start address
    uint8_t  _pad;
    uint16_t ls;       ///< loop address
    uint16_t fd;       ///< 5.11 address increment (rate, NOT a note)
};

/// Ingest one frame by BOTH audio paths: raw FM/PSG writes and typed PCM
/// events. One call per frame so the two share a frame number.
void     ayther_audio_event_process_frame_ex(AytherAudioEventDetector* d, uint32_t frame,
                                             const AytherAudioWrite* writes, uint32_t n,
                                             const AytherPcmEvent* pcm, uint32_t m);
void     ayther_audio_event_finish(AytherAudioEventDetector* d);
uint32_t ayther_audio_event_count(const AytherAudioEventDetector* d);
/// Fill out_buf with up to buf_cap events; returns the number written.
uint32_t ayther_audio_event_get(const AytherAudioEventDetector* d,
                                AytherAudioEvent* out_buf, uint32_t buf_cap);

// ---------------------------------------------------------------------------
// BatchEventDetector — events from PCM BATCH HASHES (C-A1, complementing the
// command-based detector above). It runs over the audio history of a take
// (.arp v7, one push PER FRAME: first hash, or 0 = silence) without needing the
// fork's write log. It emits the SAME AytherAudioEvent (chip = 255 → the mix,
// no channel). It does not separate overlapping sounds within the mix.
// ---------------------------------------------------------------------------

struct AytherBatchEventDetector;  // opaque — do not dereference

AytherBatchEventDetector* ayther_audio_evdet_new();
void     ayther_audio_evdet_free(AytherBatchEventDetector* d);
/// Toggle re-attack splitting (default on): a retrigger without silence (the
/// deterministic head reappears) splits the run into two instances.
void     ayther_audio_evdet_set_split_on_reattack(AytherBatchEventDetector* d, bool on);
/// Feed one batch hash (0 = silence) — once per frame of the take.
void     ayther_audio_evdet_push(AytherBatchEventDetector* d, uint64_t hash);
/// Close the run in flight (end of the take).
void     ayther_audio_evdet_flush(AytherBatchEventDetector* d);
uint32_t ayther_audio_evdet_event_count(const AytherBatchEventDetector* d);
/// Copies events into out_buf (up to cap); returns the total (grow and retry).
uint32_t ayther_audio_evdet_get_events(const AytherBatchEventDetector* d,
                                       AytherAudioEvent* out_buf, uint32_t cap);

/// A channel keyed on RIGHT NOW with its signature (LIVE substitution,
/// runtime). F3: instrument/pitch travel with the voice (captured at key-on) —
/// the runtime resolves per-instrument match rules without waiting for the
/// event to close. 24 bytes.
struct AytherAudioActive {
    uint64_t signature;
    uint64_t instrument; ///< fm_instrument/psg_instrument (0 = unknown)
    uint8_t  chip;       ///< 0 = FM, 1 = PSG
    uint8_t  channel;    ///< FM 0-5 | PSG 0-3
    uint8_t  pitch;      ///< MIDI note at key-on; 255 = no pitch
    uint8_t  _pad[5];
};
/// Channels active right now (out, up to cap); returns the count.
uint32_t ayther_audio_event_active(const AytherAudioEventDetector* d,
                                   AytherAudioActive* out_buf, uint32_t buf_cap);
/// Empties the closed events (live use; it does not touch the channel state).
void     ayther_audio_event_clear_events(AytherAudioEventDetector* d);

// ---------------------------------------------------------------------------
// audio_events.toml — catalogue of per-event substitutions (C-A5)
// signature→asset(+channels) persistence for saving/loading the project and for
// the .ay delivery.
// ---------------------------------------------------------------------------

struct AytherEventSub {
    uint64_t signature;
    char     asset[256];   ///< HD asset (logical path)
    uint32_t channels;     ///< channel mask (see ayther_chan_bit); 0 = re-derivable
    /// SEQUENCE (Mix): fields in the former _pad[6].
    uint8_t  looping;      ///< 1 = the HD loops until the window closes
    uint8_t  _pad;
    uint32_t duration_frames;  ///< window in frames (0 = classic per-event sub)
    /// F3: match rule — sizeof 288 (it was 272; both sides live in this repo).
    uint64_t match_instrument; ///< timbre identity of the rule (0 = no rule)
    uint8_t  match_rule;       ///< 0 exact (legacy) · 1 instrument · 2 instr+note
    uint8_t  match_pitch;      ///< MIDI note of rule 2 (255 = no pitch)
    /// The sound's bus — 0 unclassified · 1 music · 2 effects · 3 voices.
    ///
    /// It comes out of one of the padding bytes that were already there, so the
    /// layout does NOT change: an old binary reads 0 where it used to read
    /// padding, and 0 is exactly what "this pack did not say" means.
    uint8_t  bus;
    uint8_t  _pad2[5];
};

/// Formats `subs` (n entries) into audio_events.toml text in `out` (with a nul)
/// if it fits. Returns the length WITHOUT the nul; if > out_cap it writes
/// nothing (retry with a larger buffer).
uint32_t ayther_audio_events_format(const AytherEventSub* subs, uint32_t n,
                                    char* out, uint32_t out_cap);
/// Parses audio_events.toml text → out (up to cap). Returns the number
/// written.
uint32_t ayther_audio_events_parse(const char* text, AytherEventSub* out, uint32_t cap);

// ---------------------------------------------------------------------------
// AudioSubstitutor — hash-to-HD-asset mapping engine
//
// Two sources of substitution (same priority model as SpriteSubstitutor):
//   1. catalog  — loaded from audio_substitutions.toml at startup.
//   2. overrides — set per-frame by Lua ayther.audio.replace() (beats catalog).
//
// Ownership: caller creates with _new(), must release with _free().
// ---------------------------------------------------------------------------

struct AytherAudioSubstitutor;  // opaque — do not dereference

/// Allocate a new AudioSubstitutor.  Free with ayther_audio_sub_free().
AytherAudioSubstitutor* ayther_audio_sub_new();

/// Destroy an AudioSubstitutor.
void ayther_audio_sub_free(AytherAudioSubstitutor* s);

/// Load substitution catalog from audio_substitutions.toml inside the pack.
/// TOML schema (same [[sub]] convention as tiles/sprites):
///   [[sub]]
///   hash  = "0x0123456789abcdef"
///   asset = "audio/sfx/ring_pickup.wav"
void ayther_audio_sub_load_pack(AytherAudioSubstitutor* s, const AyArchive* pack);

/// Register a runtime override (e.g. from Lua ayther.audio.replace()).
void ayther_audio_sub_add_override(AytherAudioSubstitutor* s,
                                    uint64_t                hash,
                                    const char*             asset_path);

/// Clear all runtime overrides.  Call at the start of each tick.
void ayther_audio_sub_clear_overrides(AytherAudioSubstitutor* s);

/// Number of entries loaded from the TOML catalog.
uint32_t ayther_audio_sub_catalog_len(const AytherAudioSubstitutor* s);

/// Resolve audio occurrences to HD substitution instructions.
/// Returns the number of entries written to out_buf.
uint32_t ayther_audio_sub_resolve(const AytherAudioSubstitutor* s,
                                   const AytherAudioOccurrence*  occs,
                                   uint32_t                      occ_count,
                                   AytherAudioSub*               out_buf,
                                   uint32_t                      buf_cap);

// --- Audio event substitution  (C-A2) --------------------------------------

/// A resolved HD substitution for a whole audio event: mute the emulator over
/// [start_frame, end_frame] and play asset_path aligned to start_frame.
struct AytherAudioEventSub {
    uint64_t signature;
    uint64_t start_frame;
    uint64_t end_frame;
    char     asset_path[256];
    uint8_t  looping;
};

/// Bind an HD asset to an event signature (authoring assign; persists).
void ayther_audio_sub_add_event_override(AytherAudioSubstitutor* s,
                                          uint64_t signature,
                                          const char* asset_path,
                                          uint8_t looping);
void     ayther_audio_sub_clear_event_overrides(AytherAudioSubstitutor* s);
uint32_t ayther_audio_sub_event_catalog_len(const AytherAudioSubstitutor* s);
/// Resolve events (from ayther_audio_evdet_get_events) → substitutions.
uint32_t ayther_audio_sub_resolve_events(const AytherAudioSubstitutor* s,
                                          const AytherAudioEvent* events,
                                          uint32_t event_count,
                                          AytherAudioEventSub* out_buf,
                                          uint32_t buf_cap);

// ---------------------------------------------------------------------------
// Lua shader API  (v0.9.4)
// ---------------------------------------------------------------------------

/// Shader parameters written by `ayther.shader.set_param()` from Lua.
/// All values in [0, 1].  Default: crt_strength=0.0 (passthrough, no effect).
struct AytherShaderParams {
    float crt_strength;   ///< overall CRT effect weight
    float scan_strength;  ///< scanline darkness
    float vignette;       ///< edge-darkening intensity
};

/// Read shader parameters from the Lua script environment.
/// Writes one AytherShaderParams to *out.
/// Pass the result as push constants to VkPostProcess::apply().
void ayther_script_get_shader_params(const AytherScriptEnv* env,
                                      AytherShaderParams*    out);

// ---------------------------------------------------------------------------
// Fondos — BackgroundStitcher + ScrollUnwrapper  (core/src/background.rs)
//
// Reconstruct a scrolling plane into a full level strip (no single frame holds
// it: the nametable wraps). observe() each visible cell in LEVEL space; the
// caller turns on-screen cells into absolute level tiles using the VDP scroll,
// unwrapping the wrapped Hscroll via ScrollUnwrapper (game-agnostic). Validated
// against Sonic 2 EHZ in tools/background_spike (0 conflicts over 1033px of scroll).
// The engine-side consumer is BackgroundExporter (ayther_background_export.h).
// ---------------------------------------------------------------------------
struct AytherBgStitcher;  // opaque

AytherBgStitcher* ayther_bg_stitcher_new();
void   ayther_bg_stitcher_free(AytherBgStitcher* s);
/// Record one visible cell at absolute level-tile (lx, ly) on `plane` (0/1/2).
/// `cell` is the opaque code (nametable word & 0x7FFF: pattern|flips|palette).
void   ayther_bg_stitcher_observe(AytherBgStitcher* s, uint8_t plane,
                                  int32_t lx, int32_t ly, uint32_t cell);
size_t ayther_bg_stitcher_cell_count(const AytherBgStitcher* s, uint8_t plane);
size_t ayther_bg_stitcher_conflicts(const AytherBgStitcher* s, uint8_t plane);
size_t ayther_bg_stitcher_animated_cells(const AytherBgStitcher* s, uint8_t plane);
/// Fill out4 = {min_x, min_y, max_x, max_y} (level tiles). False if plane empty.
bool   ayther_bg_stitcher_bounds(const AytherBgStitcher* s, uint8_t plane, int32_t* out4);
/// Read the code at level cell (x, y) into *out_code. False if never seen.
bool   ayther_bg_stitcher_get(const AytherBgStitcher* s, uint8_t plane,
                              int32_t x, int32_t y, uint32_t* out_code);

struct AytherScrollUnwrapper;  // opaque — one per plane axis

AytherScrollUnwrapper* ayther_scroll_unwrapper_new(int32_t period);
void    ayther_scroll_unwrapper_free(AytherScrollUnwrapper* u);
/// Feed this frame's wrapped scroll ([0, period)); returns the absolute camera.
int64_t ayther_scroll_unwrapper_push(AytherScrollUnwrapper* u, int32_t wrapped);
/// Delta (px) of the last push — a non-physical |delta| (> ~32 px/frame) =
/// a scene cut; the caller freezes the stitch accumulation.
int32_t ayther_scroll_unwrapper_last_step(const AytherScrollUnwrapper* u);

// ---------------------------------------------------------------------------
// Game profile — Mode 3 (RAM anchoring) input plumbing  (core/src/game_profile.rs)
//
// A TOML anchor profile declares where entities live in game RAM (base + X/Y
// offsets + count/stride + match box). `entities()` reads their world positions
// (handling the 68k word-swap); `assign()` maps this frame's SAT sprites to them
// using the VDP camera scroll — the exact per-instance identity Mode 2's visual
// clustering can't produce for two identical entities. See Mode3Resolver
// (ayther_mode3.h) for the engine-side consumer.
//
// Ownership: caller creates with _load(), must release with _free().
// ---------------------------------------------------------------------------
struct AytherGameProfile;  // opaque — do not dereference

/// Load a profile from a TOML file. NULL on read/parse error.
AytherGameProfile* ayther_game_profile_load(const char* toml_path);
/// The pack case — game_profile.toml lives INSIDE the .ay and arrives as a
/// string, never as a file. NULL on a parse error.
AytherGameProfile* ayther_game_profile_load_str(const char* toml);
void     ayther_game_profile_free(AytherGameProfile* p);

/// Read the profile's active entities from `ram` into parallel arrays
/// (id / world_x / world_y) up to `cap`. Returns the number written.
size_t   ayther_game_profile_entities(const AytherGameProfile* p,
                                       const uint8_t* ram, size_t ram_size,
                                       uint64_t* out_id, int32_t* out_wx,
                                       int32_t* out_wy, size_t cap);

/// Assign this frame's SAT sprites (screen-space top-lefts) to the profile's
/// entities using the plane camera `scroll_x/y` and size `plane_w/h` (world→
/// screen wraps mod the plane). `out_ent_id[j]` = the id claiming sprite j, or 0.
/// Returns the number of active entities considered.
size_t   ayther_game_profile_assign(const AytherGameProfile* p,
                                     const uint8_t* ram, size_t ram_size,
                                     int32_t scroll_x, int32_t scroll_y,
                                     int32_t plane_w, int32_t plane_h,
                                     const int16_t* spr_x, const int16_t* spr_y,
                                     size_t n_spr, uint64_t* out_ent_id);

/// Anchor-kind introspection (map a resolved instance id → kind → HD asset).
size_t   ayther_game_profile_kind_count(const AytherGameProfile* p);
int32_t  ayther_game_profile_kind_of_id(const AytherGameProfile* p, uint64_t id);
/// Copy kind `idx`'s name into `buf` (NUL-terminated, up to `cap`); returns the
/// name's byte length (grow and retry if it exceeds cap-1), or 0.
size_t   ayther_game_profile_kind_name(const AytherGameProfile* p, size_t idx,
                                        char* buf, size_t cap);

#ifdef __cplusplus
}  // extern "C"
#endif
