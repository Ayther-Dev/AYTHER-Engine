#ifndef AYTHER_SDK_H
#define AYTHER_SDK_H
/* ---------------------------------------------------------------------------
 * ayther_sdk.h — provisional C facade for the native engine.
 *
 * This deliberately small interface avoids exposing a C++ compiler ABI and
 * keeps private engine types out of foreign-language integrations.
 *
 * Global contract for every function in this header:
 *
 *   - Threading: one session has one owner and is driven from one thread. No
 *     function is thread-safe unless it explicitly says otherwise.
 *   - Ownership: pointers returned by the API are borrowed from the session and
 *     remain valid only until the next state-mutating operation. Copy any data
 *     that must survive that boundary.
 *   - Errors: fallible functions return AyStatus. Status values are intended
 *     for machines; error-message text is diagnostic and may change.
 *   - Encapsulation: AySession is incomplete. Consumers must not depend on its
 *     representation.
 *
 * The SDK does not provide ROMs or emulator cores. The caller supplies both and
 * remains responsible for authorization, compatibility, and safe distribution.
 * --------------------------------------------------------------------------- */
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Version of this C facade. It is independent of the broader SDK version. */
#define AYTHER_SDK_C_API_VERSION 1

/* -- Errors ----------------------------------------------------------------- */
typedef enum AyStatus {
    AY_OK = 0,
    AY_ERR_ARGS = 1,        /**< Null pointer or out-of-range argument. */
    AY_ERR_CORE = 2,        /**< Core loading or ABI negotiation failed. */
    AY_ERR_ROM = 3,         /**< ROM loading failed. */
    AY_ERR_PACK = 4,        /**< Pack loading, verification, or matching failed. */
    AY_ERR_STATE = 5,       /**< Operation is invalid in the current state. */
    AY_ERR_UNSUPPORTED = 6, /**< Loaded core does not expose the capability. */
    AY_ERR_CAPACITY = 7,    /**< Caller-provided buffer is too small. */
    AY_ERR_INTERNAL = 8
} AyStatus;

/** Opaque session handle. Its representation is private and unstable. */
typedef struct AySession AySession;

/** Returns this session's last diagnostic message. Never null. The borrowed
 *  string remains valid until another operation updates the session error. */
const char* ay_error_message(const AySession* s);

/** Returns the thread-local diagnostic from the last failed creation operation. */
const char* ay_last_create_error(void);

/* -- Lifecycle -------------------------------------------------------------- */

typedef struct AySessionConfig {
    const char* core_path;   /**< Caller-supplied libretro core path. */
    const char* rom_path;    /**< Caller-supplied ROM path. */
    const char* pack_path;   /**< Optional `.ay` path; null or empty means none. */
    int         enable_audio;/**< Zero selects headless operation. */

    /* Core options are identified by their libretro keys. They are applied
     * only while opening the session because some cores initialize audio and
     * timing state from these values. To change an option, create a new
     * session.
     *
     * Available keys are core-specific and can be inspected through
     * `ay_core_option_count`, `ay_core_option_key`, and
     * `ay_core_option_desc` after opening a session. New fields must be
     * appended to preserve source compatibility. Null pointers and a zero
     * count mean that no options were supplied. */
    const char* const* option_keys;    /**< Keys; null means no options. */
    const char* const* option_values;  /**< Values parallel to `option_keys`. */
    uint32_t           option_count;
} AySessionConfig;

/** Creates a session. `*out` is written only on `AY_OK`. On failure, retrieve
 *  the diagnostic with `ay_last_create_error()`. */
AyStatus ay_create(const AySessionConfig* cfg, AySession** out);

/** Destroys a session and every resource it owns. Null is a no-op. */
void ay_destroy(AySession* s);

/* -- Frames ----------------------------------------------------------------- */

/** Pixel format of a delivered frame. Values match libretro for interoperability,
 *  but are declared here so consumers do not need to include `libretro.h`. */
typedef enum AyPixelFormat {
    AY_PIX_UNKNOWN  = -1,
    AY_PIX_0RGB1555 = 0,
    AY_PIX_XRGB8888 = 1,
    AY_PIX_RGB565   = 2
} AyPixelFormat;

