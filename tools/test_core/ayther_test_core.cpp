// ---------------------------------------------------------------------------
// ayther_test_core — a deterministic libretro core that speaks the AYTHER ABI.
//
// WHY THIS EXISTS. `abi_negociacion` measures the NEGOTIATION, and the positive
// half of it needed a core that exports `ayther_get_interface`. The only such
// core lived in another repository and was distributed as a 10 MB binary that
// `/third_party/cores/*` gitignores, so on any clean checkout -- CI included --
// the oracle returned 77 and the negotiation went unmeasured. A skipped test
// that nobody can un-skip is not coverage, it is a note.
//
// This core is small, this repository owns it, and it is built from source on
// every platform, so the positive case runs everywhere. It emulates nothing:
// it produces a deterministic function of (ROM bytes, frame number, input),
// which is exactly what an oracle for the ABI and for end-to-end determinism
// needs. Same inputs, same frames, same audio, same RAM -- on Windows and on
// Linux, because there is no hardware in the loop to disagree.
//
// It is built twice from this one source:
//
//   ayther_test_core        — exports ayther_get_interface (the positive case)
//   ayther_test_core_stock  — does not (AYTHER_TEST_CORE_NO_ABI)
//
// so both halves of "the negotiation is additive" are exercised by cores this
// repository can actually ship.
// ---------------------------------------------------------------------------
#include "libretro_host/ayther_api.h"
#include "libretro_host/libretro.h"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>

namespace {

// ---- Geometry -------------------------------------------------------------
// H32/V24, which is a real Mega Drive mode and keeps the frame small enough to
// hash quickly. h40 must stay 0 while viewport_w is 256: the SYSTEM oracle
// checks exactly that the two never contradict each other.
constexpr unsigned kWidth  = 256;
constexpr unsigned kHeight = 192;
constexpr double   kFps = 60.0;
constexpr double   kSampleRate = 44100.0;
constexpr unsigned kSamplesPerFrame = 735;        // 44100 / 60, exactly

constexpr size_t kWorkRamBytes = 64 * 1024;
constexpr size_t kVramBytes    = 64 * 1024;
constexpr size_t kZ80RamBytes  = 8 * 1024;
constexpr size_t kCramBytes    = 128;
constexpr size_t kVsramBytes   = 80;
constexpr size_t kVdpRegBytes  = 32;

/// Bytes per parsed-sprite entry on the legacy pointer AND in the ABI region.
/// The E-5 oracle memcmps `count * 10` bytes of one against the other, so the
/// two must be the same buffer, not two views that agree by construction.
constexpr unsigned kSpriteEntryBytes = 10;
constexpr uint8_t  kSpritesPerFrame  = 12;

/// Chip writes reported per frame. Enough that the read-parity oracle has
/// something to compare instead of warning that its checks were vacuous.
constexpr uint8_t  kAudioWritesPerFrame = 16;

/// VRAM is addressed as 32-byte patterns, so 64 KiB is 2048 of them -- the
/// exact width of ayther_frame_delta_v1::dirty_patterns.
constexpr unsigned kPatternBytes = 32;
constexpr unsigned kPatternCount = kVramBytes / kPatternBytes;


// ---- libretro callbacks ---------------------------------------------------

retro_environment_t   g_environ = nullptr;
retro_video_refresh_t g_video   = nullptr;
retro_audio_sample_t  g_sample  = nullptr;
retro_audio_sample_batch_t g_batch = nullptr;
retro_input_poll_t    g_poll    = nullptr;
retro_input_state_t   g_input   = nullptr;

// ---- Core state -----------------------------------------------------------
//
// Everything that serialize() must capture lives in one struct so a savestate
// is a memcpy and cannot silently miss a field.

struct SerializedState {
    uint64_t frame;
    uint64_t lcg;
    uint16_t last_input;
    uint8_t  sprite_count;
    uint8_t  padding[5];
    uint8_t  work_ram[kWorkRamBytes];
    uint8_t  vram[kVramBytes];
    uint8_t  z80_ram[kZ80RamBytes];
    uint8_t  cram[kCramBytes];
    uint8_t  vsram[kVsramBytes];
    uint8_t  vdp_regs[kVdpRegBytes];
    uint8_t  sprites[kSpritesPerFrame * kSpriteEntryBytes];
    uint8_t  audio_write_count;
    uint8_t  layer_mask;
    uint8_t  layer_dim;
    uint8_t  padding2[5];
    uint32_t audio_mute_mask;
    uint32_t control_generation;
    ayther_audio_write_v1 audio_writes[kAudioWritesPerFrame];
    /// One byte per 32-byte VRAM pattern, set for every pattern this frame
    /// touched. A consumer uses it to skip re-uploading tiles that did not
    /// change, so a FALSE NEGATIVE here shows up as a stale tile on screen --
    /// which is why the oracle checks for those and not for false positives.
    uint8_t  dirty_patterns[kPatternCount];
    uint32_t raster_event_count;
};

SerializedState g_state{};

std::vector<uint8_t> g_rom;
uint32_t g_rom_crc32 = 0;
std::vector<uint16_t> g_framebuffer(kWidth * kHeight, 0);
std::vector<int16_t>  g_audio(kSamplesPerFrame * 2, 0);


// Subscriptions. `requested` is taken immediately; `active` only catches up on
// the next frame boundary, because that is when a real core can safely turn
// observation on and the session's "activas == pedidas after one frame" check
// would otherwise pass without the boundary ever existing.
uint32_t g_sub_requested = 0;
uint32_t g_sub_active    = 0;
uint64_t g_sub_activation_frame = 0;


/// Names the library in retro_get_system_info AND identifies the build in
/// the ABI descriptor, so it lives outside the ABI guard: the stock core
/// still has to say what it is.
const char kBuildId[] = "ayther-test-core-1";

// ---- Deterministic generation --------------------------------------------

uint32_t crc32_of(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
        }
    }
    return ~crc;
}

