#pragma once
// ---------------------------------------------------------------------------
// audio_hd_mixer.h — HD voice mixer on the main stream's SAMPLE timeline.
//
// THE PROBLEM IT SOLVES. HD replacements used to run in their own SDL streams:
// they started "now" according to wall-clock time while the original traveled
// through `emu_stream_` with a ~70 ms DRC cushion. The phase between original
// and HD therefore depended on backlog, stalls, and catch-up size. Here every
// voice is PLACED at an absolute sample on the staged block timeline and mixed
// INSIDE that block: a trigger at frame N lands on the same sample under 1x1
// execution or catch-up 16, and everything—original, router, and HD—crosses
// the SAME DRC/backlog. Pausing one stream pauses everything.
//
// WHAT THIS MODULE IS. Mixing only: voices with already decoded and converted
// PCM (S16 stereo at 44100 Hz, guaranteed by the AudioPlayer cache), sample
// placement, phase-preserving loops, gain, cut fades, and the per-frame
// lifetime contract (end + tail). It does NOT touch SDL: mixing is a pure
// function over a buffer, so the 1x1-vs-catch-up identity oracle can be exact,
// byte for byte, without a device.
//
// The FRAME-based lifecycle (`end_frame`/`cut_frame`) deliberately remains in
// frames: it is the same contract used by `tick_events` and session windows.
// Frame-to-sample conversion lives in ONE place (start placement), rather than
// being scattered across every sweep.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <memory>
#include <vector>

/// Shared mix-ready PCM: interleaved stereo S16 at 44100 Hz. Shared
/// (shared_ptr) because N voices of the same asset must not duplicate the
/// decode, and because a voice must survive a cache invalidation without
/// dangling.
using HdMixPcm = std::shared_ptr<const std::vector<int16_t>>;

class HdMixer {
public:
    /// Cut fade, in FRAMES at 44100 (~60 ms — the same audible criterion as
    /// the stop_sfx_by_key fade in the stream path).
    static constexpr uint32_t kFadeFrames = 2646;

    struct Voice {
        uint64_t key          = 0;
        HdMixPcm pcm;                       ///< stereo S16 44100 (mix-ready)
        uint64_t start_sample = 0;          ///< ABSOLUTE start sample
        size_t   pos          = 0;          ///< cursor in FRAMES within the pcm
        float    gain         = 1.0f;
        bool     looping      = false;
        bool     event        = false;      ///< the play_event_hd contract
        uint64_t end_frame    = UINT64_MAX; ///< end of window (FRAME domain)
        uint64_t cut_frame    = UINT64_MAX; ///< end + tail; MAX = drains
        /// > 0 = fade frames remaining (the voice dies on reaching 0).
        uint32_t fade_left    = 0;
        /// The number of frames the current fade STARTED with — the denominator
        /// of the ramp. Without this, an authored fade of 30,000 frames would be
        /// divided by `kFadeFrames` and would sit pinned at gain > 1 until the
        /// end.
        uint32_t fade_span    = kFadeFrames;
        /// FADE_OUT end policy, in frames at 44100. 0 = no fade (hard_cut or
        /// tail, as configured). The ramp starts on passing `end_frame` and
        /// reaches silence `fade_frames` later — which is what the author asked
        /// for: "end in silence N frames after the limit".
        uint32_t fade_frames  = 0;
        /// Loop REGION within the asset, in FRAMES. `loop_end == 0` = no
        /// region: the whole asset repeats, which was the only option before.
        ///
        /// It only applies when `looping`: a region on a one-shot would mean
        /// nothing, and trimming it by that region would cut the sound in half.
        size_t   loop_begin   = 0;
        size_t   loop_end     = 0;
        /// Telemetry: how many samples late the start arrived (0 = in phase).
        uint64_t late_samples = 0;
    };

