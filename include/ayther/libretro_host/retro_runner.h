#pragma once
#include <string>
#include <cstdint>
#include <functional>
#include <vector>
#include <map>
#include "core_loader.h"
#include "libretro.h"
#include "ayther_api.h"   // Versioned AYTHER core-extension contract.

// Owns a Libretro core, loads a game ROM, and drives the emulation loop.
// Phase 1 (v0.1.0): headless — logic + RAM access.
// Phase 2 (v0.2.0): video_cb_ feeds the TileHasher sprite fingerprinter.
// Phase 3 (v0.3.0): video_cb_ also drives the Vulkan renderer.
//
// With the versioned AYTHER extension available, reads use a captured frame
// snapshot and are validated against its generation. Controls use bounded,
// frame-aware operations and return explicit status codes. Without that
// extension, only supported standard or deprecated read paths are available;
// render and audio controls must report unsupported behavior instead of
// silently writing through raw memory pointers. The authoritative extension
// identifiers and layouts live in ayther_api.h and must not be duplicated here.

/// @brief Owns one libretro core instance and drives its frame callbacks.
///
/// Memory pointers returned by accessors are borrowed from the core and become
/// invalid when the core unloads or changes the corresponding allocation. The
/// runner is not thread-safe. Its C callback bridge uses process-visible
/// dispatch state, so multiple runners must not execute concurrently.
class RetroRunner {
public:
    RetroRunner();
    ~RetroRunner();

    RetroRunner(const RetroRunner&)            = delete;
    RetroRunner& operator=(const RetroRunner&) = delete;

    // Load the core DLL and the ROM file. Returns false on any error.
    bool init(const std::string& core_path, const std::string& rom_path);

    // Advance emulation by exactly one hardware tick.
    void run_frame();

    void shutdown();

    // --- RAM access (read-only, Phase 1) ---
    // Returns pointer to the 64 KB 68000 work RAM, or nullptr if not running.
    const uint8_t* work_ram()      const { return static_cast<const uint8_t*>(ram_ptr_); }
    size_t         work_ram_size() const { return ram_size_; }

    // Returns the core-owned mutable work-RAM view. Writes are immediately
    // visible to the emulated CPU. AytherSession owns the policy that marks the
    // session modified and prevents invalid recording assumptions.
    uint8_t* work_ram_mut() { return static_cast<uint8_t*>(ram_ptr_); }

    // --- VRAM access (read-only, v0.8.0 — SpriteHasher) ---
    // Returns pointer to the core's VRAM (RETRO_MEMORY_VIDEO_RAM = 3), or nullptr.
    // For Genesis Plus GX this is typically 64 KB.
    // NOTE: the pointer is owned by the core and remains valid until shutdown().
    [[deprecated("Use read_vram_v1() with the versioned AYTHER extension")]]
    const uint8_t* video_ram() const {
        if (!fn_retro_get_memory_data) return nullptr;
        return static_cast<const uint8_t*>(fn_retro_get_memory_data(3 /*RETRO_MEMORY_VIDEO_RAM*/));
    }
    size_t video_ram_size() const {
        if (!fn_retro_get_memory_size) return 0;
        return fn_retro_get_memory_size(3 /*RETRO_MEMORY_VIDEO_RAM*/);
    }

    // --- CRAM access (read-only — the Mapper's multi-space map) -------------
    // PRIVATE id of the Ayther fork of GPX (there is no standard libretro id
    // for CRAM): 128 bytes = 64 nine-bit colours as host-endian uint16 in the
    // internal GPX layout (0000BBB0GGG0RRR0). Stock cores return null.
    static constexpr unsigned kAytherMemoryCram = 0x100;
    [[deprecated("E-5: use read_cram_v1() with ABI v1")]]
    const uint8_t* color_ram() const {
        if (!fn_retro_get_memory_data) return nullptr;
        return static_cast<const uint8_t*>(
            fn_retro_get_memory_data(kAytherMemoryCram));
    }
    size_t color_ram_size() const {
        if (!fn_retro_get_memory_size) return 0;
        return fn_retro_get_memory_size(kAytherMemoryCram);
    }

    // --- VDP registers (read-only — Mapper tilemap viewer, M9.3) ------------
    // PRIVATE id of the fork: the 32 VDP registers (reg[0x20]). The Lab derives
    // the plane name-table bases and the plane size from them.
    static constexpr unsigned kAytherMemoryVdpRegs = 0x101;
    [[deprecated("E-5: use read_vdp_regs_v1() with ABI v1")]]
    const uint8_t* vdp_regs() const {
        if (!fn_retro_get_memory_data) return nullptr;
        return static_cast<const uint8_t*>(
            fn_retro_get_memory_data(kAytherMemoryVdpRegs));
    }
    size_t vdp_regs_size() const {
        if (!fn_retro_get_memory_size) return 0;
        return fn_retro_get_memory_size(kAytherMemoryVdpRegs);
    }