/// A 64-bit LCG. Deterministic, identical on every platform, and dependent on
/// nothing but its own state -- no time, no address, no uninitialised memory.
uint64_t next_random() {
    g_state.lcg = g_state.lcg * 6364136223846793005ULL + 1442695040888963407ULL;
    return g_state.lcg >> 17;
}

bool subscribed(uint32_t bit) { return (g_sub_active & bit) != 0; }

void reset_state(bool clear_subscriptions) {
    std::memset(&g_state, 0, sizeof(g_state));
    // Seed from the ROM so a different ROM produces a different run, which is
    // what makes the end-to-end hashes meaningful.
    g_state.lcg = 0x9E3779B97F4A7C15ULL ^ g_rom_crc32;
    for (size_t i = 0; i < kVdpRegBytes; ++i) {
        g_state.vdp_regs[i] = static_cast<uint8_t>(i * 7);
    }
    std::fill(g_framebuffer.begin(), g_framebuffer.end(), uint16_t{0});
    std::fill(g_audio.begin(), g_audio.end(), int16_t{0});

    if (clear_subscriptions) {
        // A real reset drops observation, which is precisely why the session
        // has to re-apply it. Keeping them here would make that path untested.
        g_sub_requested = 0;
        g_sub_active = 0;
        g_sub_activation_frame = 0;
    }
}