    /// Starts (or retriggers) a voice. `start_sample` = ABSOLUTE position on
    /// the stream timeline where its first frame falls — the caller computes it
    /// as (timeline + offset of the current frame within the staged block),
    /// which is what makes the placement immune to catch-up.
    /// `offset_frames` starts from the middle of the asset (resume, late
    /// trigger): for a loop it enters through the modulo (phase preserved).
    /// Retriggering the same key fades the previous voice out (the usual
    /// retrigger contract). Returns false with no usable PCM.
    bool start(uint64_t key, HdMixPcm pcm, uint64_t start_sample,
               uint64_t offset_frames, float gain, bool looping, bool event,
               uint64_t end_frame, uint64_t cut_frame,
               uint32_t fade_frames = 0,
               // loop region in FRAMES (0,0 = the whole asset).
               size_t loop_begin = 0, size_t loop_end = 0) {
        if (!pcm || pcm->size() < 2) return false;
        stop(key);   // retrigger: the previous one fades out, not overlapped
        Voice v;
        v.key = key;
        v.start_sample = start_sample;
        v.gain = gain;
        v.looping = looping;
        v.event = event;
        v.end_frame = end_frame;
        v.cut_frame = cut_frame;
        v.fade_frames = fade_frames;
        const size_t frames = pcm->size() / 2;
        // The region is SANITISED here and not while mixing: a `loop_end`
        // larger than the asset or inverted would read past the buffer, and
        // checking it per sample would mean paying that check 44,100 times a
        // second for something that never changes over the life of the voice.
        if (loop_end > frames) loop_end = frames;
        if (loop_begin >= loop_end) { loop_begin = 0; loop_end = 0; }
        v.loop_begin = loop_begin;
        v.loop_end   = loop_end;
        v.pos = looping ? static_cast<size_t>(offset_frames % frames)
                        : static_cast<size_t>(offset_frames);
        // Resuming inside a loop that has a region: if the offset falls
        // outside the cycle, it enters at its phase WITHIN the region. Leaving
        // it before the start would make the resume replay the intro again.
        if (v.loop_end && v.pos >= v.loop_end) {
            const size_t span = v.loop_end - v.loop_begin;
            v.pos = v.loop_begin + ((v.pos - v.loop_begin) % span);
        }
        v.pcm = std::move(pcm);
        // Non-loop with the offset past the end: nothing to mix — success
        // without a voice, the same contract as the stream path.
        if (!v.looping && v.pos >= frames) return true;
        voices_.push_back(std::move(v));
        ++started_;
        return true;
    }

    /// Fade cut (~60 ms) of every voice of a key. true if it reached one that
    /// was not already fading — the same observable contract as
    /// stop_sfx_by_key (telling "I cut it short" apart from "there was
    /// nothing").
    bool stop(uint64_t key) {
        bool cut = false;
        for (Voice& v : voices_)
            if (v.key == key && v.fade_left == 0) {
                v.fade_left = kFadeFrames;
                v.fade_span = kFadeFrames;   // the cut one, not the authored one
                cut = true;
            }
        return cut;
    }

    /// Immediate HARD cut (pause / seek): no fade, no residue — the staging is
    /// discarded whole on those paths and the voice must not drain.
    void cut_all() { voices_.clear(); }

    /// Hard cut filtered by class: the session paths distinguish one-shots
    /// (stop_all_sfx) from event streams (stop_all_events) and the unified mode
    /// has to respect that boundary — a seek that invalidates the take events
    /// must not silence an unrelated one-shot.
    void cut_all_of(bool event) {
        for (size_t i = voices_.size(); i-- > 0; )
            if (voices_[i].event == event)
                voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
    }

    bool stop_hard(uint64_t key) {
        const size_t n = voices_.size();
        for (size_t i = voices_.size(); i-- > 0; )
            if (voices_[i].key == key)
                voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
        return voices_.size() != n;
    }

    /// Live gain of a key (the slider of a Sequence while it plays).
    bool set_gain(uint64_t key, float gain) {
        bool any = false;
        for (Voice& v : voices_)
            if (v.key == key && v.fade_left == 0) { v.gain = gain; any = true; }
        return any;
    }

