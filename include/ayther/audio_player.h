#pragma once
// ---------------------------------------------------------------------------
// audio_player.h — SDL3 audio device: emulator passthrough + HD WAV playback.
//
// PCM passthrough via continuous emu_stream_ + one-shot HD SFX streams.
// v0.9.1:  Mute-on-substitution — emulator PCM is suppressed for any hash that
//          had a resolved HD substitution in the previous tick.  This left a
//          1-tick (~16 ms) bleed on the first appearance of a new substitution,
//          because the audio callback fires *during* run_frame(), before the
//          substitution for the current frame has been resolved.
// v0.9.7:  Deferred passthrough — the audio callback no longer pushes PCM
//          directly.  It buffers each batch (tagged with its hash) via
//          buffer_emulator(); the main loop resolves substitutions for the
//          *same* frame, refreshes the mute set, then calls flush_emulator(),
//          which pushes only the non-muted batches.  The mute decision now uses
//          the current frame's resolution, eliminating the 1-tick bleed.
//
// Design:
//   emu_stream_     Continuous emulator PCM passthrough (S16 stereo, 44100 Hz).
//
//   pending_pcm_    Per-frame interleaved PCM staged by buffer_emulator() and
//   pending_batches_  drained by flush_emulator().  Capacity is reused across
//                   frames (no steady-state allocation).
//
//   sfx_streams_    One-shot streams for HD WAV substitutions.
//                   Created in play_substitutions(), reaped in tick() once drained.
//
//   mute_hashes_    Set of hashes whose emulator PCM should be suppressed this
//                   frame.  Refreshed via set_mute_hashes() from resolved subs.
//
//   wav_cache_      WAV assets decoded once per asset_path and held in memory.
//                   Freed in shutdown().
//
// Thread model: all public methods are called from the main/emulation thread.
// SDL3 audio streams are internally thread-safe for the push side.
//
// Typical per-tick call sequence:
//
//   // inside runner audio callback (called during run_frame()):
//   uint64_t hash = ayther_audio_hasher_process_batch(hasher, data, frames);
//   audio_player.buffer_emulator(hash, data, frames);
//
//   // after ayther_audio_sub_resolve() — unconditional, clears set when 0:
//   audio_player.play_substitutions(pack, audio_subs, n_audio_subs);
//   audio_player.set_mute_hashes(audio_subs, n_audio_subs);
//   audio_player.flush_emulator();   // pushes non-muted batches to emu_stream_
//
//   // end of frame:
//   audio_player.tick();
// ---------------------------------------------------------------------------

#include <SDL3/SDL.h>
#include "audio_hd_mixer.h"
#include "audio_asset_level.h"
#include "ayther_core_ffi.h"
#include "runtime_options.h"

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

/// @brief Owns SDL audio resources and aligns native and replacement audio.
///
/// init() is fallible; shutdown() is idempotent and is also called by the
/// destructor. Unless a method states otherwise, pointers passed to this class
/// are borrowed for the duration of the call and their data is copied when it
/// must be retained.
///
/// This class is not thread-safe. Drive staging, substitution, and flush
/// operations from the session thread. SDL callbacks and stream ownership must
/// not outlive the player.
class AudioPlayer {
public:
    /// Explains why a replacement asset is unavailable. `None` means decoded
    /// and ready. Disk failures may be invalidated when the file appears or its
    /// fingerprint changes; failures for immutable pack content are permanent.
    enum class AssetError : uint8_t {
        None,         ///< Decoded PCM is available in the cache.
        Missing,      ///< The disk or pack entry cannot be opened.
        Empty,        ///< The asset exists but has no payload.
        Unsupported,  ///< No registered decoder accepts the extension.
        Corrupt,      ///< The decoder rejected the payload.
    };

    AudioPlayer()  = default;
    /// RAII: release the SDL device + streams. shutdown() is idempotent and safe
    /// on a never-initialised player, so owners (e.g. AytherSession) don't need a
    /// manual shutdown() call before destruction.
    ~AudioPlayer() { shutdown(); }

    // Holds raw SDL handles — non-copyable (a copy would double-free the device).
    AudioPlayer(const AudioPlayer&)            = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // ---- Lifecycle ----------------------------------------------------------