typedef struct AyFrame {
    const void* pixels;   /**< Borrowed; valid until the next `ay_step`. */
    uint32_t    width;
    uint32_t    height;
    uint32_t    pitch;    /**< Bytes per row. */
    int32_t     format;   /**< One of `AyPixelFormat`. */
    uint64_t    index;    /**< Monotonic session frame number. */
} AyFrame;

/** Advances the emulated session by exactly one frame. */
AyStatus ay_step(AySession* s);

/** Returns the current frame without advancing the session.
 *
 * `pixels` can be null when the core reuses the preceding frame. In that case,
 * retain the previous image rather than replacing it with black. */
AyStatus ay_frame(const AySession* s, AyFrame* out);

/* -- Content ---------------------------------------------------------------- */
AyStatus ay_set_pack(AySession* s, const char* pack_path);
AyStatus ay_clear_pack(AySession* s);
/** Returns nonzero when a content pack is mounted. */
int      ay_has_pack(const AySession* s);
/** Returns the pack-declared game ID, or an empty string without a pack.
 *  The borrowed string is invalidated by the next `ay_set_pack`. */
const char* ay_game_id(const AySession* s);

/* -- Memory ----------------------------------------------------------------- */

/** Returns visible work-RAM size in bytes, or zero when unavailable. */
uint32_t ay_memory_size(const AySession* s);

/** Copies `len` bytes at `addr` from the 68000-visible work-RAM view.
 *
 * The returned view is normalized rather than the core's raw buffer. On Mega
 * Drive cores, this accounts for word-swapped work RAM centrally so every
 * caller observes the same byte order. */
AyStatus ay_read_memory(const AySession* s, uint32_t addr, void* dst, uint32_t len);

/* -- Input ------------------------------------------------------------------ */

/** RetroPad buttons as a mask. Values match libretro. */
typedef enum AyButton {
    AY_BTN_B = 1u << 0, AY_BTN_Y = 1u << 1, AY_BTN_SELECT = 1u << 2,
    AY_BTN_START = 1u << 3, AY_BTN_UP = 1u << 4, AY_BTN_DOWN = 1u << 5,
    AY_BTN_LEFT = 1u << 6, AY_BTN_RIGHT = 1u << 7, AY_BTN_A = 1u << 8,
    AY_BTN_X = 1u << 9, AY_BTN_L = 1u << 10, AY_BTN_R = 1u << 11
} AyButton;

/** Sets the button state for a port. The value persists until replaced. */
AyStatus ay_set_input(AySession* s, uint32_t port, uint32_t buttons);
/** Returns the last button state assigned to the port. */
uint32_t ay_get_input(const AySession* s, uint32_t port);

/* -- Audio ------------------------------------------------------------------ */

typedef struct AyAudioEvent {
    uint64_t signature;   /**< Stable sound-event identity. */
    uint64_t instrument;  /**< Timbre identity independent of note and channel. */
    uint32_t start_frame;
    uint32_t end_frame;
    uint8_t  chip;        /**< 0 = FM · 1 = PSG · 3 = PCM */
    uint8_t  channel;
    uint8_t  pitch;       /**< MIDI note; 255 means unpitched. */
    uint8_t  velocity;    /**< Range 1-127; zero means unknown. */
} AyAudioEvent;

/** Copies at most `max` detected events to `out` and returns the total count.
 *  Pass a null `out` pointer to query the required capacity. */
uint32_t ay_audio_events(const AySession* s, AyAudioEvent* out, uint32_t max);

/* -- Capabilities ----------------------------------------------------------- */

/** Declares which observations the loaded core exposes.
 *
 * Upstream cores without the AYTHER extension expose empty observation
 * regions. Capability checks distinguish unavailable data from valid zeroes. */
typedef enum AyCapability {
    AY_CAP_VIDEO        = 1u << 0,  /**< Emulator framebuffer. */
    AY_CAP_AUDIO_EVENTS = 1u << 1,  /**< Typed audio events. */
    AY_CAP_MEMORY       = 1u << 2,  /**< Work RAM. */
    AY_CAP_INPUT        = 1u << 3,  /**< Input injection. */
    AY_CAP_VDP          = 1u << 4,  /**< VRAM/CRAM/VSRAM; requires the fork. */
    AY_CAP_IDENTITIES   = 1u << 5,  /**< Sprite/tile identities; requires VDP. */
    AY_CAP_PACK         = 1u << 6   /**< `.ay` pack reads and substitutions. */
} AyCapability;