void advance_frame() {
    // Subscriptions requested during the previous frame take effect here, at
    // the boundary.
    if (g_sub_active != g_sub_requested) {
        g_sub_active = g_sub_requested;
        g_sub_activation_frame = g_state.frame;
    }

    if (g_poll) g_poll();
    uint16_t input = 0;
    if (g_input) {
        for (unsigned button = 0; button < 12; ++button) {
            if (g_input(0, RETRO_DEVICE_JOYPAD, 0, button)) {
                input |= static_cast<uint16_t>(1u << button);
            }
        }
    }
    g_state.last_input = input;

    // Work RAM: a small deterministic churn that depends on the frame and the
    // input, so a replay with different inputs diverges and a replay with the
    // same ones does not.
    for (unsigned i = 0; i < 64; ++i) {
        const uint64_t value = next_random();
        const size_t offset = static_cast<size_t>(value % kWorkRamBytes);
        g_state.work_ram[offset] = static_cast<uint8_t>(value ^ input);
    }
    // A fixed window carries the frame counter and input verbatim, so a test
    // can assert on something legible rather than only on a hash.
    std::memcpy(g_state.work_ram, &g_state.frame, sizeof(g_state.frame));
    std::memcpy(g_state.work_ram + 8, &input, sizeof(input));

    // A new frame starts with nothing dirty: the delta describes THIS frame,
    // not everything that ever changed.
    std::memset(g_state.dirty_patterns, 0, sizeof(g_state.dirty_patterns));
    // Raster activity that rises and falls, so an oracle checking that the
    // count both grows and shrinks has something real to observe.
    g_state.raster_event_count = static_cast<uint32_t>(g_state.frame % 17);

    if (subscribed(AYTHER_SUB_VDP_MEMORY)) {
        for (unsigned i = 0; i < 32; ++i) {
            const uint64_t value = next_random();
            const size_t offset = static_cast<size_t>(value % kVramBytes);
            const uint8_t byte = static_cast<uint8_t>(value >> 8);
            // Only a byte that actually CHANGES dirties its pattern. Marking on
            // every write would hide the bug the oracle hunts, because a delta
            // that says "everything changed" is never wrong and never useful.
            if (g_state.vram[offset] != byte) {
                g_state.vram[offset] = byte;
                g_state.dirty_patterns[offset / kPatternBytes] = 1;
            }
        }
        for (size_t i = 0; i < kCramBytes; i += 2) {
            const uint16_t colour =
                static_cast<uint16_t>((g_state.frame * 3 + i) & 0x0EEE);
            g_state.cram[i]     = static_cast<uint8_t>(colour >> 8);
            g_state.cram[i + 1] = static_cast<uint8_t>(colour);
        }
    }

    // Parsed sprites. Gated on the subscription on purpose: a core with the
    // standard profile parses nothing until a consumer asks, and the E-5
    // oracle's control runner discovered exactly this by measuring a silent
    // zero. Both the session and that control ask for SPRITE_CAPTURE.
    //
    // The table is a function of the ROM and the slot, NOT of the frame. That
    // is forced by how the oracle observes it: a libretro core is process
    // global, so the session and the control runner share this one instance and
    // step it alternately. A frame-varying table would have the session's
    // captured copy compared against a legacy pointer the control had already
    // advanced, and every frame would "disagree" for a reason that is about the
    // test harness rather than about the engine. Holding it steady keeps the
    // comparison on the axis E-5 is named for -- what the session PUBLISHES
    // versus what the legacy pointer says -- at the cost of not catching a
    // stale copy. The frame-varying signals live in RAM, video, and audio,
    // which no two observers read alternately.
    if (subscribed(AYTHER_SUB_SPRITE_CAPTURE)) {
        g_state.sprite_count = kSpritesPerFrame;
        for (uint8_t s = 0; s < kSpritesPerFrame; ++s) {
            uint8_t* entry = g_state.sprites + static_cast<size_t>(s) * kSpriteEntryBytes;
            const uint32_t seed = g_rom_crc32 + s * 2654435761u;
            const uint16_t x = static_cast<uint16_t>((seed >> 3) % kWidth);
            const uint16_t y = static_cast<uint16_t>((seed >> 11) % kHeight);
            const uint16_t tile = static_cast<uint16_t>((seed >> 17) & 0x07FF);
            entry[0] = static_cast<uint8_t>(x);
            entry[1] = static_cast<uint8_t>(x >> 8);
            entry[2] = static_cast<uint8_t>(y);
            entry[3] = static_cast<uint8_t>(y >> 8);
            entry[4] = static_cast<uint8_t>(tile);
            entry[5] = static_cast<uint8_t>(tile >> 8);
            entry[6] = static_cast<uint8_t>(s);            // slot
            entry[7] = 1;                                  // size
            entry[8] = static_cast<uint8_t>(s & 3);        // palette
            entry[9] = 0;
        }
    } else {
        g_state.sprite_count = 0;
        std::memset(g_state.sprites, 0, sizeof(g_state.sprites));
    }

    // Chip writes. Gated like the sprites: a core with the standard profile
    // logs nothing until someone subscribes.
    if (subscribed(AYTHER_SUB_AUDIO_WRITES)) {
        g_state.audio_write_count = kAudioWritesPerFrame;
        for (uint8_t i = 0; i < kAudioWritesPerFrame; ++i) {
            ayther_audio_write_v1& write = g_state.audio_writes[i];
            write.cycle = static_cast<uint32_t>(i) * 421u;
            write.addr  = static_cast<uint16_t>(0x4000 + i);
            write.data  = static_cast<uint8_t>((g_rom_crc32 >> (i % 24)) & 0xFF);
            write.chip  = static_cast<uint8_t>(i % 2 ? AYTHER_AUDIO_SOURCE_PSG
                                                     : AYTHER_AUDIO_SOURCE_FM);
        }
    } else {
        g_state.audio_write_count = 0;
        std::memset(g_state.audio_writes, 0, sizeof(g_state.audio_writes));
    }

    // Framebuffer: RGB565, a function of the frame and the pixel position, so
    // consecutive frames differ and a given frame is reproducible.
    for (unsigned y = 0; y < kHeight; ++y) {
        for (unsigned x = 0; x < kWidth; ++x) {
            const unsigned r = (x + static_cast<unsigned>(g_state.frame)) & 0x1F;
            const unsigned g = (y * 2 + static_cast<unsigned>(g_state.frame)) & 0x3F;
            const unsigned b = (x ^ y) & 0x1F;
            g_framebuffer[y * kWidth + x] =
                static_cast<uint16_t>((r << 11) | (g << 5) | b);
        }
    }

    // Audio: a square wave whose period walks with the frame, so the audio
    // hash is frame-sensitive rather than constant.
    const unsigned period = 32 + static_cast<unsigned>(g_state.frame % 64);
    for (unsigned i = 0; i < kSamplesPerFrame; ++i) {
        const int16_t value =
            static_cast<int16_t>(((i / period) % 2) ? 6000 : -6000);
        g_audio[i * 2]     = value;
        g_audio[i * 2 + 1] = static_cast<int16_t>(-value);
    }

    ++g_state.frame;
}

// Everything from here to the descriptor exists only to be reached through
// ayther_get_interface. The stock build compiles that entry point out, so this
// whole block goes with it: leaving it behind would be dead code that the
// compiler is right to complain about, and silencing that complaint would hide
// the next piece of dead code too.
#ifndef AYTHER_TEST_CORE_NO_ABI

// Everything below is reachable only through ayther_get_interface, so it is
// declared here rather than at the top of the file: the stock build compiles
// that entry point out and would otherwise carry a descriptor, its capability
// list, and a recomposition cache that nothing can ever reach.
constexpr unsigned kLinesPerFrame = 262;          // NTSC

/// The frame on which the VDP "chooses" a mode. Before it, SYSTEM reports
/// vdp_mode 0 with GEOMETRY_PENDING set; after it, a settled geometry. The
/// oracle needs both states to occur.
constexpr uint64_t kGeometrySettlesAtFrame = 3;