    // --- VSRAM (read — vertical scroll) -------------------------------------
    // 128 bytes = 64 u16 entries (11 bits of vscroll). The hscroll lives in
    // VRAM; this was the only thing missing to resolve the on-screen position
    // of a plane tile (Phase 2c). Stock cores return null.
    static constexpr unsigned kAytherMemoryVsram = 0x107;
    [[deprecated("E-5: use read_vsram_v1() with ABI v1")]]
    const uint8_t* vsram() const {
        if (!fn_retro_get_memory_data) return nullptr;
        return static_cast<const uint8_t*>(
            fn_retro_get_memory_data(kAytherMemoryVsram));
    }
    size_t vsram_size() const {
        if (!fn_retro_get_memory_size) return 0;
        return fn_retro_get_memory_size(kAytherMemoryVsram);
    }

    // --- Z80 RAM (ABI 1.9) --------------------------------------------------
    //
    // The 8 KB the 68k sees at 0xA00000-0xA01FFF. The 68k and the Z80 split the
    // sound work, and several games leave the id of the track to play there —
    // not in work RAM.
    //
    // WHY THERE IS AN ACCESSOR FOR THIS. To record a clean track from the Sound
    // Test one has to find the slot where the 68k leaves the id. In Golden Axe
    // the work-RAM differential left two candidates and automatic confirmation
    // discarded both: the only hypothesis left was this RAM, and there was
    // nowhere to look.
    //
    // Stock cores return null, as with the rest of the fork's regions.
    static constexpr unsigned kAytherMemoryZ80Ram = 0x10F;
    const uint8_t* z80_ram() const {
        if (!fn_retro_get_memory_data) return nullptr;
        return static_cast<const uint8_t*>(
            fn_retro_get_memory_data(kAytherMemoryZ80Ram));
    }
    /// The same region, mutable. WRITING IT WHILE THE Z80 RUNS IS A RACE: it
    /// has to be done with the bus held, or one accepts that what is written
    /// may last a frame — which is enough to trigger a sound by id, and not
    /// enough for anything else.
    uint8_t* z80_ram_mut() {
        if (!fn_retro_get_memory_data) return nullptr;
        return static_cast<uint8_t*>(
            fn_retro_get_memory_data(kAytherMemoryZ80Ram));
    }
    size_t z80_ram_size() const {
        if (!fn_retro_get_memory_size) return 0;
        return fn_retro_get_memory_size(kAytherMemoryZ80Ram);
    }

    // --- Parsed sprites this frame (READ list 0x10B + WRITE-reset count 0x10C) ---
    // Sprite detection from what parse_satb ACTUALLY parsed: **10-byte**
    // entries — yr/xr/attr u16 + w/h u8 + sat_idx/chain_pos u8 (the fork's
    // `ayther_sprite_v1`), deduplicated. The real consumer
    // (`ayther_sprite_hasher_process_sprites`) uses that stride; this comment
    // used to say 8 and made anyone who believed it read misaligned. It is
    // robust to the game rewriting the SAT mid-frame / changing its base (the
    // genie in Aladdin's Sega logo), where reading the SAT at end of frame
    // shows only placeholders. Reset (count=0) before run_frame; read after.
    // No-op with a stock core → the FFI falls back to autodetect.
    // --- Arbitrary core export (probes/spikes) ------------------------------
    // Resolves a symbol exported by the module ACTUALLY loaded by THIS runner
    // (with multiple instances each runner loads ITS copy of the DLL, with its
    // own statics: resolving against another load would touch ANOTHER state).
    // nullptr with a core that does not export it (stock → clean
    // degradation).
    template <typename FnPtr>
    FnPtr core_sym(const char* name) const { return loader_.sym<FnPtr>(name); }

    // --- AYTHER ABI v1 (E-1) ------------------------------------------------
    // The fork's VERSIONED contract, negotiated when the DLL is loaded. Until
    // now the whole dialogue with the core went through
    // `retro_get_memory_data(0x100-0x10E)`: raw mutable pointers, with no
    // version, no validation, and no way to ask what the core on the other side
    // understands.
    //
    // The negotiation is OPTIONAL and does not take part in the success of the
    // load: a stock core does not export the symbol and that is perfectly valid
    // — the legacy path is followed. `has_ayther_v1()` is the gate callers use
    // to pick one path or the other (E-3/E-4).

    /// True if the core exports ABI v1 and the negotiation succeeded.
    bool has_ayther_v1() const { return ayther_api_ != nullptr; }

    /// The loaded medium is a DISC image (.iso/.cue/.chd) — i.e. Sega CD, the
    /// only hardware with chips the voice router does not mirror: the RF5C164
    /// PCM and the CDDA. It is resolved from the extension at init, before the
    /// first frame, and not from whatever the game happens to touch.
    bool cd_media() const { return cd_media_; }

