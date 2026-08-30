#pragma once
// ---------------------------------------------------------------------------
// decode_limits.h — ceilings on what DECODING a pack entry may allocate.
//
// pack_security.rs bounds the CONTAINER: archive size, entry count, per-entry
// bytes, and the 200:1 expansion ratio. Those stop a zip bomb. They do not stop
// the next thing, because a decoder's output is not the entry's size:
//
//   * A 70-byte PNG whose IHDR declares 60000 x 60000 asks for 14 GB of RGBA
//     the moment stbi_load_from_memory touches it. It compresses to nothing and
//     passes every container check, because the FILE is 70 bytes.
//   * An IVF header declaring 65535 x 65535 does the same to the video path.
//   * A WAV can declare far more audio than anyone will listen to.
//
// So the ceilings here are applied to the DECLARED output, read from the header,
// BEFORE the allocation happens. A refusal costs nothing; discovering the size
// after allocating is not a check.
//
// Processing time is bounded by the same numbers plus the Lua instruction
// budget in core/src/script_env.rs: with output bytes capped, decode time is
// capped, and a script cannot spin past MAX_INSTRUCTIONS_PER_FRAME.
//
// Engine-internal header: not installed.
// ---------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>

namespace ayther::limits {

// ---- Images ---------------------------------------------------------------

/// Longest side accepted from a decoded image. 16384 is the floor Vulkan's
/// maxImageDimension2D is allowed to report, so anything larger could not be
/// uploaded as a texture on a conforming device even if it decoded.
inline constexpr int64_t kMaxImageDimension = 16384;

/// Total pixels accepted. The dimension cap alone still permits
/// 16384 x 16384 = 268 M pixels, which is 1 GiB of RGBA; this is the cap that
/// actually bounds the allocation. 64 M pixels is 256 MiB at RGBA8, and it
/// still admits an 8192 x 8192 sheet.
inline constexpr int64_t kMaxImagePixels = 64ll << 20;

/// Bytes per decoded pixel at the widest format the engine requests (RGBA8).
inline constexpr int64_t kDecodedPixelBytes = 4;

// ---- Audio ----------------------------------------------------------------

/// Bytes of decoded PCM accepted from one asset. At 44.1 kHz stereo 16-bit
/// this is a little over 25 minutes, which is longer than any single HD cue.
inline constexpr int64_t kMaxDecodedAudioBytes = 256ll << 20;

// ---- Video ----------------------------------------------------------------

/// Longest side accepted from a video stream. Deliberately below the image cap:
/// a video allocates several frames at once, and the engine composites at
/// console resolutions.
inline constexpr int64_t kMaxVideoDimension = 8192;

/// Total pixels per video frame.
inline constexpr int64_t kMaxVideoPixels = 16ll << 20;

/// Largest single compressed packet accepted from a stream.
inline constexpr int64_t kMaxVideoPacketBytes = 64ll << 20;

// ---- Predicates -----------------------------------------------------------
//
// Signed and taken as int64_t on purpose: these are fed values parsed from
// untrusted headers, and a negative or absurd number must fail the check
// rather than wrap into a small positive one.

[[nodiscard]] constexpr bool image_dimensions_ok(int64_t width,
                                                 int64_t height) noexcept {
    if (width <= 0 || height <= 0) return false;
    if (width > kMaxImageDimension || height > kMaxImageDimension) return false;
    return width <= kMaxImagePixels / height;
}

[[nodiscard]] constexpr bool video_dimensions_ok(int64_t width,
                                                 int64_t height) noexcept {
    if (width <= 0 || height <= 0) return false;
    if (width > kMaxVideoDimension || height > kMaxVideoDimension) return false;
    return width <= kMaxVideoPixels / height;
}

[[nodiscard]] constexpr bool decoded_audio_bytes_ok(int64_t bytes) noexcept {
    return bytes >= 0 && bytes <= kMaxDecodedAudioBytes;
}

[[nodiscard]] constexpr bool video_packet_bytes_ok(int64_t bytes) noexcept {
    return bytes > 0 && bytes <= kMaxVideoPacketBytes;
}

// ---- Header inspection ----------------------------------------------------

/// Reads ONLY the header of an encoded image and reports whether decoding it
/// would stay inside the ceilings above. Never allocates the pixel buffer, so
/// it is safe to call on untrusted bytes.
///
/// `width` and `height` receive the declared dimensions when they are readable,
/// which lets a caller log what it refused. Returns false for an unreadable
/// header as well as an oversized one: a decoder that cannot state its output
/// size up front does not get to allocate it.
[[nodiscard]] bool image_header_within_limits(const uint8_t* data, size_t size,
                                              int* width, int* height) noexcept;

}  // namespace ayther::limits