constexpr uint32_t kSupportedSubscriptions = AYTHER_SUB_ALL;

/// Only what this core actually implements. Declaring a capability it does not
/// honour would make the frontend take a path that then returns nothing, which
/// is worse than declaring nothing: the engine checks these before calling.
constexpr uint64_t kCapabilities =
    AYTHER_CAP_LEGACY_MEMORY |
    AYTHER_CAP_REGION_QUERY |
    AYTHER_CAP_REGION_READ |
    AYTHER_CAP_FRAME_SNAPSHOT |
    AYTHER_CAP_PARSED_SPRITES_V1 |
    AYTHER_CAP_AUDIO_WRITES_V1 |
    AYTHER_CAP_CONTROL_WRITE |
    AYTHER_CAP_RECOMPOSE_V1 |
    AYTHER_CAP_FRAME_DELTA_V1 |
    AYTHER_CAP_SUBSCRIPTIONS_V1 |
    AYTHER_CAP_SYSTEM_V1;

// Recomposed layers, cached by (frame, control generation). Not part of the
// savestate: they are derived, and a savestate that carried them would let a
// stale composition survive a restore.
constexpr size_t kFramePixels = static_cast<size_t>(kWidth) * kHeight;
std::vector<uint16_t> g_layer_bg_a(kFramePixels, 0);
std::vector<uint16_t> g_layer_bg_b(kFramePixels, 0);
std::vector<uint16_t> g_layer_window(kFramePixels, 0);
std::vector<uint16_t> g_layer_sprites(kFramePixels, 0);
uint64_t g_recompose_key = UINT64_MAX;   // no frame composed yet
uint64_t g_recompose_calls = 0;
uint64_t g_recompose_hits  = 0;

void fill_system(ayther_system_v1& out) {
    std::memset(&out, 0, sizeof(out));
    out.struct_size = sizeof(ayther_system_v1);
    out.layout_version = AYTHER_LAYOUT_SYSTEM_V1;
    out.system_hw = 0x80;                 // Mega Drive
    out.region_pal = 0;
    // Before the VDP settles, the mode is unknown and the geometry pending.
    const bool settled = g_state.frame >= kGeometrySettlesAtFrame;
    out.vdp_mode = settled ? 5 : 0;
    out.interlace = 0;
    // h40 describes the EMITTED frame. This core emits 256 px, so h40 stays 0
    // and never contradicts viewport_w.
    out.h40 = 0;
    out.shadow_highlight = 0;
    out.lines_per_frame = kLinesPerFrame;
    out.viewport_x = 0;
    out.viewport_y = 0;
    out.viewport_w = kWidth;
    out.viewport_h = kHeight;
    out.cpu_clock = 7670453;
    out.master_clock = 53693175;
    out.fm_core = 0;
    out.psg_present = 1;
    out.pcm_present = 0;
    out.flags = settled ? uint8_t{0} : AYTHER_SYSTEM_GEOMETRY_PENDING;
    out.rom_crc32 = g_rom_crc32;
    out.rom_bytes = static_cast<uint32_t>(g_rom.size());
}

// ---- AYTHER ABI -----------------------------------------------------------

struct RegionView {
    const void* data;
    uint32_t element_size;
    uint32_t capacity;
    uint32_t byte_size;
    uint32_t legacy_id;
    /// The subscription a consumer must hold to read it. 0 = always readable.
    uint32_t requires_subscription;
};

bool region_view(uint32_t region_id, RegionView& out) {
    switch (region_id) {
        case AYTHER_REGION_VRAM:
            out = {g_state.vram, 1, kVramBytes, kVramBytes, 3, AYTHER_SUB_VDP_MEMORY};
            return true;
        case AYTHER_REGION_CRAM:
            out = {g_state.cram, 1, kCramBytes, kCramBytes, 0x100, AYTHER_SUB_VDP_MEMORY};
            return true;
        case AYTHER_REGION_VDP_REGS:
            out = {g_state.vdp_regs, 1, kVdpRegBytes, kVdpRegBytes, 0x101,
                   AYTHER_SUB_VDP_MEMORY};
            return true;
        case AYTHER_REGION_VSRAM:
            out = {g_state.vsram, 1, kVsramBytes, kVsramBytes, 0x107,
                   AYTHER_SUB_VDP_MEMORY};
            return true;
        case AYTHER_REGION_Z80_RAM:
            out = {g_state.z80_ram, 1, kZ80RamBytes, kZ80RamBytes, 0x10F,
                   AYTHER_SUB_VDP_MEMORY};
            return true;
        case AYTHER_REGION_PARSED_SPRITES:
            out = {g_state.sprites, kSpriteEntryBytes, kSpritesPerFrame,
                   static_cast<uint32_t>(g_state.sprite_count) * kSpriteEntryBytes,
                   0x10B, AYTHER_SUB_SPRITE_CAPTURE};
            return true;
        case AYTHER_REGION_AUDIO_WRITES:
            out = {g_state.audio_writes,
                   static_cast<uint32_t>(sizeof(ayther_audio_write_v1)),
                   kAudioWritesPerFrame,
                   static_cast<uint32_t>(g_state.audio_write_count *
                                         sizeof(ayther_audio_write_v1)),
                   0x109, AYTHER_SUB_AUDIO_WRITES};
            return true;
        default:
            return false;
    }
}