uint32_t ay_capabilities(const AySession* s);

/* -- Frame export ----------------------------------------------------------- */

/** Copies the current emulator frame into caller-owned storage.
 *
 * `out_desc` always describes the required format and dimensions. The exported
 * frame contains resolved substitutions before GPU composition; this API has
 * no windowing or Vulkan dependency. If `cap` is too small, the function
 * returns `AY_ERR_CAPACITY` while still filling `out_desc`, allowing an exact
 * second allocation. */
AyStatus ay_export_frame(const AySession* s, void* dst, uint32_t cap,
                         AyFrame* out_desc);

/** Returns the number of bytes required by `ay_export_frame`. */
uint32_t ay_export_frame_size(const AySession* s);

/* -- Session-independent pack access ---------------------------------------
 *
 * Catalogs, installers, and validators can inspect a pack without loading an
 * emulator core. Verification is lazy: opening checks the signature and index;
 * reading an entry verifies that entry's content hash. */
typedef struct AyPack AyPack;   /* Opaque, owning handle. */

/** Opens an `.ay` pack. Returns null for an invalid path, signature, or index. */
AyPack* ay_pack_open(const char* path);
/** Closes an owning pack handle. A null pointer is a no-op. */
void    ay_pack_close(AyPack* p);

/** Returns the number of indexed entries. */
uint32_t ay_pack_entry_count(const AyPack* p);
/** Returns the borrowed name of entry `i`, or null when out of range.
 *  The pointer remains valid until `ay_pack_close`. */
const char* ay_pack_entry_name(const AyPack* p, uint32_t i);

/** Returns an entry's byte size, or -1 when it is absent. */
int64_t ay_pack_entry_size(const AyPack* p, const char* logical_path);

/** Reads and verifies a complete entry.
 *  Returns copied bytes, or -1 when missing, invalid, or too large for `cap`. */
int64_t ay_pack_read_entry(const AyPack* p, const char* logical_path,
                           void* dst, uint32_t cap);

/** Returns nonzero if an entry supports direct range reads.
 *  Compressed entries are not streamable because a range would require full
 *  decompression. */
int ay_pack_entry_streamable(const AyPack* p, const char* logical_path);

/** Reads a range from a streamable entry, or returns -1 when unsupported. */
int64_t ay_pack_read_range(const AyPack* p, const char* logical_path,
                           uint64_t off, uint32_t len, void* dst);

/** Returns the manifest `game_id`, or an empty string when absent. */
const char* ay_pack_game_id(const AyPack* p);

/* -- Core options ----------------------------------------------------------- */

/** Returns the number of options declared by the loaded core. */
uint32_t ay_core_option_count(const AySession* s);

/** Returns option `i`'s key, or an empty string when out of range. */
const char* ay_core_option_key(const AySession* s, uint32_t i);

/** Returns the core's raw description, usually `"Description; a|b|c"` with
 *  the default value first. The SDK does not impose a parser or presentation. */
const char* ay_core_option_desc(const AySession* s, uint32_t i);

/* -- Compatibility grades -------------------------------------------------- */
/*
 * This is the shared compatibility decision used before execution, launch, or
 * distribution. `EXPERIMENTAL` means that required evidence was unavailable;
 * it is distinct from both verified compatibility and incompatibility.
 * `EXACT` therefore requires complete verification context. The query does not
 * load an emulator core and can run before content is downloaded.
 */
typedef struct AyCompat AyCompat;

/** Ordered from best to worst. Values up to `AY_COMPAT_WARNINGS` are verified
 *  as usable. Enumerator values are part of the C ABI. */
typedef enum {
    AY_COMPAT_EXACT        = 0,  /**< Verified match without findings. */
    AY_COMPAT_WARNINGS     = 1,  /**< Verified as usable with limitations. */
    AY_COMPAT_EXPERIMENTAL = 2,  /**< Not fully verified; context is missing. */
    AY_COMPAT_INCOMPATIBLE = 3   /**< Verified as incompatible. */
} AyCompatGrade;