    /// Open the default SDL3 audio device and create the emulator stream.
    /// Must be called after SDL_Init(SDL_INIT_AUDIO).
    /// Returns true on success; false leaves the player in a safe no-op state.
    ///
    /// `options` is injected rather than read from the environment here, so a
    /// test can drive the PCM tee without touching the process environment.
    bool init(const ayther::RuntimeOptions& options =
                  ayther::RuntimeOptions::process());

    /// Flush all one-shot streams, unbind the emulator stream, and close the
    /// audio device.  Safe to call even if init() was never called or failed.
    void shutdown();

    // ---- Mute-on-substitution (v0.9.1) --------------------------------------

    /// Returns true if emulator PCM with this hash should be suppressed because
    /// a HD substitution is active for it this frame.
    /// hash == 0 (silent batch) always returns false.
    bool should_mute(uint64_t hash) const;

    /// Refresh the mute set from the current frame's resolved substitutions.
    /// Pass count == 0 to clear (no active substitutions → passthrough restored).
    /// Call once per frame, after play_substitutions() and before flush_emulator().
    void set_mute_hashes(const AytherAudioSub* subs, uint32_t count);

    /// Persistent per-hash mute requested by the author (timeline AUDIO rows in
    /// Edit). Independent of HD substitutions — survives between frames and is
    /// OR'd into should_mute(). Pass n == 0 to clear (all sounds audible again).
    void set_user_mute_hashes(const uint64_t* hashes, size_t n);

    /// Sets gain for the complete game-audio stream; `1.0` is neutral. The
    /// stream includes routed synthesized audio, so this attenuates the game
    /// mix without enumerating individual identities.
    ///
    /// Intended for temporary ducking. The operation is idempotent and updates
    /// SDL only when the value changes.
    void set_game_gain(float g);
    float game_gain() const { return game_gain_; }

    // ---- Per-audio-callback -------------------------------------------------

    /// Stage one emulator PCM batch (S16LE stereo, 44100 Hz) tagged with its
    /// hash.  Called synchronously from the libretro audio_sample_batch
    /// trampoline inside run_frame() — i.e. on the main thread.
    /// The batch is not pushed to the device until flush_emulator() runs, by
    /// which time the current frame's substitutions have been resolved.
    void buffer_emulator(uint64_t hash, const int16_t* data, size_t frames);

    // ---- Per-frame ----------------------------------------------------------

    /// Push every staged batch whose hash is not muted to emu_stream_, then
    /// clear the staging buffers (capacity retained).  Call once per frame,
    /// after set_mute_hashes().
    ///
    /// In unified mode, muted native batches are replaced with silence rather
    /// than removed, preserving the block timeline. Replacement voices are
    /// added at their exact sample positions and the combined block crosses the
    /// same rate-control backlog. `suppress_original` silences the native block
    /// while preserving the replacement mix. It replaces discard-based range
    /// muting, which would also remove replacement audio from a unified block.
    void flush_emulator(bool suppress_original = false);

    /// Drop every staged emulator batch WITHOUT pushing it to the device, then
    /// clear the staging buffers (capacity retained).  Used by replay_seek's
    /// fast re-sim: the skipped frames' PCM piles up in pending_pcm_ via the
    /// audio callback and would otherwise blast out at once on the final
    /// produce_frame — discard it so a seek stays silent until its target frame.
    void discard_emulator();

    // ---- Per-tick -----------------------------------------------------------

    /// For each resolved AytherAudioSub, decode the WAV from the pack and
    /// start a one-shot playback stream.  Deduplicates by hash within the tick
    /// (and across ticks if the previous stream has not yet finished).
    void play_substitutions(AyArchive*            pack,
                            const AytherAudioSub* subs,
                            uint32_t              count);

    /// Reap one-shot streams that have finished playing (available bytes == 0).
    /// Call once per frame, after play_substitutions().
    void tick();

    /// Immediately stop and destroy all active one-shot streams.
    void stop_all_sfx();

    /// Cuts the one-shots marked `preview` IMMEDIATELY (2026-08-22: closing the
    /// Audio Library with a ▶ still playing). Immediate and deliberately
    /// without a fade: the fade is progressed by tick(), which only runs inside
    /// the audible flush of produce — with the transport PAUSED (the typical
    /// case with a dialog open) a fade never finishes and the preview drains in
    /// full.
    void stop_preview_sfx();