/// The control regions a consumer may write, and the byte width each expects.
/// Anything not listed is read-only, which is a different answer from
/// "unknown": the engine distinguishes them and so must a core.
bool control_region(uint32_t region_id, uint32_t& expected_bytes) {
    switch (region_id) {
        case AYTHER_REGION_LAYER_MASK:            expected_bytes = 1; return true;
        case AYTHER_REGION_LAYER_DIM:             expected_bytes = 1; return true;
        case AYTHER_REGION_AUDIO_MUTE:            expected_bytes = 4; return true;
        case AYTHER_REGION_SPRITE_SUPPRESS:       expected_bytes = 0; return true;
        case AYTHER_REGION_TILE_SUPPRESS:         expected_bytes = 0; return true;
        case AYTHER_REGION_PLANE_TILE_SUPPRESS:   expected_bytes = 0; return true;
        case AYTHER_REGION_PLANE_SUPPRESS_ACTIVE: expected_bytes = 0; return true;
        default: return false;
    }
}

int32_t AYTHER_CALL api_query_region(uint32_t region_id,
                                     ayther_region_info_v1* out,
                                     uint32_t out_size) {
    if (out == nullptr || out_size < sizeof(ayther_region_info_v1)) {
        return AYTHER_STATUS_INVALID_ARGUMENT;
    }
    RegionView view{};
    if (!region_view(region_id, view)) return AYTHER_STATUS_NOT_FOUND;

    std::memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(ayther_region_info_v1);
    out->region_id = region_id;
    out->data_version = 1;
    out->element_size = view.element_size;
    out->capacity = view.capacity;
    out->byte_size = view.byte_size;
    out->access_flags = 0;
    out->legacy_memory_id = view.legacy_id;
    return AYTHER_STATUS_OK;
}

int32_t AYTHER_CALL api_read_region(uint32_t region_id, uint32_t offset,
                                    void* out, uint32_t byte_count,
                                    uint64_t expected_generation,
                                    uint64_t* actual_generation) {
    if (actual_generation != nullptr) *actual_generation = g_state.frame;
    if (out == nullptr) return AYTHER_STATUS_INVALID_ARGUMENT;
    // AYTHER_GENERATION_ANY means "whatever you have"; 0 is the same request
    // spelled by a caller that does not track generations at all.
    if (expected_generation != AYTHER_GENERATION_ANY && expected_generation != 0 &&
        expected_generation != g_state.frame) {
        return AYTHER_STATUS_STALE_GENERATION;
    }

    // SYSTEM is a descriptor rather than a buffer, so it is built on demand.
    if (region_id == AYTHER_REGION_SYSTEM) {
        ayther_system_v1 system{};
        fill_system(system);
        if (offset != 0 || byte_count > sizeof(system)) {
            return AYTHER_STATUS_OUT_OF_BOUNDS;
        }
        std::memcpy(out, &system, byte_count);
        return AYTHER_STATUS_OK;
    }

    RegionView view{};
    if (!region_view(region_id, view)) return AYTHER_STATUS_NOT_FOUND;
    // A region nobody subscribed to says so, rather than handing back a silent
    // buffer of zeroes. The difference matters: zeroes look exactly like a
    // valid reading of an empty frame, and a consumer cannot tell them apart.
    if (view.requires_subscription != 0 &&
        !subscribed(view.requires_subscription)) {
        return AYTHER_STATUS_NOT_SUBSCRIBED;
    }
    if (static_cast<uint64_t>(offset) + byte_count > view.byte_size) {
        return AYTHER_STATUS_OUT_OF_BOUNDS;
    }
    std::memcpy(out, static_cast<const uint8_t*>(view.data) + offset, byte_count);
    return AYTHER_STATUS_OK;
}

int32_t AYTHER_CALL api_write_control(uint32_t region_id, uint32_t offset,
                                      const void* data, uint32_t byte_count,
                                      uint64_t expected_generation,
                                      uint64_t* out_generation) {
    if (data == nullptr || byte_count == 0) return AYTHER_STATUS_INVALID_ARGUMENT;
    if (expected_generation != AYTHER_GENERATION_ANY && expected_generation != 0 &&
        expected_generation != g_state.control_generation) {
        return AYTHER_STATUS_STALE_GENERATION;
    }

    uint32_t expected_bytes = 0;
    if (!control_region(region_id, expected_bytes)) {
        // A region that exists but is observation-only is READ_ONLY; one that
        // does not exist at all is NOT_FOUND. Collapsing the two would tell a
        // caller to give up when the real answer is "wrong region".
        RegionView view{};
        return region_view(region_id, view) ? AYTHER_STATUS_READ_ONLY
                                            : AYTHER_STATUS_NOT_FOUND;
    }
    if (expected_bytes != 0 && byte_count != expected_bytes) {
        return AYTHER_STATUS_INVALID_ARGUMENT;
    }
    if (offset != 0) return AYTHER_STATUS_OUT_OF_BOUNDS;

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    switch (region_id) {
        case AYTHER_REGION_LAYER_MASK: g_state.layer_mask = bytes[0]; break;
        case AYTHER_REGION_LAYER_DIM:  g_state.layer_dim  = bytes[0]; break;
        case AYTHER_REGION_AUDIO_MUTE:
            std::memcpy(&g_state.audio_mute_mask, bytes, sizeof(uint32_t));
            break;
        default:
            // The suppression masks are accepted and reflected only in the
            // generation. Nothing in this core draws, so storing the bits would
            // be storing something no code reads.
            break;
    }

    // Every accepted write advances the generation, so a consumer holding a
    // snapshot can tell that its view is now behind.
    ++g_state.control_generation;
    if (out_generation != nullptr) *out_generation = g_state.control_generation;
    return AYTHER_STATUS_OK;
}

