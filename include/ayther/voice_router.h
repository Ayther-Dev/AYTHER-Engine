// ---------------------------------------------------------------------------
// voice_router — the per-voice channel router (Phase 2).
//
// THE INVERSION. Until now substitution was SUBTRACTIVE: the chip played in
// full and channels were masked out with a mask derived from event WINDOWS. A
// window is a MODEL of the sound; the chip IS the sound, so every instant the
// window did not cover while the chip kept playing was a leak — the gaps
// between notes and the seam between Sequences are the same defect seen in two
// places.
//
// Here the default is flipped: the chip is MUTED and everything heard is
// produced by this router. A voice takes over the channel FROM THE CHIP'S OWN
// KEY-ON until the end of its tail. There is no window left to get wrong.
//
// THE TWO PIECES
//   ChipMirror  — a YM2612 (ymfm) and an SN76489 (PsgSynth) fed with the same
//                 write log the core receives, producing the 10 channels
//                 SEPARATELY. It is the substrate: it always runs, whether or
//                 not the voices take its output.
//   ChannelRouter — 10 slots. On every key-on it asks the policy what should
//                 play and points the slot at that source.
//
// WHY THE MIRROR ALWAYS RUNS: its register state IS the identity of the sound.
// If it were switched off while a voice is substituted, on return it would not
// know what timbre it was playing with — measured in Phase 0: starting the
// synthesiser without the previous state drops the envelope correlation from
// 0.975 to 0.889.
// ---------------------------------------------------------------------------

#pragma once

#include "ayther_core_ffi.h"
#include "psg_synth.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ymfm { class ym2612; }

namespace ayther {

class Ym2612Mirror;   // implementation detail: ym2612 with per-channel output

// ---------------------------------------------------------------------------
// ChipMirror
// ---------------------------------------------------------------------------
class ChipMirror {
public:
    static constexpr int kFmChannels  = 6;
    static constexpr int kPsgChannels = 4;
    static constexpr int kChannels    = kFmChannels + kPsgChannels;

    ChipMirror();
    ~ChipMirror();

    /// Cuts everything and returns to zero. For seeks and scene cuts.
    /// CAREFUL: this DISCARDS the register state, so it must be primed
    /// afterwards or the timbre comes out wrong — it is not like a sampler
    /// panic.
    void reset();

    /// PAL changes the lines per frame and with it the M-cycles per frame.
    void set_pal(bool pal);
    bool pal() const { return pal_; }

    /// Consumes one frame of writes and generates its per-channel PCM.
    /// Writes are placed by their `cycle`, so a note that starts mid-frame
    /// starts mid-frame — not at the boundary.
    void run_frame(const AytherAudioWrite* w, uint32_t n);

    /// Same as run_frame but WITHOUT generating audio: it only advances the
    /// register state. This is the priming after a seek — the earlier writes
    /// must be replayed to know what timbre each channel was playing with.
    void prime_frame(const AytherAudioWrite* w, uint32_t n);

    /// PCM of the isolated channel from the last run_frame: interleaved L,R,
    /// already DC-FREE (see dc_block_ in the .cpp — a silent channel does not
    /// sit at zero and the step would be a click when the voice is taken or
    /// released).
    const float* channel(int ch) const;

    /// Stereo samples produced by the last run_frame.
    size_t frame_samples() const { return frame_samples_; }

    /// Internal rate = MCLK/1008 = 53267.03 Hz on NTSC. It is the NATIVE rate
    /// of the YM2612; the PSG (which runs 4.2× faster) is averaged down to
    /// it.
    double rate() const;

private:
    void gen_until(uint64_t target_sample);

    std::unique_ptr<Ym2612Mirror> fm_;
    PsgSynth                      psg_;
    std::vector<float>            buf_[kChannels];
    float                         dc_x1_[kChannels][2] = {};
    float                         dc_y1_[kChannels][2] = {};
    bool                          dc_primed_ = false;
    uint64_t                      generated_    = 0;   // total FM samples
    uint64_t                      psg_ticks_    = 0;   // total PSG ticks
    uint64_t                      frame_base_   = 0;   // M-cycles at the start of the frame
    size_t                        frame_samples_ = 0;
    bool                          pal_           = false;
};

// ---------------------------------------------------------------------------
// StreamResampler — from the chip rate to the device rate, preserving phase.
//
// It has to be stateful rather than a per-block resample because the two ends
// are not in phase and the block changes size frame to frame: the Lab does not
// run at 16.7 ms per frame (the probe measures 20-33), so catch-up sometimes
// delivers several frames at once. Resetting the phase on every block would
// insert a jump at every seam — one click per frame.
// ---------------------------------------------------------------------------
class StreamResampler {
public:
    void reset();
    void set_rates(double src_hz, double dst_hz);

    /// Enqueues interleaved stereo samples at the SOURCE rate.
    void push(const float* in, size_t frames);

    /// Pulls up to `frames` stereo samples at the DESTINATION rate. Returns
    /// how many it could deliver — fewer than requested means input is missing,
    /// and the caller decides whether to pad with silence or wait.
    size_t pull(float* out, size_t frames);