    /// TOTAL cut of GAMEPLAY audio when the transport is paused.
    /// - stops the gameplay one-shot SFX and event streams (the one-shots
    ///   marked `preview` — authoring previews — keep playing);
    /// - discards the staging (pending_pcm_/pending_batches_);
    /// - drains the PCM already queued in emu_stream_ and synth_stream_;
    /// - resets the DRC state (EMA, ratio, last_flush) so the first flush after
    ///   resuming takes the stall path and re-primes the cushion.
    /// Idempotent (repeating it while paused does nothing new and leaks no
    /// handles). Returns the stereo FRAMES discarded (staging + emu + synth)
    /// and accumulates them in pause_cut_frames() — pause telemetry.
    uint64_t cut_transport_audio();

    /// Telemetry: frames discarded by pause cuts (accumulated) and how many
    /// effective cuts there were (calls that discarded something).
    uint64_t pause_cut_frames() const { return pause_cut_frames_; }
    uint64_t pause_cuts()       const { return pause_cuts_; }

    // ---- Per-EVENT HD (C-A2, Components) ------------------------------------

    /// Starts the HD asset (WAV/OGG/FLAC from the pack) of a substituted EVENT,
    /// aligned to its start_frame. A retrigger of the same `signature` RESTARTS
    /// the stream (the event was detected again). `looping`: the PCM is re-fed
    /// in tick_events() until the frame passes `end_frame`; a non-loop asset
    /// plays once and its stream is reaped once drained.
    /// Returns true only if the stream ended up PLAYING on the device — the
    /// caller decides whether to mute the original from this answer, not from
    /// the existence of the assignment.
    /// `cut_frame` = the ABSOLUTE frame after which tick_events destroys the
    /// stream even with PCM queued (end_frame + the policy tail).
    /// UINT64_MAX = drains in full (the legacy contract of non-loops). A loop
    /// stops re-feeding at end_frame and its remainder drains until cut_frame.
    /// `start_offset_seconds` starts FROM THE MIDDLE of the asset — resuming
    /// after a pause recreates the stream at the point dictated by the emulated
    /// clock. For a loop the offset is modulo the asset (phase is preserved);
    /// for a non-loop past the end it returns true WITHOUT creating a stream
    /// (nothing to play, not a failure — the same contract as the one-shot).
    /// `fade_frames` > 0 = FADE_OUT end policy — past end_frame the voice fades
    /// over those frames (44100/s) and dies in silence, instead of being cut
    /// dead. It is an ALTERNATIVE to tail, not cumulative: with a fade,
    /// `cut_frame` does not interrupt the ramp. 0 = the previous behaviour.
    bool play_event_hd(AyArchive* pack, const char* asset_path, bool looping,
                       uint64_t signature, uint64_t end_frame,
                       uint64_t cut_frame = UINT64_MAX,
                       double start_offset_seconds = 0.0,
                       uint32_t fade_frames = 0,
                       // The AUTHORED gain of the Sequence. It goes last and
                       // with a neutral default so no caller has to change:
                       // the absence of the value is 1.0, just as in the TOML.
                       float gain = 1.0f,
                       // Loop region in asset FRAMES. (0,0) = the whole asset,
                       // which is what was always done. The mixer already knows
                       // how to cycle it with a modulo over the span; what was
                       // missing was for the value to reach here.
                       size_t loop_begin = 0, size_t loop_end = 0);

    /// Per-frame maintenance of the event streams: cuts the loops that passed
    /// their end_frame, re-feeds the active ones running out of data, and reaps
    /// the non-loops already drained. Call it with the current frame.
    void tick_events(uint64_t frame);

    /// Cuts ALL event streams right now (stop / scrub / take change).
    void stop_all_events();

    /// Plays a PCM buffer (S16 stereo, 44100 Hz) as a one-shot — the preview of
    /// a game audio captured by hash (Layers panel). It replaces the preview in
    /// progress. `frames` = stereo frames (samples = frames×2).
    /// Queues PCM from the SoundFont synthesiser: INTERLEAVED f32 stereo at
    /// 44100, which is what RustySynth delivers and the same rate as the
    /// emulator — that way "one game frame = N samples" is the same arithmetic
    /// for both.
    ///
    /// It is a CONTINUOUS stream, not a one-shot: it is called once per frame
    /// with exactly that frame's samples. A `play_oneshot_pcm` per frame would
    /// open a new stream every time and they would overlap.
    void feed_synth(const float* interleaved, size_t frames);