    /// The negotiated descriptor; nullptr if `!has_ayther_v1()`. Owned by the
    /// core: valid until `shutdown()` and only touched from the emulation
    /// thread.
    const ayther_interface_v1* ayther_api() const { return ayther_api_; }

    /// The ABI version the core declares (0 without the ABI). Major/minor via
    /// `AYTHER_ABI_VERSION_MAJOR/MINOR`. The rule is major == 1 and minor >=
    /// whatever is needed, never `==`: the ABI is additive (guide 1.9 §3).
    uint32_t ayther_abi_version() const { return ayther_api_ ? ayther_api_->abi_version : 0u; }

    /// The subscriptions this Engine CONSUMES — and only those (ABI 1.9, guide
    /// §4: "ask only for what will be read"). Up to ABI 1.3 this was
    /// `AYTHER_SUB_ALL` and it made no difference, because the seven bits were
    /// the seven that were read. Since 1.9 `AYTHER_SUB_ALL` is 0xFFF and
    /// includes ATTRIBUTION (one byte per pixel), LINE_STATE / LINE_CRAM /
    /// LINE_CELLS and FRAME_HASH (~100 KB walked per frame): every active bit
    /// moves the core renderer to the observed clone and pays for what it
    /// captures, and nobody in the Engine reads those regions yet. When the
    /// consumer appears (per-tile anchoring, the Lab's FRAME_HASH) ITS bit gets
    /// added here, alongside whoever reads it.
    static constexpr uint32_t kEngineSubscriptions =
        AYTHER_SUB_VDP_MEMORY | AYTHER_SUB_SPRITE_CAPTURE | AYTHER_SUB_RENDER_CONTROLS |
        AYTHER_SUB_RASTER_TRACKING | AYTHER_SUB_AUDIO_WRITES | AYTHER_SUB_RECOMPOSITION |
        AYTHER_SUB_AUDIO_EVENTS;

    /// Bits of `fallback_reasons` (snapshot) / 0x10E (legacy). The 1.9 header
    /// does not name them; the integration guide §5.8 does, and that is where
    /// they come from. `> 0` still means "fallback" to everyone; these two
    /// deserve distinguishing: OVERFLOW = the journal passed 256 events and
    /// `recompose_multilayer` returns RC_JOURNAL_OVERFLOW instead of a
    /// plausible prefix; UNSUPPORTED_CONTROLS = a control we asked for does not
    /// apply in this mode.
    static constexpr uint32_t kRasterReasonJournalOverflow     = UINT32_C(1) << 7;
    static constexpr uint32_t kRasterReasonUnsupportedControls = UINT32_C(1) << 8;

    /// Asks for what the Engine consumes (`kEngineSubscriptions`), bounded by
    /// what the core supports. Returns the enabled mask (0 with a core without
    /// the ABI, which is the legacy path and needs no request).
    ///
    /// The fork instruments NOTHING until somebody declares it: without this
    /// the chip write log comes back empty, the audio probe emits nothing and
    /// the mute mask is ignored SILENTLY. A consumer that does not know this
    /// does not see an error: it sees zeros, which is exactly what an oracle
    /// confuses with "this is silent as expected" (2026-08-13: four tools were
    /// measuring a mute core and reporting it as a result).
    ///
    /// It is idempotent and overrides nothing. The ABI tests (`abi_*`) ask by
    /// hand on purpose — there the subscription IS what is being tested.
    uint32_t subscribe_all_supported() {
        if (!ayther_api_ || !(ayther_api_->capabilities & AYTHER_CAP_SUBSCRIPTIONS_V1))
            return 0;
        ayther_subscription_state_v1 st{};
        st.struct_size = sizeof(st);
        if (ayther_api_->get_subscriptions(&st, sizeof(st)) != AYTHER_STATUS_OK)
            return 0;
        const uint32_t want = kEngineSubscriptions & st.supported_mask;
        return ayther_api_->set_subscriptions(want) == AYTHER_STATUS_OK ? want : 0;
    }


    // --- Reads through ABI v1 (E-3) -----------------------------------------
    // Parallel to the legacy accessors, which remain untouched: the caller
    // picks with `has_ayther_v1()`. The fundamental difference from the old
    // path is that here the read VALIDATES — there is a snapshot generation
    // that says whether what was read belongs to the frame one believes, and a
    // subscription state that distinguishes "there is no data" from "nobody
    // asked for it". With the raw pointers both situations look the same:
    // memory with something in it.
    struct AytherReadResult {
        int32_t  status     = AYTHER_STATUS_UNSUPPORTED;
        uint32_t count      = 0;   ///< elements read
        uint64_t generation = 0;   ///< generation the core returned
        bool ok() const { return status == AYTHER_STATUS_OK; }
    };

