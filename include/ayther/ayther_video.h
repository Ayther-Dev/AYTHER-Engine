#pragma once
// ---------------------------------------------------------------------------
// ayther_video.h — video decoder for the Kinematic.
//
// WHAT IT IS: a VP9 clip in an IVF container, decoded to BGRA8 ready to upload
// into a texture. It knows nothing about Vulkan or the session: you ask it for
// a frame index and it returns pixels.
//
// WHY VP9/IVF AND NOT FFmpeg — it is not taste, it is the project's GPL
// boundary. `ayther_engine` is a STATIC library; the FFmpeg core is LGPL-2.1+,
// so linking it would force dynamic distribution or shipping relinkable
// objects, and any --enable-gpl component would make it GPL. libvpx is BSD-3 +
// patent grant. IVF is 32 bytes of header and 12 per frame, which means the
// demuxer fits in this file and drags in no libavformat. The ENCODER remains an
// EXTERNAL ffmpeg.exe from PATH (lab/src/app/ffmpeg_pipe.h): a separate
// process, no linking, no licence question.
//
// WHY THE HEADER DOES NOT INCLUDE vpx: if `vpx/vpx_decoder.h` came in here, the
// libvpx include dir would have to be PUBLIC in the engine CMake, and the Lab,
// the runtime and the tools would start depending on an OPTIONAL library.
// Pimpl.
//
// WITHOUT libvpx (AYTHER_HAVE_VPX off) this still compiles: open() returns
// false with a reason and validate() rejects. A contributor without libvpx is
// not blocked, and the bake never bakes a video without validating it.
//
// STREAMING: the clip does NOT reside in RAM. It reads from a `VideoSource` —
// the pack by range, or a file — and keeps only the packet index, the current
// packet and the converted frame. It used to copy the whole .ivf, and that was
// the real reason for the bake's 32 MB per-video cap: not the format, but that
// `ayther_pack_read` is all-or-nothing. With the cap removed, an 8K clip takes
// the same space as a 3 s one.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace ayther {

/// Where the clip bytes come from. It exists so the player does not depend on
/// having the file in memory: the pack implements it with
/// `ayther_pack_read_range` (verifying chunk by chunk against the signed index)
/// and disk with a FILE*.
///
/// The clip keeps it for its whole life, so the source has to outlive it. With
/// the pack that means reopening the pack INVALIDATES the open clips — the
/// session clears its cache in `set_pack`.
struct VideoSource {
    virtual ~VideoSource() = default;
    /// Total size of the .ivf in bytes.
    virtual uint64_t size() const = 0;
    /// Copies EXACTLY `len` bytes from `off` into `dst`. false = it could not
    /// (range out of bounds, IO, or a chunk that fails verification): a short
    /// read is a failure, not a partial result — the demuxer needs that
    /// guarantee so it does not confuse "it was cut short" with "I reached the
    /// end".
    virtual bool read(uint64_t off, size_t len, uint8_t* dst) = 0;
    /// How much RAM the source keeps. Zero for the ones reading from disk or
    /// from the pack; the file size for the one holding it in memory. It is
    /// here so that "this is streaming" is a MEASUREMENT and not an assertion —
    /// the same criterion as `VideoClip::cost_ms`.
    virtual uint64_t resident_bytes() const { return 0; }
};

/// Source over a file on disk. Used by authoring (the Lab works against the
/// project, with no pack) and by the bake while validating.
std::unique_ptr<VideoSource> video_source_from_file(const std::string& path);

/// Source over a pack entry, read BY RANGE. `pack` is an `AyArchive*` (taken
/// as void* so this header, which half the world includes, does not drag in the
/// core FFI).
///
/// Every read verifies the chunks it touches against the signed index, so
/// streaming does NOT loosen the integrity guarantee: it changes the unit of
/// verification from the entry to the chunk.
///
/// It does NOT own the pack. The pack has to outlive the clip — the session
/// guarantees that by clearing its video cache before closing it (`set_pack`).
/// Returns nullptr if the entry is not addressable by range.
std::unique_ptr<VideoSource> video_source_from_pack(void* pack,
                                                    const std::string& logical_path);