    /// How many PCM samples the emulator has STAGED for this frame.
    ///
    /// It is the correct measure of "how much audio this frame is worth", and
    /// that is why the synthesiser uses it instead of 44100/fps: if the Lab
    /// runs slower than real time —and it does— a fixed number under-feeds and
    /// the synthesiser stream starves, while the emulator one is saved by its
    /// DRC. Tying them to the same number makes both drift alike and never
    /// separate.
    size_t pending_frames() const { return pending_pcm_.size() / 2; }

    /// Samples the SYNTHESISER stream still has unconsumed.
    ///
    /// It has mattered ever since the router uses it for ALL audio. With the
    /// SoundFont it made no difference: it delivered isolated notes and a gap
    /// was silence. The router delivers CONTINUOUS audio, and the Lab does it
    /// in bursts of 90-150 ms —not at 16.7— so if the stream carries no
    /// cushion, SDL drains it between bursts and every gap is an audible cut.
    size_t synth_queued_frames() const;

    /// Pushes `frames` samples of silence into the synthesiser stream to build
    /// that cushion before it starts delivering.
    void prime_synth(size_t frames);

    /// ADDS audio into the emulator PCM that has not been flushed yet.
    ///
    /// It is the router path, and the difference from feed_synth is not
    /// cosmetic: the emulator stream has DRC —it stretches its pace when the
    /// Lab cannot keep up, which is what ten root causes cost us— and the
    /// synthesiser one has nothing. With its own stream, the router delivers
    /// less audio than the device consumes and SDL drains it: measured,
    /// stream_queue=0 on almost every tick, i.e. one cut per burst.
    ///
    /// Riding on the emulator PCM it inherits all the pacing already solved.
    ///
    /// `mix_over_chip` decides what happens to what the chip left staged:
    ///
    ///   false — the block TAKES ITS PLACE (the staged data is discarded). It is
    ///           the cartridge case: the router mirrors the ten channels that
    ///           play, so the core PCM contributes nothing the block does not
    ///           carry, and the chip may keep playing in full for the hasher.
    ///   true  — the block is ADDED over the current frame, keeping the staged
    ///           data. It is the Sega CD case: there the core buffer carries the
    ///           chip PCM and the CDDA, which the router does not mirror. What
    ///           the router does render already came silenced from the core by
    ///           mask, so nothing is heard twice.
    void buffer_router(const float* interleaved, size_t frames,
                       bool mix_over_chip = false);

    /// Hash of the router batch. Reserved: the mute set is built from game
    /// audio hashes, so this one never lands in it.
    static constexpr uint64_t kRouterHash = 0xA17E'2600'0000'0001ull;

    /// Discards what the synthesiser has queued. For the cuts: a seek leaves
    /// notes in flight that would play over the new scene.
    void clear_synth();

    void play_oneshot_pcm(const int16_t* pcm, size_t frames);
    /// Stops the one-shot preview (the Stop button / on closing the dialog).
    void stop_oneshot();

    /// Plays a LOOSE HD ASSET (from disk, not from the pack) as a one-shot SFX
    /// — the per-event audio substitution during authoring (Audio workspace,
    /// C-A4): it decodes WAV/OGG/FLAC from the path, binds it to the device
    /// (resampled by SDL) and deduplicates it by `key` (the event signature) so
    /// it is not restarted while playing. `offset_seconds` starts FROM THE
    /// MIDDLE of the asset (a play that begins inside a Sequence window → HD in
    /// sync with the playhead). It is reaped in tick() like any SFX. No-op
    /// without a device.
    /// `gain`: stream volume (1 = original) — the Sequence slider.
    /// `preview`: an EXPLICIT authoring request (the Play button in Mix) — it
    /// does not belong to gameplay and the pause cut does not reach it.
    /// Returns true only if the stream ended up PLAYING on the device (or the
    /// offset fell past the end, which is not a failure). false = the original
    /// must play in its place.
    bool play_oneshot_asset_file(const std::string& path, uint64_t key,
                                 double offset_seconds = 0.0, float gain = 1.0f,
                                 bool preview = false);