    /// Snapshot of the CURRENT frame. Call it after `run_frame()`: the ABI
    /// resets its counters at the frame boundary, so this snapshot is what
    /// replaces the manual `reset_*()` calls of the legacy path.
    AytherReadResult capture_frame_snapshot(ayther_frame_snapshot_v1& out) const;

    /// `SYSTEM` (ABI 1.5): VDP mode (4/5), h40, interlace, S/H, PAL and the
    /// viewport of the emitted frame with its offset (Game Gear = 160×144 at
    /// (48,24)). No subscription; it is filled on read. It is the source of
    /// truth for the mode instead of decoding registers: that decoding was
    /// already fixed once in the core and the Engine's copy never found out
    /// (guide §5.1). Returns UNSUPPORTED without the capability.
    AytherReadResult read_system_v1(ayther_system_v1& out) const;

    /// A whole region into the caller's buffer, validating the generation.
    AytherReadResult read_region_v1(uint32_t region, void* out, uint32_t bytes,
                                    uint64_t generation) const;

    // VDP (AYTHER_SUB_VDP_MEMORY). `out` must hold the byte_size of the region.
    /// How many bytes the ABI says a region measures (0 if there is no ABI or
    /// it does not know it). The VDP `read_*_v1` calls write THAT size, not the
    /// one `retro_get_memory_size` reports: whoever sizes the buffer with the
    /// legacy number is betting that the two sources agree. Today they do; this
    /// exists so the caller does not have to bet.
    size_t abi_region_bytes(uint32_t region) const;

    AytherReadResult read_vram_v1    (void* out, const ayther_frame_snapshot_v1& s) const;
    AytherReadResult read_cram_v1    (void* out, const ayther_frame_snapshot_v1& s) const;
    AytherReadResult read_vdp_regs_v1(void* out, const ayther_frame_snapshot_v1& s) const;
    AytherReadResult read_vsram_v1   (void* out, const ayther_frame_snapshot_v1& s) const;

    /// Parsed sprites (AYTHER_SUB_SPRITE_CAPTURE), with FORWARD COMPATIBILITY:
    /// if the core declares an `element_size` larger than the struct this
    /// Engine knows, it reads into a temporary and copies only the known
    /// fields. Without that, a newer core would misalign the whole read — which
    /// is exactly what was happening by coincidence with the legacy pointer
    /// (8 bytes read from a layout that today has 10).
    AytherReadResult read_parsed_sprites_v1(ayther_sprite_v1* out, uint32_t max,
                                            const ayther_frame_snapshot_v1& s) const;

    /// The frame's chip writes (AYTHER_SUB_AUDIO_WRITES).
    AytherReadResult read_audio_writes_v1(ayther_audio_write_v1* out, uint32_t max,
                                          const ayther_frame_snapshot_v1& s) const;

    /// Raster fallback reasons (AYTHER_SUB_RASTER_TRACKING). The snapshot
    /// already carries them; 0 if there is no ABI or no subscription.
    uint32_t read_raster_fallback_v1(const ayther_frame_snapshot_v1& s) const;

    // --- Frame Delta Stream (E-6) -------------------------------------------
    // What got dirtied in the frame, told by the core instead of deduced: one
    // byte per *pattern name* (32 bytes of VRAM each) plus the frame counters,
    // including `raster_event_count` — the size of the raster event journal,
    // which is the only value here the snapshot does NOT carry and the input to
    // the multi-layer replay.
    //
    // It refreshes itself, inside `run_frame()`. See `poll_frame_delta_()` for
    // why the poll lives there and not in the caller.
    //
    // WHAT THIS DOES **NOT** ENABLE, worth knowing before attempting it: it is
    // no use for dropping the full VRAM read in `refresh_abi_mirror()`. Not
    // because of the data —the bitmask is a faithful superset of what changed,
    // verified frame by frame in `abi_frame_delta`— but because there is
    // nothing to gain: reading the 64 KiB through the ABI measures **0.002
    // ms/frame**, 0.01% of a 16.6 ms frame. Selective invalidation would trade
    // a linear read for a walk over 2048 bytes and N `read_region` calls, to
    // save two microseconds and add incremental state that has to be
    // invalidated correctly on every reset, unserialize and core change.
    bool has_frame_delta() const { return last_delta_ok_; }
    /// Valid only with `has_frame_delta()`; left over from the last
    /// `run_frame()`.
    const ayther_frame_delta_v1& frame_delta() const { return last_delta_; }