// ---------------------------------------------------------------------------
// BAKED PACKET INDEX — the `<name>.ivf.idx` sidecar
//
// WHAT IT SOLVES. Since streaming, the clip does not reside in memory: the
// packet of the frame in question is read and nothing else. But to know WHERE
// each packet is, the whole IVF has to be swept on open — a sequential pass
// that costs the same as reading it whole (measured 810 MB/s with SHA-256 in
// the loop). It is not a regression: it is the cost streaming did not remove.
// On today's Kinematics that is 60-83 ms and is hidden by prewarm; on a 30 s 8K
// clip it would be ~2.4 s of freeze right as the scene the artist wants to look
// good begins.
//
// WHY A PACK ASSET AND NOT A NEW FORMAT. The sidecar enters the pack like any
// other entry, so it is covered by `integrity.toml` and by the signature with
// NO new machinery, and it is verified on read like everything else.
//
// THE INDEX IS NOT AN AUTHORITY OVER THE CONTENT. It says WHERE to look, just
// like the ZIP central directory's `data_start`: if it points at an offset that
// holds no VP9 keyframe, the decode of that frame fails and THAT frame is lost
// — nothing is corrupted. The per-chunk hashes remain what guarantees the
// bytes. That is why the dimensions and the time base are still read from the
// IVF header and NOT from the sidecar: an `.idx` that lied about the frame size
// would be an authority that is not its to hold.
//
// THE FALLBACK HAS TO EXIST. Earlier packs do not carry one, and in the Lab the
// step asset is an absolute path to a loose `.ivf` with no pack at all. With no
// sidecar —or with one that does not match the source— it sweeps, as always.
// ---------------------------------------------------------------------------

/// Sidecar suffix: "clip.ivf" → "clip.ivf.idx".
///
/// This relationship is EXEMPTED from the pack's own rule, and it is worth
/// writing down why. The contract says no pack relationship is derived from a
/// name: the TOML explicitly names what it references. Here that is not
/// possible, and it is not a debt.
///
/// The index is PER TIER: every `assets/tiers/<t>/<id>` has its own `.idx`,
/// because each tier is a different file with its packets at different
/// positions. A field in the TOML would name ONE, and the TOML does not know
/// —nor should it— which tier the pack will be opened with: that is decided by
/// the display of the machine running it.
///
/// The suffix, by contrast, travels ATTACHED to the logical name and goes
/// through the same per-tier resolution as the clip: asking for `<id>.idx`
/// returns the index of the active tier, without anyone having to enumerate
/// them. The derivation is what makes the relationship correct, not what
/// weakens it.
inline std::string video_index_path(const std::string& ivf_path) {
    return ivf_path + ".idx";
}

/// Serialises the packet index of `src` (it sweeps the IVF ONCE). Used by the
/// bake, which already pays for that sweep to require all-keyframes.
/// false = the source is not a usable IVF (reason in *err).
bool video_index_build(VideoSource& src, std::vector<uint8_t>* out,
                       std::string* err);

/// How many frames a well-formed `.idx` declares. 0 = it is not one. It exists
/// so the bake can report how many it baked without re-parsing by hand.
uint32_t video_index_frames(const uint8_t* idx, size_t n);


/// A luma or chroma plane exactly as the decoder delivers it. `stride` is NOT
/// `w`: libvpx aligns the rows, and reading it as if it were tightly packed
/// shears the image.
struct VideoPlane {
    const uint8_t* data   = nullptr;
    uint32_t       stride = 0;
    uint32_t       w      = 0;
    uint32_t       h      = 0;
};

/// A decoded frame, in I420 and WITHOUT A COPY.
///
/// The planes point at the decoder's own frame and live until the next
/// decode() — the consumer uploads them and forgets them, it does not store
/// them. This used to hold a BGRA8 converted on the CPU, and it was the biggest
/// expense of the whole player: measured 12.5 of the 20.3 ms per frame at
/// 2304×2016 (62%), plus 17.7 MB of buffer that WERE the entire residency of
/// the clip. The conversion is now done by the fragment shader, which
/// additionally uploads 1.5 bytes per pixel instead of 4.
struct VideoFrameView {
    VideoPlane  y, u, v;           ///< I420: u and v halved on both axes
    uint32_t    w     = 0;
    uint32_t    h     = 0;
    uint32_t    index = 0;         ///< clip frame that was decoded

    /// Changes ONLY when the content changed. The renderer uses it to skip the
    /// re-upload when the video did not advance (paused, or a UI re-render over
    /// the same FrameView): without it, the full memcpy is paid for every
    /// interface frame while stopped.
    uint64_t    seq   = 0;
};