    // ---- Asset availability -------------------------------------------------

    /// Is the HD asset from DISK decoded and ready to play? It is THE question
    /// that precedes silencing the original: assigned ≠ playable.
    /// On a failed entry it re-verifies the fingerprint (mtime+size) with a
    /// rate limit — a file that appears or is replaced is retried without
    /// restarting the session, and one that is still broken does not log 60
    /// times a second.
    bool asset_ready_disk(const std::string& abs_path);
    /// Same for an asset FROM THE PACK. The negative is permanent: the pack is
    /// immutable by content, so what failed once fails always.
    bool asset_ready_pack(AyArchive* pack, const std::string& asset_path);
    /// Diagnostic for the Lab/telemetry: why it is not ready ("missing",
    /// "empty", "unsupported", "corrupt") or nullptr if it is ready / was never
    /// attempted. It triggers no IO.
    const char* asset_error_name(const std::string& path) const;

    /// HD start attempts that FAILED with the asset already ready (creating or
    /// binding the SDL stream) — the rare class; asset failures show up in
    /// asset_error_name and in the caller's fallback.
    uint64_t hd_start_fails() const { return hd_start_fails_; }
    // -- Level analysis -------------------------------------------------------
    //
    // What an author needs to know BEFORE publishing: whether the asset clips,
    // whether it will get lost in the mix, and how much it would need
    // correcting.
    //
    // It is measured over the ALREADY DECODED PCM —the same `wav_cache_`
    // playback uses— so analysing an asset that already played costs no decode,
    // and analysing a new one leaves it cached for when it plays. The source
    // file is never touched: every AYTHER correction is gain at playback, not a
    // rewrite.
    /// The struct lives in audio_asset_level.h: it is shared by this player
    /// (which measures) and AytherSession (through which the Lab queries it),
    /// and those two headers cannot include each other.
    using AssetLevel = ayther::AudioAssetLevel;
    /// Measures a disk asset (WAV/OGG/FLAC). The result is cached by path —
    /// walking the samples of a long asset is not free and the Lab asks for it
    /// per frame while the panel is open.
    const AssetLevel& asset_level(const std::string& abs_path);

    /// The asset ENVELOPE — minimum and maximum per column, in -1..1.
    ///
    /// Returns `bins` interleaved (min, max) pairs: `[min0, max0, min1, …]`.
    /// Min and max rather than a single absolute value: a waveform drawn only
    /// from the maximum does not show asymmetry, and asymmetry is where DC
    /// offset and one-sided clipping become visible — which is half of what one
    /// looks at it for.
    ///
    /// It is measured over the SAME PCM as the mix (S16 44.1 stereo), like
    /// `asset_level`: what the author wants to see is what is going to play.
    ///
    /// It is cached by (path, bins). The Lab asks for it per frame while the
    /// panel is open and walking a long asset is not free.
    const std::vector<float>& asset_waveform(const std::string& abs_path,
                                             uint32_t bins);

