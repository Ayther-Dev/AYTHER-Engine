// ---------------------------------------------------------------------------
// PsgSynth — our own SN76489 (PSG), with PER-CHANNEL output. Phase 1.
//
// WHY OUR OWN. ymfm covers the YM2612 but not the SN76489, and the
// implementations in circulation (MAME, GPGX itself) are GPL: ayther_engine is
// a STATIC library and adding GPL contaminates the whole of it. The chip is
// genuinely small —three square-wave generators and an LFSR— so writing it is
// cheaper than arguing about licences.
//
// `gpgx-src/core/sound/psg.c` is the reference for BEHAVIOUR (constants, volume
// table, noise feedback network), not for code.
//
// INTERNAL RATE. The chip runs at MCLK/15 and divides by 16, i.e. one tick
// every 15*16 = 240 M-cycles → 223721.56 Hz with the NTSC clock. That is
// exactly 4.2× the YM2612 rate (MCLK/1008), and it is no coincidence:
// 1008/240 = 4.2.
//
// That detail matters. ALL PSG frequency increments are multiples of 240
// M-cycles, so every wave transition lands EXACTLY on a tick boundary —
// sampling at 223721 Hz is exact, with no sub-sample jitter and no need for
// band-limited synthesis here. Aliasing is handled later, when decimating to
// the output rate with the resampler.
// ---------------------------------------------------------------------------

#pragma once

#include <cstdint>

namespace ayther {

class PsgSynth {
public:
    static constexpr int      kChannels       = 4;        // 3 tones + noise
    static constexpr uint32_t kMCyclesPerTick = 15 * 16;  // 240

    PsgSynth() { reset(); }

    void reset();

    /// One byte to the PSG port, exactly as the fork records it (chip == PSG,
    /// addr == 0). The chip has a single write port: the byte is
    /// self-describing (bit 7 = register latch, otherwise continuation data).
    void write(uint8_t data);

    /// Advances one internal tick and leaves the output of the 4 channels in
    /// `out`, normalised to the same order of magnitude as ymfm.
    ///
    /// Each channel comes out UNIPOLAR (0 or volume), just like the chip model.
    /// That introduces DC, and what removes it is the DC blocker in the router
    /// — the same correction the isolated FM needs, and for the same reason: a
    /// voice starting or stopping would be a step, that is, a click.
    void tick(float out[kChannels]);

private:
    void shift_noise();

    int regs_[8]      = {};   // 0/2/4 = tone period · 1/3/5/7 = volume · 6 = noise control
    int latch_        = 3;
    int inc_[4]       = {};   // period in TICKS (not in M-cycles)
    int counter_[4]   = {};
    int polarity_[3]  = {};
    int noise_pol_    = 0;
    int noise_shift_  = 0;
};

}  // namespace ayther
