#pragma once
// ---------------------------------------------------------------------------
// audio_live_resume.h — PURE resume decision for a live replacement.
//
// A pause physically stops the HD streams, but the LOGICAL replacement
// instance—which asset, anchored to which frame, and until when—remains alive
// in the session. On resume, playback must continue FROM THE OFFSET dictated
// by the emulated clock, not from zero: clearing edges and retriggering removes
// the silence but introduces drift, which is explicitly forbidden.
//
// The chosen strategy is to RECREATE the stream at the offset rather than
// freeze the physical stream. It uses the same arithmetic as the take path
// (`(f - anchor) / fps`), survives Assets OFF/ON and workspace changes through
// the same code, and is compatible with a future unified mixer: the logical
// instance knows nothing about SDL.
//
// This header is PURE (no SDL and no core), so the decision can be tested
// without a session or ROM, following the same criterion as transport_gate.h.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace ayther {

/// What to do with a live instance when the transport resumes.
enum class LiveResumeAction : uint8_t {
    Restart,    ///< re-create the stream from `offset_seconds`
    Finished,   ///< the instance expired during the pause/bypass — discard it
};

struct LiveResumeDecision {
    LiveResumeAction action         = LiveResumeAction::Finished;
    double           offset_seconds = 0.0;   ///< only with Restart
};

constexpr uint64_t kLiveNoCut = UINT64_MAX;   // = drains in full

/// Has this instance been left behind? A loop with no tail dies at end_frame
/// (a loop is never left free — the tick_events contract); with a tail it
/// drains until cut_frame. A non-loop is governed by its cut (kLiveNoCut = it
/// is bounded only by the asset duration).
constexpr bool live_instance_over(uint64_t frame, uint64_t end_frame,
                                  uint64_t cut_frame, bool looping) noexcept {
    const uint64_t last =
        looping ? (cut_frame == kLiveNoCut ? end_frame : cut_frame)
                : cut_frame;
    return frame > last;
}

/// Decides the resume of ONE instance.
///
/// `frame`         emulated clock at resume time (steps taken while paused do
///                 advance and count: the offset comes from here, not from a
///                 wall clock).
/// `start_frame`   the anchor of the instance (the real rising edge).
/// `end_frame`     end of the window; kLiveNoCut = free one-shot (its end is
///                 the asset duration).
/// `cut_frame`     hard cut end+tail; kLiveNoCut = drains in full.
/// `looping`       a loop has no "natural end": it lives until its window
///                 (end, or cut if it has a tail) and its offset preserves
///                 PHASE — the modulo is applied by the player over the
///                 decoded PCM.
/// `fps`           game timing (PAL/NTSC); <= 1 uses 60.
/// `asset_seconds` asset duration if known, 0 = unknown (bounding by duration
///                 is left to the player, which returns success-without-stream
///                 if the offset lands past the end).
inline LiveResumeDecision live_resume_decide(uint64_t frame,
                                             uint64_t start_frame,
                                             uint64_t end_frame,
                                             uint64_t cut_frame,
                                             bool     looping,
                                             double   fps,
                                             double   asset_seconds) noexcept {
    if (live_instance_over(frame, end_frame, cut_frame, looping))
        return {LiveResumeAction::Finished, 0.0};

    const double f = fps > 1.0 ? fps : 60.0;
    const double offset = static_cast<double>(frame - start_frame) / f;

    // Non-loop with a known duration: past the end of the PCM there is nothing
    // left to resume — expired, even if its window/cut is still formally open.
    if (!looping && asset_seconds > 0.0 && offset >= asset_seconds)
        return {LiveResumeAction::Finished, 0.0};

    return {LiveResumeAction::Restart, offset};
}

/// Start offset in BYTES within the decoded PCM, aligned to a whole frame. For
/// a loop it applies the modulo: phase is preserved after any number of pauses.
/// Returns a multiple of `bytes_per_frame`, always < pcm_bytes for loops; for a
/// non-loop it may return >= pcm_bytes (nothing to play — the caller decides).
inline uint64_t live_resume_offset_bytes(double   offset_seconds,
                                         uint32_t freq,
                                         uint32_t bytes_per_frame,
                                         uint64_t pcm_bytes,
                                         bool     looping) noexcept {
    if (offset_seconds <= 0.0 || freq == 0 || bytes_per_frame == 0)
        return 0;
    uint64_t off = static_cast<uint64_t>(offset_seconds *
                                         static_cast<double>(freq)) *
                   bytes_per_frame;
    if (looping && pcm_bytes >= bytes_per_frame) {
        // Modulo over WHOLE frames of the asset: pcm_bytes may not be an exact
        // multiple of bytes_per_frame if the file comes out odd.
        const uint64_t whole = (pcm_bytes / bytes_per_frame) * bytes_per_frame;
        if (whole > 0) off %= whole;
    }
    return off;
}

}   // namespace ayther