    /// Duration in SECONDS of an HD asset from disk (WAV/OGG/FLAC) — it decodes
    /// (and caches) the file and computes it from the PCM. 0 if it cannot be
    /// read/decoded. It does NOT need an open audio device (it only decodes).
    /// Used to size the span of the Sequence timeline (the HD may be longer
    /// than the events).
    double asset_duration_seconds(const std::string& abs_path);
    /// Decodes a DISK asset to S16 stereo 44100 PCM (the format of the MP4
    /// export mixdown). Returns the number of stereo FRAMES (out holds
    /// frames×2 samples); 0 if it could not be read/converted. It needs no
    /// device; the raw decode is cached (wav_cache_), the conversion is not.
    size_t decode_asset_pcm_s16_44k(const std::string& abs_path,
                                    std::vector<int16_t>& out);
    /// PREWARM: decodes and caches an HD disk asset WITHOUT playing it (no
    /// device needed). It is called when opening the project / setting the subs
    /// so the FIRST trigger does not pay for the decode mid-playback — the
    /// stall caused catch-up and triggers were skipped (report 2026-07-23: "the
    /// first time some sounds were not heard, the second time they were").
    /// Idempotent (cached).
    void prewarm_asset_file(const std::string& path);
    /// Cuts (with tick's fast fade-out) the one-shot SFX with this `key` —
    /// e.g. the HD preview of the Sequence lane when pausing. No-op with no
    /// match. Returns true if it CUT something: silencing halfway through a
    /// long asset has to be distinguishable from silencing between triggers,
    /// and calling it every frame while the mute lasts is idempotent (a fade
    /// already started is not restarted) but only the first returns true.
    bool stop_sfx_by_key(uint64_t key);
    /// Changes the volume of a one-shot ALREADY PLAYING. Without this, the gain
    /// is only applied when the stream is created and dragging the slider would
    /// not be heard until the next re-sync — that is, never, during continuous
    /// playback, which is exactly when one wants to adjust it. It does not
    /// touch the ones fading out. Returns true if it touched any.
    bool set_sfx_gain_by_key(uint64_t key, float gain);
    /// Cuts the event stream of a signature (the play_event_hd path, which is a
    /// different stream from the one-shot). Returns true if it cut something.
    bool stop_event(uint64_t signature);
    /// True while the one-shot preview still has undrained PCM (the device is
    /// still playing it). The dialog toggles Play/Stop from this, and it
    /// auto-resets to Play when the sound ends.
    bool preview_playing() const {
        return preview_stream_ && SDL_GetAudioStreamAvailable(preview_stream_) > 0;
    }

    // ---- Accessors ----------------------------------------------------------

    bool              is_open()   const { return device_ != 0; }
    SDL_AudioDeviceID device_id() const { return device_;      }
    /// Live SDL one-shot streams. After retiring the old path these are ONLY
    /// the explicit authoring previews — the gameplay one-shots are mixer
    /// voices and are counted with `hd_voice_count()`. It is deliberately kept
    /// separate: adding them together would erase the distinction that has to
    /// be assertable (that gameplay no longer opens streams of its own).
    size_t            sfx_count() const { return sfx_streams_.size(); }
    /// Live HD events (per-event substitutions from the pack) — the observable
    /// for "did the window cut it?" without listening, sibling of sfx_count().
    /// It comes from the mixer — the per-event SDL stream path was retired and
    /// `event_streams_` with it. The count means the same as before (how many
    /// per-event substitutions are playing), so the existing oracles keep
    /// measuring what they measured.
    size_t            event_count() const { return hd_mixer_.event_voice_count(); }

    // ---- UNIFIED path -------------------------------------------------------
    // The gameplay HD sounds are HdMixer voices, summed inside the emulator's
    // staged block at their exact sample: one stream, one DRC, phase
    // independent of stalls and catch-up. EXPLICIT authoring previews stay in
    // their own streams (they do not belong to the transport).
    //
    // The `set_unified()` switch was retired, and with it the per-event stream
    // path. It was the emergency exit while the mixer was new; there are no
    // longer two paths to maintain nor an A/B to run without recompiling.
    /// Marks that HERE begins the PCM of the next emulated frame within the
    /// staged block. The session calls it before every audible run_frame: it is
    /// what places a trigger from frame k of a catch-up at ITS offset and not
    /// at the start of the block.
    void mark_frame_boundary() { frame_mark_ = pending_pcm_.size() / 2; }
    /// ABSOLUTE timeline cursor (frames already flushed).
    uint64_t timeline_samples() const { return timeline_samples_; }
    size_t   hd_voice_count() const { return hd_mixer_.voice_count(); }
    /// Phase 0 telemetry: voices started, accumulated/maximum placement
    /// lateness (a sustained 0 = the phase is exact).
    uint64_t hd_voices_started() const { return hd_mixer_.started(); }
    uint64_t hd_mix_skew() const { return hd_mixer_.skew_samples(); }
    uint64_t hd_mix_max_skew() const { return hd_mixer_.max_skew_samples(); }

    // ---- Global mute --------------------------------------------------------
    /// Silences/restores ALL output by setting the device gain to 0/1 (it
    /// affects the emulator passthrough + the HD SFX). Idempotent and safe if
    /// the device never opened.
    void set_muted(bool m);
    bool is_muted() const { return muted_; }

