#pragma once
// ---------------------------------------------------------------------------
// audio_asset_level.h — measured level of a decoded audio asset.
//
// `AudioPlayer` produces this value from decoded PCM and `AytherSession`
// exposes it to authoring clients. Keeping the value type independent avoids a
// cyclic include between those components.
//
// Values answer three pre-publication questions: whether the asset clips,
// whether it will be masked by native audio, and which non-destructive gain
// adjustment is appropriate.
// ---------------------------------------------------------------------------
#include <cstdint>

namespace ayther {

struct AudioAssetLevel {
    bool     ok            = false;   ///< False when reading or decoding failed.
    double   duration_s    = 0.0;
    int      sample_rate   = 0;       ///< Container sample rate supplied by the author.
    int      channels      = 0;       ///< Container channel count.
    float    peak          = 0.0f;    ///< Linear peak in [0, 1].
    float    peak_db       = -120.0f; ///< dBFS; -120 represents silence.
    float    rms           = 0.0f;
    float    rms_db        = -120.0f;
    /// Runs of at least three consecutive full-scale samples. A single maximum
    /// sample is common in normalized material; a plateau is a stronger
    /// clipping signal and avoids false positives for commercial masters.
    uint64_t clipped_runs  = 0;
    bool     clipping      = false;
    /// Publication thresholds are separate from clipping because they imply
    /// different authoring actions: attenuate the master or raise its level.
    bool     too_hot       = false;   ///< Peak >= -0.1 dBFS or clipping detected.
    bool     too_quiet     = false;   ///< RMS < -30 dBFS; likely masked by game audio.
    /// Non-destructive gain in dB that would place the peak at -1 dBFS.
    float    suggested_gain_db = 0.0f;
};

}  // namespace ayther