    // --- Typed audio events -------------------------------------------------
    // The SECOND audio path, and it exists for a concrete reason: the Sega CD
    // PCM chip has no exposed bus, so `read_audio_writes_v1` —which carries raw
    // FM and PSG writes— does not carry it and cannot. This path brings
    // ALREADY-TYPED events (key-on/off, volume, pitch).
    //
    // Which one governs which chip, in writing: the raw writes are the source
    // for FM and PSG; the events, for the PCM. The IDENTITY of a sound is
    // decided by neither — it is computed by the detector
    // (`core/src/audio_event.rs`) for all three chips alike.
    //
    // It is CONSUME-ON-POLL over an SPSC queue: what is read disappears, so
    // there is a single consumer and it calls once per frame. `event_size`
    // comes from the core and not from a local `sizeof` — the struct became a
    // union and changed size once already.
    //
    // Returns the number of events written into `out` (0 without the ABI,
    // without a subscription, or with nothing pending).
    uint32_t poll_audio_events_v1(ayther_audio_event_v1* out, uint32_t max) const;
    /// Events the transport dropped for lack of polling, accumulated.
    uint32_t audio_events_dropped() const;

    // --- Control writes through ABI v1 (E-4) --------------------------------
    // The legacy writes are `*p = value` over core memory: they do not validate
    // bounds, do not signal the generation change (the snapshot system goes out
    // of sync) and nothing stops them happening in the middle of `retro_run`.
    // `write_control` validates all three and returns the new generation.
    struct AytherWriteResult {
        int32_t  status         = AYTHER_STATUS_UNSUPPORTED;
        uint64_t new_generation = 0;
        bool ok() const { return status == AYTHER_STATUS_OK; }
    };

    /// A write into a CONTROL region, between frames.
    /// `AYTHER_GENERATION_ANY` skips generation validation, which is the right
    /// thing for a control set outside an active snapshot.
    AytherWriteResult write_control_v1(
        uint32_t region, const void* data, uint32_t bytes,
        uint64_t expected_generation = AYTHER_GENERATION_ANY) const;

    // ---- Render and audio controls (AYTHER_SUB_RENDER_CONTROLS) ------------
    // The sizes are part of the CONTRACT of each control, not a caller detail:
    // they live here so nobody repeats them by hand.
    static constexpr uint32_t kSpriteSuppressBytes    = 16;        // 128 SAT slots
    static constexpr uint32_t kTileSuppressBytes      = 512;       // 64×64 cells
    static constexpr uint32_t kPlaneTileSuppressBytes = 3 * 1024;  // 3 planes

    /// Visible layer mask: 1 byte with the A/B/Window/Sprites bits. The
    /// renderer reads it PER LINE, so it hides or shows layers live.
    AytherWriteResult set_layer_mask_v1(uint8_t mask) const {
        return write_control_v1(AYTHER_REGION_LAYER_MASK, &mask, 1);
    }
    /// 0 = normal render (bit-exact). !=0 = the pixels that are NOT sprite are
    /// emitted at 25%, so sprites stand out (Lab Animation).
    AytherWriteResult set_layer_dim_v1(uint8_t on) const {
        return write_control_v1(AYTHER_REGION_LAYER_DIM, &on, 1);
    }
    /// A 128-bit bitmask: SAT slots `parse_satb` will skip.
    AytherWriteResult set_sprite_suppress_v1(const uint8_t* bits16) const {
        return write_control_v1(AYTHER_REGION_SPRITE_SUPPRESS,
                                bits16, kSpriteSuppressBytes);
    }
    /// 8px output cells (64×64, stride 64) painted with the backdrop, revealing
    /// the VDP background. Applied by `render_line`.
    AytherWriteResult set_tile_suppress_v1(const uint8_t* bits, uint32_t n) const {
        return write_control_v1(AYTHER_REGION_TILE_SUPPRESS, bits, n);
    }
    /// 3 planes × a bitmap of (pattern<<2 | palette): `render_bg_m5/_vs` skip
    /// those cells and reveal the plane behind. The fork used to have a
    /// separate `active` flag (0x106) without which this was a SILENT no-op;
    /// through the ABI the core manages it itself — measured in
    /// `abi_write_control`.
    AytherWriteResult set_plane_tile_suppress_v1(const uint8_t* bits, uint32_t n) const {
        return write_control_v1(AYTHER_REGION_PLANE_TILE_SUPPRESS, bits, n);
    }
    /// Channels to silence. The channel is set to 0 in the OUTPUT mixer
    /// without touching the chip state: replay-safe, the chip evolves
    /// identically and only the emitted PCM changes — the hasher has to keep
    /// seeing it. It is the primitive of per-event substitution: muting the
    /// channels of an event while its HD asset plays. 0 = everything plays.
    static constexpr uint32_t audio_mute_fm(int ch)  { return uint32_t(1u << ch); }        // ch 0-5
    static constexpr uint32_t audio_mute_psg(int ch) { return uint32_t(1u << (6 + ch)); }  // ch 0-3
    static constexpr uint32_t audio_mute_pcm(int ch) { return uint32_t(1u << (10 + ch)); } // ch 0-7
    AytherWriteResult set_audio_mute_v1(uint32_t mask) const {
        return write_control_v1(AYTHER_REGION_AUDIO_MUTE, &mask, sizeof(mask));
    }