/** Evidence known by the consumer. Missing optional values remain unverified
 *  and lower the result to `EXPERIMENTAL`; they are never assumed valid. */
typedef struct {
    uint32_t    rom_crc32;
    int         has_rom;         /**< Zero when the ROM CRC is unknown. */
    const char* platform;        /**< "megadrive" · "segacd" · NULL */
    const char* core_build_id;   /**< Active core build ID, or null. */
    const char* engine_version;  /**< Null selects this build's version. */
    int         release_build;   /**< Unsigned packs fail in release mode. */
} AyCompatCtx;

/** Evaluates `pack_path`. A null `ctx` supplies no verification evidence, so
 *  the grade cannot be `EXACT`. Returns null only for an invalid path. */
AyCompat* ay_pack_compat(const char* pack_path, const AyCompatCtx* ctx);
/** Releases an owning compatibility result. A null pointer is a no-op. */
void      ay_compat_close(AyCompat* c);

/** Returns the evaluated compatibility grade. */
AyCompatGrade ay_compat_grade(const AyCompat* c);

/** Returns nonzero unless the result is `AY_COMPAT_INCOMPATIBLE`. */
int ay_compat_runnable(const AyCompat* c);

/** Returns a non-empty borrowed explanation of the grade. */
const char* ay_compat_reason(const AyCompat* c);

/** Returns the number of evidence fields that could not be verified. */
uint32_t    ay_compat_unverified_count(const AyCompat* c);
/** Returns the borrowed name of unverified field `i`, or an empty string. */
const char* ay_compat_unverified(const AyCompat* c, uint32_t i);

/** Returns the complete borrowed verdict and validation report as JSON. */
const char* ay_compat_json(const AyCompat* c);

/* -- Extension points ------------------------------------------------------ */

/** Frame observer invoked after each successful `ay_step`. */
typedef void (*AyFrameObserver)(const AyFrame* frame, void* user);

/** Audio observer receiving events produced by the current frame. */
typedef void (*AyAudioObserver)(const AyAudioEvent* events, uint32_t count,
                                void* user);

/** Post-filter that may modify pixels in the caller-owned export buffer.
 *
 * Return zero on success and nonzero on failure. The callback never receives
 * the engine's internal framebuffer, so it cannot mutate session state. */
typedef int (*AyPostFilter)(void* pixels, uint32_t width, uint32_t height,
                            uint32_t pitch, int32_t format, void* user);

/** Registers an extension and writes its removal ID to `id_out`.
 *
 * A failing extension is disabled after `AY_EXT_MAX_FAILURES` consecutive
 * failures. Extension failure does not terminate the session or corrupt its
 * frame. Query accumulated failures with `ay_extension_failures`. */
#define AY_EXT_MAX_FAILURES 3

AyStatus ay_add_frame_observer(AySession* s, AyFrameObserver fn, void* user,
                               uint32_t* id_out);
AyStatus ay_add_audio_observer(AySession* s, AyAudioObserver fn, void* user,
                               uint32_t* id_out);
AyStatus ay_add_post_filter(AySession* s, AyPostFilter fn, void* user,
                            uint32_t* id_out);
/** Removes an observer or post-filter by ID. */
AyStatus ay_remove_extension(AySession* s, uint32_t id);
/** Returns accumulated failures, or `UINT32_MAX` for an unknown ID. */
uint32_t ay_extension_failures(const AySession* s, uint32_t id);
/** Returns nonzero while the extension remains active. */
int      ay_extension_active(const AySession* s, uint32_t id);

/* Extensions cannot write emulator-core state, alter asset identities, or
 * bypass pack verification because those operations are absent from this API.
 * Observers receive const data and filters modify only caller-owned copies.
 *
 * Engine replay remains deterministic with extensions enabled. A filter that
 * depends on external state can make its own output nondeterministic, but it
 * cannot affect session replay. */

#ifdef __cplusplus
}  /* extern "C" */
#endif
#endif  /* AYTHER_SDK_H */