int32_t AYTHER_CALL api_recompose_multilayer(
    uint16_t* out_bg_a, uint16_t* out_bg_b, uint16_t* out_window,
    uint16_t* out_sprites, uint16_t* out_composite, uint32_t pixel_capacity,
    uint32_t /*flags*/, uint32_t* out_width, uint32_t* out_height) {
    if (!subscribed(AYTHER_SUB_RECOMPOSITION)) {
        return AYTHER_STATUS_NOT_SUBSCRIBED;
    }
    const uint32_t pixels = kWidth * kHeight;
    // A buffer too small for the frame is a caller mistake in the RECOMPOSE
    // family, and the frontend distinguishes those from the generic statuses.
    // Answering BUFFER_TOO_SMALL here would flatten a typed reason into one the
    // caller cannot act on differently.
    if (pixel_capacity < pixels) return AYTHER_STATUS_RC_INVALID_PARAMS;

    // Recomposing the same frame twice must not redo the work: a frontend that
    // asks for one layer at a time would otherwise pay for the whole
    // composition once per layer. The cache is keyed on the frame AND on the
    // control generation, because changing a layer control changes the answer
    // without advancing the frame.
    ++g_recompose_calls;
    const uint64_t key = (g_state.frame << 8) ^ g_state.control_generation;
    if (key != g_recompose_key) {
        g_recompose_key = key;
        for (uint32_t i = 0; i < pixels; ++i) {
            const uint32_t x = i % kWidth;
            const uint32_t y = i / kWidth;
            // Four visibly different layers, each with internal structure, so
            // an oracle asking whether they are actually separated gets a real
            // answer rather than four copies of one picture.
            g_layer_bg_a[i]    = static_cast<uint16_t>(((x & 0x1F) << 11) | 0x0001);
            g_layer_bg_b[i]    = static_cast<uint16_t>(((y & 0x3F) << 5) | 0x0002);
            g_layer_window[i]  = static_cast<uint16_t>(((x ^ y) & 0x1F) | 0x0004);
            g_layer_sprites[i] = static_cast<uint16_t>((((x + y) & 0x1F) << 6) | 0x0008);
        }
    } else {
        ++g_recompose_hits;
    }

    const size_t bytes = static_cast<size_t>(pixels) * sizeof(uint16_t);
    // The composite is the frame the core emitted, byte for byte. That is the
    // contract a frontend leans on: with no raster activity, recomposing has to
    // reproduce exactly what came out of video_refresh, or every pixel decided
    // from the layers is decided against a different picture.
    if (out_composite != nullptr) std::memcpy(out_composite, g_framebuffer.data(), bytes);
    if (out_bg_a != nullptr)      std::memcpy(out_bg_a, g_layer_bg_a.data(), bytes);
    if (out_bg_b != nullptr)      std::memcpy(out_bg_b, g_layer_bg_b.data(), bytes);
    if (out_window != nullptr)    std::memcpy(out_window, g_layer_window.data(), bytes);
    if (out_sprites != nullptr)   std::memcpy(out_sprites, g_layer_sprites.data(), bytes);

    if (out_width != nullptr) *out_width = kWidth;
    if (out_height != nullptr) *out_height = kHeight;
    return AYTHER_STATUS_OK;
}

int32_t AYTHER_CALL api_capture_snapshot(ayther_frame_snapshot_v1* out,
                                         uint32_t out_size) {
    if (out == nullptr || out_size < sizeof(ayther_frame_snapshot_v1)) {
        return AYTHER_STATUS_INVALID_ARGUMENT;
    }
    std::memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(ayther_frame_snapshot_v1);
    out->snapshot_version = 1;
    out->snapshot_generation = g_state.frame;
    out->frame_generation = g_state.frame;
    out->flags = AYTHER_SNAPSHOT_CONTENT_LOADED | AYTHER_SNAPSHOT_FRAME_ACTIVE;
    out->overflow_flags = 0;
    // No raster fallback: this core has no raster path to fall back FROM, and
    // reporting a non-zero reason would tell the engine a fidelity story that
    // is not true.
    out->fallback_reasons = 0;
    out->parsed_sprite_count = g_state.sprite_count;
    out->audio_write_count = g_state.audio_write_count;
    return AYTHER_STATUS_OK;
}