    // --- Per-frame fidelity signal (R-5, id 0x10E) --------------------------
    // A WRITABLE u32: writes with a visual effect mid-screen (CRAM/VSRAM/the
    // hscroll table/regs). The frontend resets it before the visible frame and
    // reads it after; >0 = the frame is NOT faithfully recomposed from the
    // final state (R-1) → that frame falls back to the blit (hybrid). 0 with a
    // stock core.
    static constexpr unsigned kAytherMemoryRasterDirty = 0x10E;
    [[deprecated("E-5: use read_raster_fallback_v1() with ABI v1")]]
    uint32_t raster_dirty() const {
        if (!fn_retro_get_memory_data) return 0;
        const auto* p = static_cast<const uint32_t*>(
            fn_retro_get_memory_data(kAytherMemoryRasterDirty));
        return p ? *p : 0;
    }

    static constexpr unsigned kAytherMemoryParsedSprites = 0x10B;
    static constexpr unsigned kAytherMemoryParsedCount   = 0x10C;
    [[deprecated("E-5: use read_parsed_sprites_v1() with ABI v1")]]
    const uint8_t* parsed_sprites() const {
        if (!fn_retro_get_memory_data) return nullptr;
        return static_cast<const uint8_t*>(
            fn_retro_get_memory_data(kAytherMemoryParsedSprites));
    }
    [[deprecated("E-5: use capture_frame_snapshot() with ABI v1")]]
    uint8_t parsed_sprite_count() const {
        if (!fn_retro_get_memory_data) return 0;
        const auto* p = static_cast<const uint8_t*>(
            fn_retro_get_memory_data(kAytherMemoryParsedCount));
        return p ? *p : 0;
    }

    // --- Audio chip writes this frame (READ log 0x109 + READ/WRITE-reset count 0x10A) ---
    // Temporal log of the RAW writes to the sound chips — YM2612 (FM) + SN76489
    // (PSG) — in bus order within the frame. Each AudioWrite = {cycle, addr, data,
    // chip} (8 bytes, ABI-identical to the fork's AytherAudioWrite). It is the
    // basis of audio identity by COMMAND SEQUENCE to the chip (stable across
    // replay, because the CPU/VDP are byte-deterministic) instead of hashing the
    // output PCM, which is NOT reproducible after unserialize (the FM phase
    // diverges). The same frontend-reset pattern as parsed_sprites: reset
    // (count=0) before run_frame, read after. No-op with a stock core (a clean
    // degradation → the FFI falls back to the PCM).
    struct AudioWrite { uint32_t cycle; uint16_t addr; uint8_t data; uint8_t chip; };
    static constexpr unsigned kAytherMemoryAudioWrites = 0x109;
    static constexpr unsigned kAytherMemoryAudioCount  = 0x10A;
    static constexpr uint8_t  kAudioChipFM  = 0;   // YM2612
    static constexpr uint8_t  kAudioChipPSG = 1;   // SN76489
    [[deprecated("E-5: use read_audio_writes_v1() with ABI v1")]]
    const AudioWrite* audio_writes() const {
        if (!fn_retro_get_memory_data) return nullptr;
        return static_cast<const AudioWrite*>(
            fn_retro_get_memory_data(kAytherMemoryAudioWrites));
    }
    [[deprecated("E-5: use capture_frame_snapshot() with ABI v1")]]
    uint32_t audio_write_count() const {
        if (!fn_retro_get_memory_data) return 0;
        const auto* p = static_cast<const uint32_t*>(
            fn_retro_get_memory_data(kAytherMemoryAudioCount));
        return p ? *p : 0;
    }

    // --- Cheats (the Lab's advanced mode: GG/PAR codes via the core) --------
    void cheat_set(unsigned index, bool enabled, const char* code) {
        if (fn_retro_cheat_set) fn_retro_cheat_set(index, enabled, code);
    }
    void cheat_reset() {
        if (fn_retro_cheat_reset) fn_retro_cheat_reset();
    }

    bool is_running() const { return running_; }

    // Frames-per-second reported by the core via retro_system_av_info.timing.fps.
    // Available after init(). Returns 60.0 before the ROM is loaded.
    double fps() const { return fps_; }

    // Pixel format set by the core (RETRO_PIXEL_FORMAT_*).
    // 0 = 0RGB1555 (legacy), 1 = XRGB8888, 2 = RGB565 (Genesis Plus GX default).
    unsigned pixel_format() const { return pixel_format_; }