/// THE SPECIFICATION of the conversion the shader performs, in C++.
///
/// BT.601 limited range —what ffmpeg produces by default for SD— with the
/// INTEGER arithmetic `i420_to_bgra` used on the CPU before the conversion
/// moved to the fragment shader. It exists so the oracle can compare the GPU
/// result against a number computed here: without an executable reference, "the
/// shader converts correctly" cannot be asserted.
///
/// The coefficients are NOT changed without re-measuring shaders/video.frag
/// against this.
inline void video_i420_to_bgra_px(int Y, int U, int V, uint8_t out_bgra[4]) {
    const int C = Y - 16, D = U - 128, E = V - 128;
    const int r = (298 * C + 409 * E + 128) >> 8;
    const int g = (298 * C - 100 * D - 208 * E + 128) >> 8;
    const int b = (298 * C + 516 * D + 128) >> 8;
    auto clamp8 = [](int v) -> uint8_t {
        return uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    out_bgra[0] = clamp8(b);
    out_bgra[1] = clamp8(g);
    out_bgra[2] = clamp8(r);
    out_bgra[3] = 255;   // the video is opaque: it covers the whole screen
}

/// Result of validating an .ivf without fully opening it. Used by the bake.
struct VideoInfo {
    uint32_t frames = 0;
    uint32_t w      = 0;
    uint32_t h      = 0;
    /// Time base declared in the IVF header (0 = absent). The player NEEDS it:
    /// without it, it assumed the clip runs at the game fps and a 30 fps clip
    /// over a 59.92 take came out at double speed.
    double   fps    = 0.0;
    bool     all_keyframes = false;
};

/// MEASURED cost of playing a clip on THIS machine.
///
/// It exists because a clip can be perfectly valid —all-keyframes, decodable—
/// and still not fit the budget: measured 20.3 ms per frame at 2304×2016
/// against 16.7 at 60 fps, i.e. ~49 fps. And it cannot be tabulated in advance,
/// because it depends on the machine's cores; so it is not estimated, it is
/// measured.
struct VideoProbe {
    /// Per-frame average of the SECOND HALF of the measurement — see
    /// `ms_burst`.
    double   ms_decode  = 0.0;
    double   ms_convert = 0.0;   ///< I420→BGRA (removed by uploading 3 R8 planes)
    /// The MOST EXPENSIVE frame of the second half. It is kept apart from the
    /// average because a hitch is produced by the bad frame, not by the mean: a
    /// clip that averages 15 ms and spikes to 40 looks worse than an even 18.
    double   ms_worst   = 0.0;
    /// Average of the FIRST half. It exists as a GUARD on the measurement
    /// itself: if CPU turbo inflated the start, an `ms_burst` far below
    /// `ms_per_frame()` would expose it, and nobody could "improve" the number
    /// by shortening the measurement without it showing. On the machine it was
    /// calibrated on there was no measurable drop (20.1 against 20.1 ms), but
    /// the guarantee cannot depend on that: on a laptop that throttles, there
    /// will be one.
    double   ms_burst   = 0.0;
    uint32_t sampled    = 0;     ///< frames of the second half (0 = not measured)
    uint32_t w = 0, h = 0;
    unsigned threads    = 0;

    double ms_per_frame() const { return ms_decode + ms_convert; }
    double max_fps()     const {
        return ms_per_frame() > 0.0 ? 1000.0 / ms_per_frame() : 0.0;
    }
};

/// @brief Move-disabled owner for a streamed or memory-backed VP9 clip.
///
/// Opening transfers ownership of the VideoSource. Decoded frame views are
/// borrowed from the clip and are invalidated by the next successful decode,
/// reopen, or destruction. The class is not thread-safe.
///
/// Input dimensions, packet ranges, and sidecar indexes are untrusted and must
/// pass bounds validation before allocation or decoding.
class VideoClip {
public:
    VideoClip();
    ~VideoClip();
    VideoClip(const VideoClip&)            = delete;
    VideoClip& operator=(const VideoClip&) = delete;

    /// Demuxes the IVF over `src` and opens the VP9 decoder. The clip keeps
    /// the source: nothing of the content is copied to RAM except the packet
    /// index.
    ///
    /// The index sweep reads the file SEQUENTIALLY and in blocks, not jumping
    /// from header to header. With the pack source, asking for 12 bytes costs a
    /// whole chunk verification, so jumping would pay for that chunk once PER
    /// FRAME; sequentially, each chunk is verified exactly once.
    bool open(std::unique_ptr<VideoSource> src, std::string* err);

    /// Same, with the BAKED INDEX: if `idx` is a well-formed sidecar and
    /// COHERENT with the source, the whole sweep is skipped. If it is not —or
    /// if `idx` is null— it sweeps, and says nothing about it: an old pack
    /// carries no sidecar and that is not an error.
    ///
    /// "Coherent" is checked: every packet has to fall INSIDE the file and none
    /// may exceed the size cap. It is a PLAUSIBILITY validation, not a content
    /// one — whether an offset points at a real keyframe is decided by the
    /// decoder, and if it does not, that frame is lost and nothing more (see
    /// the note above on why the index is not an authority).
    bool open(std::unique_ptr<VideoSource> src, const uint8_t* idx, size_t idx_n,
              std::string* err);

    /// Same, over a buffer already in RAM. It COPIES the bytes (the caller's
    /// buffer need not outlive the clip). It is the path for small clips and
    /// for anything that already has the file read; for a large video, use the
    /// `VideoSource` overload.
    bool open(const uint8_t* ivf, size_t n, std::string* err);

    bool     is_open()     const;
    uint32_t frame_count() const;
    uint32_t width()       const;
    uint32_t height()      const;
    /// fps declared in the IVF time base (0 = absent). The player maps GAME
    /// frames to CLIP frames with this ratio.
    double   fps()         const;

    /// Decodes frame `index` and returns its view, or nullptr on failure.
    ///
    /// IDEMPOTENT: asking for the same index twice does NOT re-decode and does
    /// NOT move `seq`. It is a requirement, not an optimisation — produce_frame
    /// is not 1:1 with emulated frames (the compose re-produces,
    /// replay_invalidate re-produces, the export re-produces), and without
    /// idempotency the video would advance per re-render rather than per game
    /// frame.
    const VideoFrameView* decode(uint32_t index);

    /// Validates an .ivf without instantiating anything. For the bake: a video
    /// that cannot be decoded at the destination is never baked.
    ///
    /// It requires ALL frames to be keyframes, and that is phase 1 and not a
    /// preference: it makes decoding frame N a SINGLE packet, with no GOP walk
    /// and no seek state machine. It is what the decision "the position comes
    /// from the content" actually demands — a jump into the middle has to land
    /// on the right frame without decoding everything before it.
    static bool validate(const uint8_t* ivf, size_t n,
                         VideoInfo* out, std::string* err);

    /// Same, over a source. The RAM it uses is the LARGEST packet of the clip,
    /// not the clip: that is what lets the bake validate an 8K video without
    /// reserving a gigabyte.
    static bool validate(VideoSource& src, VideoInfo* out, std::string* err);

    /// Whether the engine was built with libvpx. Without this, everything else
    /// fails with a legible reason instead of failing strangely.
    static bool available();

    /// Accumulated cost, in ms, split into its two halves. It exists because
    /// they are two DIFFERENT problems with different fixes, and the breakdown
    /// is what settled it: the I420→BGRA conversion was 62% of the per-frame
    /// cost and was removed by moving it to the fragment shader; the VP9 decode
    /// cannot be moved, and only comes down with less resolution or more
    /// threads.
    ///
    /// Since then `convert` sits at ZERO: there is no CPU conversion. It is
    /// kept in the signature —and not deleted— because it is the MEASURE that
    /// it stays at zero; if it ever rises again, the oracle sees it.
    void cost_ms(double* decode, double* convert, uint32_t* frames) const;

    /// Threads the VP9 decoder was opened with (row-mt). Zero if it is not
    /// open. It sits next to the cost because they are the same number read two
    /// ways: 38.9 ms per frame on one thread does not say the same as on
    /// eight.
    unsigned threads() const;

    /// RAM the open clip keeps: packet index + current packet + converted
    /// frame + whatever the source retains. Because of streaming it does NOT
    /// depend on the video size, and that is the whole thesis — so it is a
    /// number that has to be inspectable, not a promise
    /// (tools/video_stream_smoke).
    uint64_t resident_bytes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Measures the cost of playing `clip` by decoding CONSECUTIVE frames for at
/// least `measure_ms`. false if it could not be measured (empty clip, or a
/// decode that failed) — and in that case the caller must NOT invent a verdict.
///
/// WHY IT TAKES MORE THAN A SECOND. The per-frame cost is not a constant of the
/// machine: in a short burst the CPU may be in turbo and deliver more than it
/// sustains, and a half-second measurement would measure exactly that. It
/// measures for more than a second and reports the average of the SECOND HALF,
/// which is post-boost; `ms_burst` leaves the first half visible as a guard
/// (see VideoProbe).
///
/// Calibrated against the real sustained cost in tools/video_stream_smoke,
/// which decodes hundreds of frames in a row and compares: the probe has to
/// land in the same order of magnitude, and if it drifts, the warning shown to
/// the artist lies.
///
/// CONSECUTIVE and not spread out: the conversion writes a whole frame per
/// frame, and in playback that happens 60 times a second in a row. Skipping
/// gives the prefetcher conditions playback does not have. It starts in the
/// middle of the clip and wraps around: a video usually starts on black, and a
/// black frame decodes far faster than a busy one.
///
/// It takes an ALREADY OPEN clip so it does not decide where the bytes come
/// from: both what is authored on disk and what will run from the pack can be
/// measured.
bool video_probe(VideoClip& clip, double measure_ms, VideoProbe* out);

}  // namespace ayther