    // ---- Dynamic rate control (DRC, v0.10) ----------------------------------
    /// Enable/disable drift compensation on the emulator stream (on by default).
    void  set_drc_enabled(bool on) { drc_enabled_ = on; }
    /// Last frequency ratio applied to emu_stream_ (1.0 = neutral; ~±0.5%).
    float drc_ratio() const { return drc_ratio_; }
    /// Flush frames with a backlog < 1/4 of the target (starvation) —
    /// telemetry.
    uint64_t starved_frames() const { return starved_frames_; }
    /// Average backlog (EMA) in frames of the emulator stream.
    float drc_queue_avg() const { return drc_queue_avg_; }

private:
    // ---- Injected configuration ---------------------------------------------

    /// Held by value: the player outlives whatever expression produced it, and
    /// a dangling reference to the options would be a hard bug to see.
    ayther::RuntimeOptions options_;

    // ---- SDL objects --------------------------------------------------------

    SDL_AudioDeviceID device_     = 0;        ///< logical audio device
    SDL_AudioStream*  emu_stream_ = nullptr;  ///< continuous emulator passthrough
    float             game_gain_  = 1.0f;     ///< soundtrack ducking (Kinematic)
    /// The SoundFont synthesiser stream. Deliberately SEPARATE from the
    /// emulator one: `flush_emulator` skips the whole batch whose hash is
    /// muted, so a timbre mixed in there would go silent along with the voice
    /// it comes to replace. With its own stream, SDL mixes them at the device
    /// and the mute of the original does not reach it.
    SDL_AudioStream*  synth_stream_ = nullptr;
    bool              muted_      = false;    ///< device gain at 0 (global mute)

    // ---- Dynamic rate control -----------------------------------------------
    // Keep emu_stream_'s backlog near a target by nudging its resample ratio
    // ±0.5% (inaudible) so the emulator clock tracks the host device clock —
    // no drift underrun (crackle) / overrun (latency). See ayther-engine.md §6.2.
    bool  drc_enabled_   = true;
    float drc_queue_avg_ = 0.0f;   ///< EMA of queued frames (jitter filter)
    // Starvation diagnostic — a backlog < 1/4 of the target means the device
    // is about to scrape the bottom (audible crackle). Counter + rate-limited
    // log (1/s).
    /// Backlog target of the emulator stream (frames @44.1 kHz).
    /// 3072 ≈ 70 ms — a cushion that absorbs the dip of a dense scene (~10 ms)
    /// even after a stall, with latency imperceptible for authoring.
    static constexpr float kDrcTargetFrames = 3072.0f;
    uint64_t starved_frames_    = 0;
    uint64_t last_starve_log_ms_ = 0;
    // Pause telemetry — frames discarded by cut_transport_audio (staging + emu
    // + synth) and how many cuts discarded something.
    uint64_t pause_cut_frames_  = 0;
    uint64_t pause_cuts_        = 0;
    uint64_t last_flush_ms_      = 0;   // stall detector (re-priming)
    // Tee of the emulator PCM to a WAV (env AYTHER_AUDIO_DUMP=<path>) — what is
    // QUEUED to the device (post-mute, pre-DRC). An objective discriminator: a
    // clean WAV + degraded listening = a DELIVERY problem (device/pacing); a
    // WAV with clicks = a CONTENT problem (upstream of the queueing).
    void* dump_ = nullptr;            // FILE* (void* so cstdio is not included here)
    uint64_t dump_data_bytes_ = 0;
    float drc_ratio_     = 1.0f;   ///< last applied frequency ratio (telemetry)

    struct SfxStream {
        SDL_AudioStream* stream = nullptr;
        uint64_t         hash   = 0;   ///< source audio hash (for dedup)
        /// 0 = playing normally. != 0 = the SDL_GetTicks() ms at which its
        /// fade-out started (a new trigger with the SAME key overrode it) —
        /// tick() lowers its gain to silence and destroys it, instead of
        /// letting it play in full overlapped with the new one.
        uint64_t         fade_start_ms = 0;
        /// An EXPLICIT authoring preview — the transport pause cut
        /// (cut_transport_audio) does not touch it.
        bool             preview = false;
    };
    std::vector<SfxStream> sfx_streams_;

    SDL_AudioStream* preview_stream_ = nullptr;  ///< one-shot of the audio preview

    // ---- Deferred emulator passthrough (v0.9.7) -----------------------------