    // Optional video callback (wired to TileHasher in Phase 2, Vulkan in Phase 3).
    using VideoCb = std::function<void(const void*, unsigned, unsigned, size_t)>;
    void set_video_callback(VideoCb cb) { video_cb_ = std::move(cb); }

    // Optional audio callback wired to AudioHasher.
    // Signature mirrors retro_audio_sample_batch_t: returns frames consumed.
    using AudioCb = std::function<size_t(const int16_t*, size_t)>;
    void set_audio_callback(AudioCb cb) { audio_cb_ = std::move(cb); }

    // ----- Savestates (R2 base · emulator conventions) ----------------------
    // Genesis Plus GX ≈ 150–250 KB/state. Used by the determinism spike and,
    // later, by rewind/.arp recordings.

    /// Size in bytes of a serialized state, or 0 if unsupported.
    size_t serialize_size() const {
        if (!fn_retro_serialize_size) return 0;
        CallbackScope callback_scope{*const_cast<RetroRunner*>(this)};
        return fn_retro_serialize_size();
    }
    /// Capture the current state into `out`. Returns false on failure.
    /// CallbackScope binds this operation to the current thread; a nested
    /// shadow-core call restores this runner when it returns.
    bool serialize(std::vector<uint8_t>& out) const {
        const size_t n = serialize_size();
        if (!fn_retro_serialize || n == 0) return false;
        CallbackScope callback_scope{*const_cast<RetroRunner*>(this)};
        out.resize(n);
        return fn_retro_serialize(out.data(), out.size());
    }
    /// Restore a state captured by serialize(). Returns false on failure.
    bool unserialize(const std::vector<uint8_t>& data) {
        if (!fn_retro_unserialize || data.empty()) return false;
        CallbackScope callback_scope{*this};
        return fn_retro_unserialize(data.data(), data.size());
    }
    /// Soft reset (retro_reset).
    void reset() {
        if (!fn_retro_reset) return;
        CallbackScope callback_scope{*this};
        fn_retro_reset();
    }

    // ----- Input injection (R2 base) ----------------------------------------
    // Per-port button bitfield read by s_input_state. Bit i = RETRO_DEVICE_ID_
    // JOYPAD_* id i (B=0, Y=1, SELECT=2, START=3, UP=4, DOWN=5, LEFT=6, RIGHT=7,
    // A=8, X=9, L=10, R=11). hash(0) = no buttons (today's behaviour).

    void     set_input(int port, uint16_t buttons) { if (port >= 0 && port < kPorts) input_[port] = buttons; }
    uint16_t input(int port) const { return (port >= 0 && port < kPorts) ? input_[port] : 0; }

    // ----- Core options (EM-7.1) --------------------------------------------
    //
    // libretro calls them "variables": key/value pairs the core declares at
    // startup (SET_VARIABLES) and queries when it needs them (GET_VARIABLE).
    // They are what give "no sprite limit" —the anti-flicker— and the
    // overclock where the core offers it.
    //
    // UNTIL NOW THEY WERE NOT SUPPORTED: `GET_VARIABLE` always returned false
    // and the core fell back to its internal defaults. The plumbing that was
    // assumed to exist was the hook, not the answer.
    //
    // THEY ARE APPLIED AT INITIALISATION AND DO NOT CHANGE LIVE, on purpose.
    // The core reads its options exactly once because `GET_VARIABLE_UPDATE`
    // answers "they did not change": answering "yes" there made Genesis Plus GX
    // re-apply the options every frame and reinitialise the sound chip — mute
    // audio, a defect already paid for once. Changing an option requires
    // restarting the session, and that is the honest answer: the alternative is
    // a half-applied state nobody can explain.

    /// Sets the value of an option BEFORE `init`. After `init` it is stored but
    /// the core has already read its own.
    void set_core_option(const std::string& key, const std::string& value) {
        core_options_[key] = value;
    }
    /// The chosen value, or "" if the frontend set none (and then the core
    /// default governs).
    std::string core_option(const std::string& key) const {
        const auto it = core_options_.find(key);
        return it == core_options_.end() ? std::string() : it->second;
    }
    void clear_core_options() { core_options_.clear(); }

    /// EM-7.4: the user's IPS/BPS patch. It is applied to the ROM buffer
    /// —never to the file— before handing it to the core, so it has to be set
    /// BEFORE `init`. Empty = none.
    void set_patch_path(const std::string& p) { patch_path_ = p; }

    /// The options the CORE declared, in order: `(key, description)`. It comes
    /// from `SET_VARIABLES`, so it is populated after `init` and it is what a
    /// frontend needs to offer them without hard-coding any core's list (BYOC:
    /// we do not know which one they will use).
    const std::vector<std::pair<std::string, std::string>>& declared_options() const {
        return declared_options_;
    }

private:
    /// Binds synchronous libretro C callbacks to the runner executing on this
    /// thread. Nested calls restore the previous target, so callback dispatch
    /// is reentrant instead of relying on process-global mutable state.
    class CallbackScope {
    public:
        explicit CallbackScope(RetroRunner& runner) noexcept;
        ~CallbackScope() noexcept;