    /// The per-FRAME lifetime contract — an exact mirror of tick_events: past
    /// cut_frame the voice dies even with PCM left; a loop past its end_frame
    /// dies (no tail) or stops looping and drains until cut (tail).
    void tick_frame(uint64_t frame) {
        for (size_t i = voices_.size(); i-- > 0; ) {
            Voice& v = voices_[i];
            // FADE_OUT. On passing the limit the voice does not die abruptly:
            // it starts a ramp of `fade_frames` frames and switches itself off
            // on reaching silence. It is an end policy ALTERNATIVE to tail
            // —they do not stack— and that is why it is evaluated BEFORE
            // cut_frame: the hard cut of the window must not interrupt the ramp
            // the author asked for. A fade already in progress is not
            // restarted.
            if (v.fade_frames && frame > v.end_frame) {
                if (v.fade_left == 0) {
                    v.fade_left = v.fade_frames;
                    v.fade_span = v.fade_frames;
                    v.looping   = false;   // stops re-feeding itself: it fades out
                }
                continue;
            }
            if (frame > v.cut_frame) {
                voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            if (v.looping && frame > v.end_frame) {
                if (v.cut_frame == UINT64_MAX || v.cut_frame <= v.end_frame)
                    voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
                else
                    v.looping = false;   // tail: drains the rest until cut
            }
        }
    }

    /// ADDS the voices onto `out` (interleaved stereo S16, `frames` frames),
    /// which represents samples [block_start, block_start + frames) of the
    /// timeline. Per-sample placement: a voice whose start falls WITHIN the
    /// block enters at its exact offset — that is what makes the result
    /// identical between 1×1 and catch-up. A voice scheduled for a block that
    /// already passed enters at the beginning and records its lateness (skew
    /// telemetry).
    void mix_into(int16_t* out, size_t frames, uint64_t block_start) {
        if (!out || frames == 0) return;
        for (size_t i = voices_.size(); i-- > 0; ) {
            Voice& v = voices_[i];
            if (v.start_sample >= block_start + frames) continue;   // future
            size_t at = 0;   // offset in FRAMES within the block
            if (v.start_sample > block_start) {
                at = static_cast<size_t>(v.start_sample - block_start);
            } else if (v.start_sample < block_start && v.pos == 0 &&
                       v.late_samples == 0 && !v.looping) {
                // First block of a voice that arrived LATE (e.g. scheduled
                // during a stall): record the lateness once.
                v.late_samples = block_start - v.start_sample;
                skew_samples_ += v.late_samples;
                if (v.late_samples > max_skew_) max_skew_ = v.late_samples;
            }
            const std::vector<int16_t>& pcm = *v.pcm;
            const size_t pcm_frames = pcm.size() / 2;
            bool done = false;
            // Where the cycle ends and where it returns to. With no region it
            // is the whole asset, which is the long-standing behaviour.
            const size_t cycle_end = (v.looping && v.loop_end) ? v.loop_end : pcm_frames;
            const size_t cycle_beg = (v.looping && v.loop_end) ? v.loop_begin : 0;
            for (size_t f = at; f < frames && !done; ++f) {
                if (v.pos >= cycle_end) {
                    if (!v.looping) { done = true; break; }
                    // It returns to the START OF THE REGION, not of the file:
                    // that is what lets a track have an intro and then cycle
                    // without exporting two files.
                    v.pos = cycle_beg;
                }
                float g = v.gain;
                if (v.fade_left > 0) {
                    g *= static_cast<float>(v.fade_left) /
                         static_cast<float>(v.fade_span ? v.fade_span : 1);
                    if (--v.fade_left == 0) done = true;
                }
                const size_t s = v.pos * 2;
                const size_t o = f * 2;
                for (int c = 0; c < 2; ++c) {
                    const int32_t mixed = static_cast<int32_t>(out[o + c]) +
                        static_cast<int32_t>(
                            static_cast<float>(pcm[s + c]) * g);
                    out[o + c] = static_cast<int16_t>(
                        mixed > 32767 ? 32767 : (mixed < -32768 ? -32768 : mixed));
                }
                ++v.pos;
            }
            if (done || (!v.looping && v.pos >= pcm_frames &&
                         v.fade_left == 0 && v.start_sample <= block_start))
                voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
        }
        mixed_samples_ += frames;
    }

    size_t voice_count() const { return voices_.size(); }
    size_t event_voice_count() const {
        size_t n = 0;
        for (const Voice& v : voices_) n += v.event ? 1 : 0;
        return n;
    }

    // ---- Telemetry (Phase 0) --------------------------------------------
    uint64_t started() const { return started_; }
    uint64_t mixed_samples() const { return mixed_samples_; }
    /// Accumulated lateness samples from voices that reached an already-past
    /// block (a sustained 0 = the placement works; growing = some path is
    /// scheduling against a timeline that was already flushed).
    uint64_t skew_samples() const { return skew_samples_; }
    uint64_t max_skew_samples() const { return max_skew_; }

private:
    std::vector<Voice> voices_;
    uint64_t started_       = 0;
    uint64_t mixed_samples_ = 0;
    uint64_t skew_samples_  = 0;
    uint64_t max_skew_      = 0;
};
