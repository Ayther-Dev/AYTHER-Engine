// ---------------------------------------------------------------------------
// Decoded-resource ceilings.
//
// The container checks in pack_security.rs bound the FILE. This bounds what
// decoding the file may allocate, which is a different number entirely: the
// PNG built below is 33 bytes, compresses to nothing, passes every archive
// limit, and asks for 14 GB of RGBA the moment a decoder believes its header.
//
// That is the property under test: a small compressed input cannot produce an
// unbounded resource.
// ---------------------------------------------------------------------------
#include "decode_limits.h"

#include <cstdio>
#include <cstdint>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("  [%s] %s\n", condition ? " OK " : "FAIL", message);
    if (!condition) ++failures;
}

void push_be32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

/// A PNG carrying the signature, an IHDR declaring `width`x`height`, and an
/// empty IDAT header. A header inspection stops at the start of IDAT, so this
/// is the whole of what it reads -- and the whole of what an attacker has to
/// send to make a decoder allocate width*height*4 bytes.
std::vector<uint8_t> png_header(uint32_t width, uint32_t height) {
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    push_be32(out, 13);                       // IHDR payload length
    out.insert(out.end(), {'I', 'H', 'D', 'R'});
    push_be32(out, width);
    push_be32(out, height);
    out.push_back(8);                         // bit depth
    out.push_back(6);                         // colour type: RGBA
    out.push_back(0);                         // compression
    out.push_back(0);                         // filter
    out.push_back(0);                         // interlace
    push_be32(out, 0);                        // CRC (not verified on this path)
    push_be32(out, 0);                        // IDAT payload length
    out.insert(out.end(), {'I', 'D', 'A', 'T'});
    return out;
}

}  // namespace

int main() {
    namespace limits = ayther::limits;

    // --- The predicates ---------------------------------------------------
    {
        check(limits::image_dimensions_ok(1, 1), "a 1x1 image is accepted");
        check(limits::image_dimensions_ok(1920, 1080), "an ordinary sheet is accepted");
        check(limits::image_dimensions_ok(8192, 8192), "a large legitimate sheet is accepted");

        check(!limits::image_dimensions_ok(0, 64), "a zero side is refused");
        check(!limits::image_dimensions_ok(64, 0), "a zero height is refused");
        check(!limits::image_dimensions_ok(-1, 64), "a negative side is refused");
        check(!limits::image_dimensions_ok(64, -1), "a negative height is refused");

        check(!limits::image_dimensions_ok(limits::kMaxImageDimension + 1, 1),
              "one pixel past the per-side ceiling is refused");
        check(limits::image_dimensions_ok(limits::kMaxImageDimension, 1),
              "the per-side ceiling itself is accepted");

        // The per-side cap alone still allows 16384x16384; the pixel cap is
        // what actually bounds the allocation.
        check(!limits::image_dimensions_ok(limits::kMaxImageDimension,
                                           limits::kMaxImageDimension),
              "a square at the per-side ceiling exceeds the pixel ceiling");

        // The product must not be computed by multiplying two large values.
        check(!limits::image_dimensions_ok(16000, 16000),
              "a product that would overflow a smaller type is still refused");
    }

    {
        check(limits::video_dimensions_ok(640, 480), "an ordinary video is accepted");
        check(!limits::video_dimensions_ok(65535, 65535),
              "the largest dimensions an IVF header can express are refused");
        check(!limits::video_dimensions_ok(limits::kMaxVideoDimension + 1, 1),
              "one pixel past the video per-side ceiling is refused");
        check(!limits::video_dimensions_ok(0, 0), "a zero-sized video is refused");
        check(!limits::video_dimensions_ok(-4, -4), "negative dimensions are refused");
    }

    {
        check(limits::decoded_audio_bytes_ok(0), "an empty decode is not an overflow");
        check(limits::decoded_audio_bytes_ok(44100 * 4 * 60),
              "a minute of CD-quality stereo is accepted");
        check(limits::decoded_audio_bytes_ok(limits::kMaxDecodedAudioBytes),
              "the ceiling itself is accepted");
        check(!limits::decoded_audio_bytes_ok(limits::kMaxDecodedAudioBytes + 1),
              "one byte past the ceiling is refused");
        check(!limits::decoded_audio_bytes_ok(-1),
              "a negative size is refused rather than wrapped");

        check(limits::video_packet_bytes_ok(1024), "an ordinary packet is accepted");
        check(!limits::video_packet_bytes_ok(0), "an empty packet is refused");
        check(!limits::video_packet_bytes_ok(limits::kMaxVideoPacketBytes + 1),
              "an oversized packet is refused");
    }

    // --- Header inspection on real encoded bytes --------------------------
    {
        int width = 0;
        int height = 0;

        const std::vector<uint8_t> ordinary = png_header(64, 64);
        check(limits::image_header_within_limits(ordinary.data(), ordinary.size(),
                                                 &width, &height),
              "an ordinary PNG header passes inspection");
        check(width == 64 && height == 64,
              "and the declared dimensions are reported back");

        // The whole point. A few dozen bytes asking for 12000x12000x4, which
        // is 576 MiB -- comfortably past this engine's ceiling, and comfortably
        // INSIDE what stb_image would have decoded on its own. So the refusal
        // below is this project's limit doing the work, not an incidental one
        // inherited from the decoder.
        const std::vector<uint8_t> bomb = png_header(12000, 12000);
        check(bomb.size() < 64, "the hostile input really is tiny");
        check(!limits::image_header_within_limits(bomb.data(), bomb.size(),
                                                  &width, &height),
              "a tiny PNG declaring an enormous image is refused before decoding");
        check(width == 12000 && height == 12000,
              "and what it asked for is reported, so the refusal can be logged");

        // Far past what any decoder will parse. Still refused, and refused
        // without reporting dimensions it could not read.
        const std::vector<uint8_t> extreme = png_header(0x7FFFFFFF, 0x7FFFFFFF);
        check(!limits::image_header_within_limits(extreme.data(), extreme.size(),
                                                  &width, &height),
              "the largest dimensions a PNG can declare are refused");
        check(width == 0 && height == 0,
              "an unreadable header reports no dimensions rather than guesses");

        // Not an image at all.
        const std::vector<uint8_t> garbage(64, 0xAB);
        check(!limits::image_header_within_limits(garbage.data(), garbage.size(),
                                                  nullptr, nullptr),
              "bytes with no readable header are refused, not guessed at");

        check(!limits::image_header_within_limits(nullptr, 0, nullptr, nullptr),
              "a null buffer is refused");
        check(!limits::image_header_within_limits(ordinary.data(), 0, nullptr, nullptr),
              "an empty buffer is refused");

        // A truncated header is unreadable, so it must not be trusted either.
        const std::vector<uint8_t> truncated(ordinary.begin(), ordinary.begin() + 12);
        check(!limits::image_header_within_limits(truncated.data(), truncated.size(),
                                                  nullptr, nullptr),
              "a truncated header is refused");
    }

    std::printf("%s\n", failures == 0 ? "=== PASS ===" : "=== FAIL ===");
    return failures == 0 ? 0 : 1;
}