    /// One staged emulator batch: a slice of pending_pcm_ plus its source hash.
    struct PendingBatch {
        uint64_t hash         = 0;  ///< source audio hash (0 = silent / never muted)
        size_t   frame_offset = 0;  ///< start frame within pending_pcm_
        size_t   frames       = 0;  ///< frame count (1 frame = 2 × int16_t)
    };
    std::vector<int16_t>     pending_pcm_;      ///< interleaved S16 stereo, reused
    std::vector<PendingBatch> pending_batches_; ///< this frame's batches, reused

    // ---- Mute set -----------------------------------------------------------

    std::unordered_set<uint64_t> mute_hashes_;      ///< hashes to suppress this frame (HD subs)
    std::unordered_set<uint64_t> user_mute_hashes_; ///< author-muted hashes (persistent)

    // ---- WAV cache ----------------------------------------------------------

    struct WavEntry {
        SDL_AudioSpec        spec = {};
        std::vector<uint8_t> pcm;           ///< decoded PCM bytes (SDL_LoadWAV_IO)
        // Attempt state. `None` with pcm filled = ready; any other value = a
        // NEGATIVE cache entry with the fingerprint of the file that failed
        // (0/0 if it did not exist) — retried only when the fingerprint
        // changes.
        AssetError err           = AssetError::None;
        int64_t    fp_mtime      = 0;   ///< mtime of the failed attempt (disk)
        uint64_t   fp_size       = 0;   ///< size of the failed attempt (disk)
        uint64_t   last_check_ms = 0;   ///< rate limit of the re-stat (disk)
    };
    std::unordered_map<std::string, WavEntry> wav_cache_;
    /// Measured level per path. It is invalidated along with `wav_cache_`.
    std::unordered_map<std::string, AssetLevel> level_cache_;
    /// Already-computed envelopes, by (path, column count).
    /// The count is part of the key because resizing the panel changes the
    /// columns, and returning the envelope of another width would draw a
    /// waveform that does not correspond to the asset being looked at.
    std::unordered_map<std::string, std::vector<float>> wave_cache_;
    /// MIX-READY PCM per asset — the WavEntry converted ONCE to S16 stereo
    /// 44100 (the format of the staged block), shared between voices via
    /// shared_ptr. The conversion uses SDL_ConvertAudioSamples and needs no
    /// device.
    std::unordered_map<std::string, HdMixPcm> mix_cache_;
    /// Converts (and caches) the PCM of a WavEntry into the mix format.
    /// nullptr if the conversion fails.
    HdMixPcm get_mix_pcm(const WavEntry* wav, const std::string& cache_key);
    /// How often AT MOST a failed disk asset is re-stat'ed to see whether it
    /// appeared or changed. A balance: perceptible hot-reload (<1 s) without
    /// paying one stat per frame for every broken assignment.
    static constexpr uint64_t kAssetRecheckMs = 400;
    uint64_t hd_start_fails_ = 0;   ///< failed stream create/bind

    // ---- Event streams (C-A2) -----------------------------------------------

    /// Stream of a substituted EVENT: it lives from start_frame to end_frame
    /// (if it loops) or until drained (one-shot). `wav` points into the asset
    /// cache (stable: wav_cache_ deletes no entries until shutdown).

    // ---- Unified path -------------------------------------------------------
    HdMixer  hd_mixer_;                  ///< HD voices summed into the staged block
    uint64_t timeline_samples_ = 0;      ///< frames already flushed (timeline)
    size_t   frame_mark_       = 0;      ///< offset of the current frame in the staging

    // ---- Helpers ------------------------------------------------------------

    /// Return a cached WavEntry (loading from pack on first access).
    /// Returns nullptr if the asset is missing, corrupt, or pack is null.
    const WavEntry* get_wav(AyArchive* pack, const std::string& asset_path);

    /// Same but reading from DISK (a loose authoring asset) instead of the
    /// pack.
    const WavEntry* get_wav_disk(const std::string& abs_path);

    /// Decodes WAV/OGG/FLAC (by extension `ext`) from `raw` into `out`
    /// (spec + pcm). true if it decoded something usable. Shared by get_wav
    /// (pack) and get_wav_disk (disk).
    bool decode_audio_bytes(WavEntry& out, const std::vector<uint8_t>& raw,
                            const std::string& ext);
};