int32_t AYTHER_CALL api_poll_frame_delta(ayther_frame_delta_v1* out,
                                         uint32_t out_size) {
    if (out == nullptr || out_size < sizeof(ayther_frame_delta_v1)) {
        return AYTHER_STATUS_INVALID_ARGUMENT;
    }
    std::memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(ayther_frame_delta_v1);
    out->delta_version = 1;
    // The same generation the snapshot reports for this frame. A delta whose
    // generation disagreed with the snapshot would describe a different frame
    // than the one the consumer is holding.
    out->frame_generation = g_state.frame;
    out->raster_event_count = g_state.raster_event_count;
    out->parsed_sprite_count = g_state.sprite_count;
    out->audio_write_count = g_state.audio_write_count;
    out->raster_events_dropped = 0;
    static_assert(sizeof(out->dirty_patterns) == kPatternCount,
                  "the delta's pattern map must cover VRAM exactly");
    std::memcpy(out->dirty_patterns, g_state.dirty_patterns, kPatternCount);
    return AYTHER_STATUS_OK;
}

int32_t AYTHER_CALL api_get_recompose_stats(ayther_recompose_stats_v1* out,
                                             uint32_t out_size) {
    if (out == nullptr || out_size < sizeof(ayther_recompose_stats_v1)) {
        return AYTHER_STATUS_INVALID_ARGUMENT;
    }
    std::memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(ayther_recompose_stats_v1);
    out->multilayer_calls = g_recompose_calls;
    out->multilayer_hits = g_recompose_hits;
    // This core implements three controls. Pack their current values into a
    // stable fingerprint so a consumer can distinguish a new frame from a
    // control change when explaining a cache miss.
    out->controls_fingerprint =
        static_cast<uint64_t>(g_state.layer_mask) |
        (static_cast<uint64_t>(g_state.layer_dim) << 8) |
        (static_cast<uint64_t>(g_state.audio_mute_mask) << 16);
    return AYTHER_STATUS_OK;
}

int32_t AYTHER_CALL api_get_subscriptions(ayther_subscription_state_v1* out,
                                          uint32_t out_size) {
    if (out == nullptr || out_size < sizeof(ayther_subscription_state_v1)) {
        return AYTHER_STATUS_INVALID_ARGUMENT;
    }
    std::memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(ayther_subscription_state_v1);
    out->state_version = 1;
    out->supported_mask = kSupportedSubscriptions;
    out->active_mask = g_sub_active;
    out->requested_mask = g_sub_requested;
    out->activation_frame = g_sub_activation_frame;
    return AYTHER_STATUS_OK;
}

int32_t AYTHER_CALL api_set_subscriptions(uint32_t requested_mask) {
    if ((requested_mask & ~kSupportedSubscriptions) != 0) {
        return AYTHER_STATUS_UNSUPPORTED;
    }
    g_sub_requested = requested_mask;
    return AYTHER_STATUS_OK;
}

const ayther_interface_v1 g_interface = {
    /* abi_version              */ AYTHER_ABI_VERSION_LATEST,
    /* struct_size              */ sizeof(ayther_interface_v1),
    /* capabilities             */ kCapabilities,
    /* host_endianness          */ 0,
    /* pointer_size             */ sizeof(void*),
    /* region_info_size         */ sizeof(ayther_region_info_v1),
    /* frame_snapshot_size      */ sizeof(ayther_frame_snapshot_v1),
    /* sprite_size              */ sizeof(ayther_sprite_v1),
    /* audio_write_size         */ sizeof(ayther_audio_write_v1),
    /* build_id                 */ kBuildId,
    /* build_id_size            */ sizeof(kBuildId) - 1,
    /* reserved0                */ 0,
    /* query_region             */ api_query_region,
    /* read_region              */ api_read_region,
    /* write_control            */ api_write_control,
    /* capture_snapshot         */ api_capture_snapshot,
    /* recompose_frame          */ nullptr,
    /* audio_event_size         */ sizeof(ayther_audio_event_v1),
    /* audio_transport_stats_size */ sizeof(ayther_audio_transport_stats_v1),
    /* poll_audio_events        */ nullptr,
    /* get_audio_transport_stats*/ nullptr,
    /* subscription_state_size  */ sizeof(ayther_subscription_state_v1),
    /* reserved1                */ 0,
    /* get_subscriptions        */ api_get_subscriptions,
    /* set_subscriptions        */ api_set_subscriptions,
    /* frame_delta_size         */ sizeof(ayther_frame_delta_v1),
    /* poll_frame_delta         */ api_poll_frame_delta,
    /* recompose_stats_size     */ sizeof(ayther_recompose_stats_v1),
    /* reserved2                */ 0,
    /* get_recompose_stats      */ api_get_recompose_stats,
    /* recompose_multilayer     */ api_recompose_multilayer,
    /* frame_delta_since        */ nullptr,
};

#endif  // !AYTHER_TEST_CORE_NO_ABI

}  // namespace

// ---------------------------------------------------------------------------
// libretro entry points
// ---------------------------------------------------------------------------