        CallbackScope(const CallbackScope&) = delete;
        CallbackScope& operator=(const CallbackScope&) = delete;

    private:
        RetroRunner* previous_ = nullptr;
    };

    static constexpr int kPorts = 2;
    uint16_t input_[kPorts] = { 0, 0 };

    // ----- Function pointers loaded from the core DLL -----
    void (*fn_retro_set_environment)(retro_environment_t)            = nullptr;
    void (*fn_retro_set_video_refresh)(retro_video_refresh_t)        = nullptr;
    void (*fn_retro_set_audio_sample)(retro_audio_sample_t)          = nullptr;
    void (*fn_retro_set_audio_sample_batch)(retro_audio_sample_batch_t) = nullptr;
    void (*fn_retro_set_input_poll)(retro_input_poll_t)              = nullptr;
    void (*fn_retro_set_input_state)(retro_input_state_t)            = nullptr;
    void (*fn_retro_init)()                                          = nullptr;
    void (*fn_retro_deinit)()                                        = nullptr;
    bool (*fn_retro_load_game)(const retro_game_info*)               = nullptr;
    void (*fn_retro_unload_game)()                                   = nullptr;
    void (*fn_retro_run)()                                           = nullptr;
    void*  (*fn_retro_get_memory_data)(unsigned)                     = nullptr;
    void   (*fn_retro_cheat_set)(unsigned, bool, const char*)        = nullptr;
    void   (*fn_retro_cheat_reset)()                                 = nullptr;
    size_t (*fn_retro_get_memory_size)(unsigned)                     = nullptr;
    void (*fn_retro_get_system_info)(retro_system_info*)             = nullptr;
    void (*fn_retro_get_system_av_info)(retro_system_av_info*)       = nullptr;
    // Savestates + reset (R2 base)
    size_t (*fn_retro_serialize_size)()                              = nullptr;
    bool (*fn_retro_serialize)(void*, size_t)                        = nullptr;
    bool (*fn_retro_unserialize)(const void*, size_t)               = nullptr;
    void (*fn_retro_reset)()                                         = nullptr;

    // E-1: the fork's ABI v1, resolved in load_symbols(). Absent on a stock
    // core — see has_ayther_v1() above.
    /// Directory the core receives as its SYSTEM/SAVE/CONTENT directory. It is
    /// the ROM's, and not a "." (the process CWD, which changes with whoever
    /// invokes it): that is where the core looks for the BIOS images
    /// —`bios_CD_U.bin` and friends for Sega CD, `bios_MD.bin`, `ggenie.bin`—
    /// and where the core itself falls back when the frontend does not answer.
    std::string system_dir_ = ".";
    std::string patch_path_;   ///< EM-7.4: the user's patch (IPS/BPS)
    /// EM-7.1: what the frontend chose, and what the core declares it offers.
    std::map<std::string, std::string> core_options_;
    std::vector<std::pair<std::string, std::string>> declared_options_;

    /// The loaded medium is a DISC image (see cd_media()).
    bool cd_media_ = false;

    ayther_get_interface_fn    fn_ayther_get_interface_ = nullptr;
    const ayther_interface_v1* ayther_api_              = nullptr;

    // E-6: the delta of the last `run_frame()`, and whether it is usable.
    ayther_frame_delta_v1 last_delta_{};
    bool                  last_delta_ok_ = false;
    void poll_frame_delta_();

    /// Common body of the four VDP reads: the size is declared by the core
    /// (query_region), not by the Engine.
    AytherReadResult read_vdp_region_(uint32_t region, void* out,
                                      const ayther_frame_snapshot_v1& s) const;

    CoreLoader loader_;
    VideoCb    video_cb_;
    AudioCb    audio_cb_;

    void*    ram_ptr_      = nullptr;
    size_t   ram_size_     = 0;
    double   fps_          = 60.0; // filled by load_rom() from retro_system_av_info
    unsigned pixel_format_ = 2;   // default: RGB565 (Genesis Plus GX)
    bool     running_      = false;

    // ----- Static trampolines (libretro needs plain C callbacks) -----
    static thread_local RetroRunner* s_active_instance_;

    static bool   s_environment(unsigned cmd, void* data);
    static void   s_video_refresh(const void* data, unsigned w, unsigned h, size_t pitch);
    static void   s_audio_sample(int16_t left, int16_t right);
    static size_t s_audio_sample_batch(const int16_t* data, size_t frames);
    static void   s_input_poll();
    static int16_t s_input_state(unsigned port, unsigned device, unsigned index, unsigned id);

    bool load_symbols();
    bool load_rom(const std::string& rom_path);
};
