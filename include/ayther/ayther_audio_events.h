#pragma once
// ---------------------------------------------------------------------------
// ayther_audio_events.h — C-A2: HD substitution of whole audio EVENTS.
//
// ## What this is
//
// A per-batch substitution (AudioSubstitutor, ayther_session.h) replaces one
// recurring PCM batch hash with an HD asset — right for a short SFX. A sound
// with an attack and a tail (a jingle, a voice, music) spans many changing
// batches → it needs to be grabbed and replaced as one **event** (a contiguous
// run of non-silent batches; AudioEventDetector, core/src/audio_event.rs, keyed
// by a stable head `signature`, split on silence or a re-attack — C-A1).
//
// This coordinator turns the detected events + the author's `signature → asset`
// assignments into the two per-frame decisions the session's audio needs:
//
//   1. **Range-mute**: while a frame falls inside a substituted event, the
//      emulator PCM is suppressed for the WHOLE event — not per hash, because the
//      event's batches all have different hashes. The session applies it by
//      calling AudioPlayer::discard_emulator() that frame instead of
//      flush_emulator() (reusing the existing capability — no player change).
//
//   2. **HD trigger**: at the event's start_frame the HD asset starts playing,
//      aligned to the attack (looping until end_frame if the asset loops).
//
// ## Session wiring (produce_frame / replay)
//
//   // once, over a take (the Lab authors on a recording; the full timeline is
//   // known → events resolve exactly):
//   AytherAudioEvent evs[N];
//   uint32_t n = ayther_audio_evdet_get_events(detector, evs, N);
//   coord.resolve(evs, n);
//
//   // each frame at `frame`:
//   if (coord.mute_at(frame)) audio.discard_emulator();
//   else                      audio.flush_emulator();
//   AudioEventTrigger t[4];
//   for (uint32_t i = 0, k = coord.triggers_at(frame, t, 4); i < k; ++i)
//       audio.play_event_hd(pack, t[i].asset, t[i].looping);   // OGG/WAV
//
// Live play (ayther_play) uses the same assignments with a short head-latency
// variant (a run is recognised once its signature head is complete) — a
// follow-up; this contract covers the recording/replay path the Lab authors on.
//
// ## Integration status
//
// The detector, substitution FFI, engine coordinator, `AudioPlayer` event
// playback, and root target are integrated. Session replay and live paths share
// the persisted assignments but retain distinct scheduling policies.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ayther_core_ffi.h"   // AytherAudioSubstitutor, AytherAudioEvent, AytherAudioEventSub

namespace ayther {

// An HD asset to start playing this frame for a substituted event (aligned to
// its start_frame). `looping` assets play until `end_frame`.
struct AudioEventTrigger {
    const char* asset     = nullptr;  ///< pack path of the HD asset (OGG/WAV)
    bool        looping   = false;
    uint64_t    signature = 0;
    uint64_t    end_frame = 0;        ///< when a looping asset should stop
};

// One author assignment (signature → HD asset), for enumeration by Deliver into
// audio_events.toml.
struct AudioEventAssignment {
    uint64_t    signature = 0;
    std::string asset;
    bool        looping   = false;
};

class AudioEventSubstitution {
public:
    AudioEventSubstitution();
    ~AudioEventSubstitution();
    AudioEventSubstitution(const AudioEventSubstitution&)            = delete;
    AudioEventSubstitution& operator=(const AudioEventSubstitution&) = delete;
    AudioEventSubstitution(AudioEventSubstitution&&) noexcept;
    AudioEventSubstitution& operator=(AudioEventSubstitution&&) noexcept;

    // -- Authoring assign (persists across frames) ----------------------------
    /// Bind an HD asset to an event signature. "" asset clears that signature.
    void assign(uint64_t signature, const std::string& asset, bool looping);
    void unassign(uint64_t signature);
    void clear() noexcept;
    bool has_assignments() const noexcept;
    /// Sorted by signature, for the Deliver workspace (audio_events.toml).
    std::vector<AudioEventAssignment> assignments() const;

    // -- Resolve the take's detected events into substitution windows ---------
    /// `events` = the detector's completed events over the recording
    /// (ayther_audio_evdet_get_events). Recomputes the active windows. Call after
    /// (re)assigning or when the take changes.
    void resolve(const AytherAudioEvent* events, uint32_t event_count);

    // -- Per-frame queries (drive the session's audio) ------------------------
    /// True if `frame` is inside a substituted event → RANGE-MUTE the emulator.
    bool mute_at(uint64_t frame) const noexcept;
    /// HD assets whose event STARTS at `frame` (play now). Returns the count
    /// written to `out` (≤ cap; usually 0 or 1).
    uint32_t triggers_at(uint64_t frame, AudioEventTrigger* out, uint32_t cap) const;

    // -- Enumerate resolved substitutions (UI / preview) ----------------------
    uint32_t                   sub_count() const noexcept;
    const AytherAudioEventSub* subs()      const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ayther