extern "C" {

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_set_environment(retro_environment_t cb) { g_environ = cb; }
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { g_video = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { g_sample = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { g_batch = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { g_poll = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { g_input = cb; }

RETRO_API void retro_init(void) { reset_state(true); }
RETRO_API void retro_deinit(void) { g_rom.clear(); }

RETRO_API void retro_get_system_info(struct retro_system_info* info) {
    if (info == nullptr) return;
    std::memset(info, 0, sizeof(*info));
    info->library_name = "AYTHER Test Core";
    info->library_version = kBuildId;
    info->valid_extensions = "md|bin|gen";
    info->need_fullpath = false;
    info->block_extract = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info) {
    if (info == nullptr) return;
    std::memset(info, 0, sizeof(*info));
    info->geometry.base_width = kWidth;
    info->geometry.base_height = kHeight;
    info->geometry.max_width = kWidth;
    info->geometry.max_height = kHeight;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps = kFps;
    info->timing.sample_rate = kSampleRate;
}

RETRO_API void retro_set_controller_port_device(unsigned, unsigned) {}

RETRO_API bool retro_load_game(const struct retro_game_info* game) {
    if (game == nullptr || game->data == nullptr || game->size == 0) return false;

    g_rom.assign(static_cast<const uint8_t*>(game->data),
                 static_cast<const uint8_t*>(game->data) + game->size);
    g_rom_crc32 = crc32_of(g_rom.data(), g_rom.size());
    reset_state(true);

    if (g_environ != nullptr) {
        // RGB565 is what the engine's tile hasher and renderer expect; a core
        // that stayed on the 0RGB1555 default would exercise a different path.
        unsigned format = RETRO_PIXEL_FORMAT_RGB565;
        g_environ(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);
    }
    return true;
}

RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info*,
                                       size_t) {
    return false;
}

RETRO_API void retro_unload_game(void) { g_rom.clear(); }

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

RETRO_API void retro_run(void) {
    advance_frame();
    if (g_video != nullptr) {
        g_video(g_framebuffer.data(), kWidth, kHeight, kWidth * sizeof(uint16_t));
    }
    if (g_batch != nullptr) {
        g_batch(g_audio.data(), kSamplesPerFrame);
    }
}

RETRO_API void retro_reset(void) {
    // A reset drops observation. The session is expected to notice and
    // re-apply; that is the behaviour the oracle protects.
    reset_state(true);
}

RETRO_API size_t retro_serialize_size(void) { return sizeof(SerializedState); }

RETRO_API bool retro_serialize(void* data, size_t size) {
    if (data == nullptr || size < sizeof(SerializedState)) return false;
    std::memcpy(data, &g_state, sizeof(SerializedState));
    return true;
}

RETRO_API bool retro_unserialize(const void* data, size_t size) {
    if (data == nullptr || size < sizeof(SerializedState)) return false;
    std::memcpy(&g_state, data, sizeof(SerializedState));
    // Deliberately does NOT touch the subscription masks. The compose path
    // unserializes every frame, and clearing them here would silence all
    // observation after the first compose -- without an error, because an
    // unsubscribed core returns zeroes rather than a failure.
    return true;
}

RETRO_API void* retro_get_memory_data(unsigned id) {
    switch (id) {
        case RETRO_MEMORY_SYSTEM_RAM: return g_state.work_ram;
        case RETRO_MEMORY_VIDEO_RAM:  return g_state.vram;
        case 0x100: return g_state.cram;
        case 0x101: return g_state.vdp_regs;
        case 0x107: return g_state.vsram;
        case 0x109: return g_state.audio_writes;
        case 0x10A: return &g_state.audio_write_count;
        case 0x10B: return g_state.sprites;
        case 0x10C: return &g_state.sprite_count;
        case 0x10F: return g_state.z80_ram;
        default: return nullptr;
    }
}

RETRO_API size_t retro_get_memory_size(unsigned id) {
    switch (id) {
        case RETRO_MEMORY_SYSTEM_RAM: return kWorkRamBytes;
        case RETRO_MEMORY_VIDEO_RAM:  return kVramBytes;
        case 0x100: return kCramBytes;
        case 0x101: return kVdpRegBytes;
        case 0x107: return kVsramBytes;
        case 0x109: return sizeof(g_state.audio_writes);
        case 0x10A: return sizeof(g_state.audio_write_count);
        case 0x10B: return sizeof(g_state.sprites);
        case 0x10C: return sizeof(g_state.sprite_count);
        case 0x10F: return kZ80RamBytes;
        default: return 0;
    }
}

RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned, bool, const char*) {}

#ifndef AYTHER_TEST_CORE_NO_ABI
// The negotiated entry point. Its ABSENCE is what the stock build tests, so
// this is the single line of difference between the two libraries.
AYTHER_API const ayther_interface_v1* AYTHER_CALL ayther_get_interface(
    uint32_t requested_version) {
    // 0 means "give me the latest you support". An explicit version this core
    // does not implement returns NULL rather than a descriptor that lies.
    if (requested_version != 0 &&
        requested_version > AYTHER_ABI_VERSION_LATEST) {
        return nullptr;
    }
    return &g_interface;
}
#endif

}  // extern "C"