    /// Destination samples already available from what is queued.
    size_t available() const;

private:
    std::vector<float> buf_;      // interleaved at the source rate
    double             pos_  = 0.0;   // read position, in source samples
    double             step_ = 1.0;   // src/dst
};

// ---------------------------------------------------------------------------
// A voice: a channel taken between a key-on and the end of its tail.
// ---------------------------------------------------------------------------
struct VoiceContext {
    uint8_t  chip    = 0;   ///< 0 = YM2612 (FM) · 1 = SN76489 (PSG)
    uint8_t  channel = 0;   ///< FM 0-5 · PSG 0-3
    uint32_t frame   = 0;   ///< frame of the key-on
    uint32_t cycle   = 0;   ///< M-cycle WITHIN the frame: the exact position
};

/// What plays on a channel. `chan` is the PCM of the mirror's isolated channel
/// for this stretch; a source may copy it, ignore it, or mix with it.
class IVoiceSource {
public:
    virtual ~IVoiceSource() = default;

    /// The chip triggered this voice. Returning false = I cannot play → it
    /// falls back to copy, which is the right degradation (the original is
    /// heard, not silence).
    virtual bool begin(const VoiceContext& ctx) = 0;

    /// Adds `n` stereo samples of `chan` into `out`. Both are interleaved.
    virtual void render(const float* chan, float* out, size_t n) = 0;

    /// The chip released the key. This is not the end: the tail is still to come.
    virtual void key_off() = 0;

    /// The tail finished. A copy never finishes —the mirror already models its
    /// own release— so it returns false and the slot is freed only on the next
    /// key-on.
    virtual bool finished() const = 0;
};

/// Copies the isolated channel as-is. It is the default and the degradation of
/// everything else: if something cannot play, the original is heard.
class CopySource final : public IVoiceSource {
public:
    bool begin(const VoiceContext&) override { return true; }
    void render(const float* chan, float* out, size_t n) override;
    void key_off() override {}
    bool finished() const override { return false; }
};

/// Nothing plays. This is what makes "nothing leaks through" structural: it is
/// not a mute that has to be timed right, it is a source that does not emit.
class SilentSource final : public IVoiceSource {
public:
    bool begin(const VoiceContext&) override { return true; }
    void render(const float*, float*, size_t) override {}
    void key_off() override {}
    bool finished() const override { return false; }
};

/// Copies the channel SCALED. It is what was missing between "copy" and
/// "mute" so that turning a bus down also reaches the ORIGINAL audio — until
/// now the game music either played in full or went silent.
///
/// THE FACTOR IS READ PER RENDER, NOT COPIED IN `begin()`.
///
/// This is the trap this repo already paid for twice: the per-Sequence gain and
/// the bus volume, both applied only when the sound starts. In a music loop
/// "when it starts" is NEVER — the user moves the slider and nothing happens
/// until the next time the music starts from scratch, which may be never.
///
/// That is why it stores a POINTER to the factor and not its value: the volume
/// is owned by the policy, which changes it when the user moves the control,
/// and the source reads it every time it fills.
class GainSource final : public IVoiceSource {
public:
    /// `gain` has to outlive the source. In practice it points at the
    /// session's per-bus gain array, which lives as long as the session.
    explicit GainSource(const float* gain) : gain_(gain) {}
    bool begin(const VoiceContext&) override { return true; }
    void render(const float* chan, float* out, size_t n) override;
    void key_off() override {}
    bool finished() const override { return false; }
private:
    const float* gain_ = nullptr;
};

/// Who decides. The real implementation (Phase 4) translates the assignments
/// that already exist —the SF2 inst_assign, audio_event_assign, the mutes—
/// into sources.
class VoicePolicy {
public:
    virtual ~VoicePolicy() = default;
    /// What plays on this channel from this key-on. nullptr = copy.
    /// The pointer is NOT owned: the policy owns its sources.
    virtual IVoiceSource* choose(const VoiceContext& ctx) = 0;
};

// ---------------------------------------------------------------------------
// ChannelRouter
// ---------------------------------------------------------------------------
class ChannelRouter {
public:
    static constexpr int kVoices = ChipMirror::kChannels;

    ChannelRouter();

    void set_policy(VoicePolicy* p) { policy_ = p; }
    ChipMirror&       mirror()       { return mirror_; }
    const ChipMirror& mirror() const { return mirror_; }

    /// Cuts everything. Leaves the slots on copy and clears the mirror.
    void reset();

    /// One frame: consumes the log, resolves the key-ons/key-offs at their
    /// exact positions, and leaves the mix in `out` (interleaved,
    /// `frame_samples()` samples). `out` is OVERWRITTEN, not accumulated.
    void tick(const AytherAudioWrite* w, uint32_t n, uint32_t frame,
              std::vector<float>& out);

    /// Telemetry — following the synth_stats precedent: without this,
    /// diagnosing by ear takes several rounds.
    struct Stats {
        uint64_t ticks = 0, key_ons = 0, key_offs = 0;
        uint64_t substituted = 0;   // key-ons that did NOT fall back to copy
        uint64_t resets = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    /// A key-on/key-off detected in the writes, with its position.
    struct KeyEvent { uint8_t chip, channel; bool on; uint32_t cycle; };

    void scan_keys(const AytherAudioWrite* w, uint32_t n,
                   std::vector<KeyEvent>& out);

    ChipMirror   mirror_;
    VoicePolicy* policy_ = nullptr;
    CopySource   copy_;
    SilentSource silent_;

    IVoiceSource* voice_[kVoices] = {};
    uint8_t       fm_key_ = 0;              // key-on state of the 6 FM channels
    uint8_t       psg_att_[4] = { 0xF, 0xF, 0xF, 0xF };   // current attenuation
    int           psg_latch_ = 3;
    Stats         stats_;
};

}  // namespace ayther
