#pragma once
// ---------------------------------------------------------------------------
// audio_bus_balance.h — BETWEEN-BUS normalization (second half).
//
// The first half—measuring each asset (peak, RMS, clipping, and suggested
// correction)—already lives in `audio_asset_level.h` and is visible in Mix.
// It lets the author fix an asset that clips or gets masked. It does not fix
// the problem that appears only once the pack is complete: **music and sound
// effects can each be correct on their own yet clash with each other**. A pack
// where every hit masks the music may still contain flawless individual assets.
//
// Why this is a separate calculation rather than "the same one, averaged":
//
//   · It averages ENERGY, not decibels. Averaging -6 dB and -30 dB gives
//     -18 dB, which is not the perceived level because -6 dB dominates.
//     Averaging energy first and then converting to dB yields a value that
//     corresponds to what is actually heard.
//   · It is weighted by DURATION. A three-minute track and a 200 ms hit do not
//     contribute equally to the perceived bus volume; counting them equally
//     would make a pack with many short effects appear loud.
//   · It includes the ALREADY-AUTHORED gain. The author may have lowered an
//     asset manually; balance must consider what will be heard, not merely
//     what the file contains.
//
// MUSIC IS THE REFERENCE. It is continuous material: the ear uses it to set
// the scene level, and it keeps playing when nothing else happens. Without
// classified music, the reference is the bus with the most measured material;
// if there is only one bus, there is nothing to balance and that is reported.
//
// What this calculation does NOT do: modify the file or the per-asset gain.
// Its output is a PER-BUS correction, exactly what
// `AytherSession::set_bus_volume` applies live and the project persists.
// Correcting the bus rather than each asset also reflects the measurement
// honestly: the bus is what was found to be unbalanced.
//
// This is stateless and defined in the public header—with its oracle,
// `audio_bus_balance`—because the rule belongs to the CALCULATION and can
// therefore be tested without an audio engine.
// ---------------------------------------------------------------------------
#include <cmath>
#include <cstdint>
#include <vector>

namespace ayther {

/// A measured asset, with its bus and its authored gain.
struct AudioBusSample {
    uint8_t bus        = 0;      ///< AudioBus (0 = Unclassified)
    float   rms        = 0.0f;   ///< 0..1 linear, from the asset analysis
    double  duration_s = 0.0;
    float   gain       = 1.0f;   ///< authored gain of the asset/Sequence (linear)
};

/// How a bus turned out.
struct AudioBusLevel {
    uint32_t count      = 0;         ///< measured assets that landed here
    double   seconds    = 0.0;       ///< total material
    float    level_db   = -120.0f;   ///< effective level (weighted energy)
    /// Correction to align it with the reference. 0 on the reference itself and
    /// on a bus with no material.
    float    correction_db = 0.0f;
    /// The imbalance exceeds the margin: this is what is shown as a warning.
    bool     out_of_range  = false;
};

struct AudioBusBalance {
    /// Indexed by AudioBus (0..Count). Bus 0 (`Unclassified`) is measured like
    /// the rest but is NEVER the reference and never receives a correction:
    /// "I do not know what this is" cannot define the level of the pack, and
    /// correcting it would move material that has nothing to do with itself as
    /// a single block.
    std::vector<AudioBusLevel> buses;
    uint8_t reference     = 0;       ///< bus taken as the reference (0 = none)
    bool    has_reference = false;
    /// Fewer than two buses with material: there is nothing to balance. That is
    /// not the same as "it is balanced", and saying so keeps the UI from
    /// showing an approval it did not earn.
    bool    comparable    = false;
};

/// dBFS of a linear amplitude. -120 is the floor (silence).
inline float ay_lin_to_db(double lin) {
    return lin > 1e-6 ? (float)(20.0 * std::log10(lin)) : -120.0f;
}
inline float ay_db_to_lin(float db) {
    return db <= -120.0f ? 0.0f : (float)std::pow(10.0, db / 20.0);
}

/// The balance between buses. `bus_count` = `kAudioBusCount`.
///
/// `margin_db` is how much imbalance is tolerated before warning (6 dB by
/// default: that is double the amplitude, the point where a bus stops
/// accompanying and starts masking). `max_correction_db` bounds what is offered
/// as a correction: a 30 dB correction is not a balance, it is the wrong
/// material, and applying it silently would hide the real problem.
inline AudioBusBalance audio_bus_balance(const std::vector<AudioBusSample>& samples,
                                         uint32_t bus_count,
                                         float margin_db = 6.0f,
                                         float max_correction_db = 12.0f) {
    AudioBusBalance out;
    out.buses.assign(bus_count, AudioBusLevel{});
    if (bus_count == 0) return out;

    // Duration-weighted energy: sum(rms² · s) / sum(s).
    std::vector<double> energy(bus_count, 0.0);
    for (const AudioBusSample& s : samples) {
        if (s.bus >= bus_count) continue;
        // An asset with no duration contributes nothing: counting it with zero
        // weight is the same as not measuring it, and counting it with weight
        // one would give an empty file the same vote as a music track.
        if (!(s.duration_s > 0.0) || !(s.rms > 0.0f)) continue;
        const double eff = (double)s.rms * (double)(s.gain < 0.0f ? 0.0f : s.gain);
        energy[s.bus]  += eff * eff * s.duration_s;
        out.buses[s.bus].seconds += s.duration_s;
        out.buses[s.bus].count   += 1;
    }
    for (uint32_t b = 0; b < bus_count; ++b) {
        AudioBusLevel& L = out.buses[b];
        if (L.count == 0 || L.seconds <= 0.0) continue;
        L.level_db = ay_lin_to_db(std::sqrt(energy[b] / L.seconds));
    }

    // The reference: Music (bus 1) if it has material; otherwise the
    // classified bus with the most seconds. `Unclassified` (0) never.
    uint32_t medidos = 0;
    for (uint32_t b = 1; b < bus_count; ++b) if (out.buses[b].count) ++medidos;
    out.comparable = medidos >= 2;

    uint8_t ref = 0;
    if (bus_count > 1 && out.buses[1].count) {
        ref = 1;
    } else {
        double best = 0.0;
        for (uint32_t b = 1; b < bus_count; ++b)
            if (out.buses[b].count && out.buses[b].seconds > best) {
                best = out.buses[b].seconds; ref = (uint8_t)b;
            }
    }
    if (!ref) return out;                 // nothing classified: no reference
    out.reference = ref; out.has_reference = true;

    const float ref_db = out.buses[ref].level_db;
    for (uint32_t b = 1; b < bus_count; ++b) {
        AudioBusLevel& L = out.buses[b];
        if (!L.count || b == ref) continue;
        const float delta = ref_db - L.level_db;      // + = it must be raised
        L.out_of_range = std::fabs(delta) > margin_db;
        L.correction_db = delta >  max_correction_db ?  max_correction_db
                        : delta < -max_correction_db ? -max_correction_db
                        : delta;
    }
    return out;
}

}  // namespace ayther
