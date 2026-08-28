//! PCM batch fingerprinting and HD-audio substitution.
//!
//! [`AudioHasher`] records deterministic sample batches, while
//! [`AudioSubstitutor`] resolves batch or event identities to replacement audio.

// ---------------------------------------------------------------------------
// audio_hasher.rs — PCM sample fingerprinting for HD audio substitution.
//
// ## Philosophy
//
// The same design as TileHasher / SpriteHasher, applied to audio:
//
//   retro_audio_sample_batch  →  AudioHasher::process_batch()
//       ↓  (per-batch xxHash3-64)
//   catalog: HashMap<u64, AudioEntry>
//       ↓
//   AudioSubstitutor::resolve()  →  AudioSub { asset_path, … }
//       ↓
//   AudioPlayer: suppress emulator batch, play HD WAV from.ay pack
//
// ## Batch granularity
//
// libretro cores call `retro_audio_sample_batch` with a buffer of
// stereo i16 samples.  Genesis Plus GX emitting at 44100 Hz × 60 fps
// produces roughly 735 stereo frames (~2940 bytes) per call.
//
// We hash the **entire raw batch** without amplitude normalization:
//   hash = xxHash3_64(bytes of the i16 stereo buffer)
//
// This works reliably for SFX because the Z80 sound processor replays
// each effect at a fixed amplitude, producing bit-identical PCM output
// for the same game event.  Music streams change every batch and will
// generate thousands of unique hashes — the Lab's "Audio" tab shows
// occurrence counts, letting the artist quickly distinguish recurring SFX
// (high counts) from one-shot music frames (count=1).
//
// ## Silent batch handling
//
// Batches where all samples are 0 (silence during pause/load screens) are
// skipped entirely — they share a trivial hash but have no substitution
// value and would pollute the catalog.
//
// ## v0.9.x changelog
//
// Raw batch hash, WAV substitution, and SFX focus.
// v0.9.1: mute-on-substitution (suppress emulator PCM when HD asset plays).
// v0.9.2: Lua audio API (ayther.audio.list/replace/clear), OGG + FLAC decode.
// ---------------------------------------------------------------------------

use std::collections::HashMap;
use xxhash_rust::xxh3::xxh3_64;

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

/// Metadata for one detected audio batch pattern.
#[derive(Clone, Debug)]
pub struct AudioEntry {
    /// Hash of the raw stereo PCM batch.
    pub hash: u64,
    /// Number of stereo frames in the original batch (frames = samples / 2).
    pub frame_count: usize,
    /// Total times this hash appeared since startup.
    pub total_hits: u64,
    /// Number of times this hash appeared in the *current* game tick.
    pub frame_hits: u32,
}

/// One audio occurrence reported to the Lab / substitutor for a single
/// game tick (analogous to `SpriteOccurrence`).
#[derive(Clone, Debug)]
pub struct AudioOccurrence {
    /// Hash of the observed stereo PCM batch.
    pub hash: u64,
    /// Number of stereo frames in this batch.
    pub frame_count: usize,
    /// How many times this hash appeared in the current tick.
    pub hits: u32,
}

// ---------------------------------------------------------------------------
// AudioHasher
// ---------------------------------------------------------------------------

/// Catalogs recurring stereo PCM batches by exact content hash.
pub struct AudioHasher {
    /// Accumulates all seen patterns: hash → entry.
    catalog: HashMap<u64, AudioEntry>,
    /// Occurrences from the most-recently-completed tick.
    last_occs: Vec<AudioOccurrence>,
    /// Hashes seen in the *current* in-flight tick (before `end_tick`).
    tick_hits: HashMap<u64, u32>,
}

impl Default for AudioHasher {
    fn default() -> Self {
        Self::new()
    }
}

impl AudioHasher {
    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------

    /// Creates an empty audio-batch catalog.
    pub fn new() -> Self {
        Self {
            catalog: HashMap::new(),
            last_occs: Vec::new(),
            tick_hits: HashMap::new(),
        }
    }

    // -----------------------------------------------------------------------
    // process_batch — call from the audio callback for every batch
    // -----------------------------------------------------------------------

    /// Fingerprint one stereo PCM batch and update the catalog.
    ///
    /// `samples` is a stereo-interleaved `i16` buffer
    /// (L₀, R₀, L₁, R₁, …) exactly as provided by
    /// `retro_audio_sample_batch`.
    ///
    /// Returns the hash of this batch, or `0` if the batch was silent
    /// (all zeros) and was skipped.
    pub fn process_batch(&mut self, samples: &[i16]) -> u64 {
        if samples.is_empty() {
            return 0;
        }

        // Skip silent batches — they share hash but carry no useful info.
        if samples.iter().all(|&s| s == 0) {
            return 0;
        }

        // Hash the raw bytes of the i16 buffer.
        let bytes: &[u8] = bytemuck_cast(samples);
        let hash = xxh3_64(bytes);

        let frame_count = samples.len() / 2; // stereo → frames

        // Update catalog.
        let entry = self.catalog.entry(hash).or_insert_with(|| AudioEntry {
            hash,
            frame_count,
            total_hits: 0,
            frame_hits: 0,
        });
        entry.total_hits += 1;

        // Track per-tick hits (resolved in end_tick).
        *self.tick_hits.entry(hash).or_insert(0) += 1;

        hash
    }

    // -----------------------------------------------------------------------
    // end_tick — call once per game frame (after all batch callbacks)
    // -----------------------------------------------------------------------

    /// Snapshot the tick's audio occurrences and reset the in-flight counters.
    ///
    /// Call once at the end of each `run_frame()` cycle.  After this call,
    /// `last_occurrences()` reflects the just-completed tick.
    pub fn end_tick(&mut self) {
        // Persist frame_hits into entries.
        for (&hash, &hits) in &self.tick_hits {
            if let Some(e) = self.catalog.get_mut(&hash) {
                e.frame_hits = hits;
            }
        }
        // Clear hits for entries that weren't seen this tick.
        let seen: std::collections::HashSet<u64> = self.tick_hits.keys().copied().collect();
        for (hash, e) in self.catalog.iter_mut() {
            if !seen.contains(hash) {
                e.frame_hits = 0;
            }
        }

        // Snapshot occurrences.
        self.last_occs.clear();
        for (&hash, &hits) in &self.tick_hits {
            if let Some(e) = self.catalog.get(&hash) {
                self.last_occs.push(AudioOccurrence {
                    hash,
                    frame_count: e.frame_count,
                    hits,
                });
            }
        }
        // Stable sort by total_hits descending (most frequent first).
        self.last_occs.sort_by(|a, b| {
            let ta = self.catalog.get(&a.hash).map(|e| e.total_hits).unwrap_or(0);
            let tb = self.catalog.get(&b.hash).map(|e| e.total_hits).unwrap_or(0);
            tb.cmp(&ta)
        });

        self.tick_hits.clear();
    }

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------

    /// Total unique PCM patterns accumulated since creation.
    pub fn unique_count(&self) -> u32 {
        self.catalog.len() as u32
    }

    /// Occurrences from the most-recently-completed tick (after `end_tick`).
    pub fn last_occurrences(&self) -> &[AudioOccurrence] {
        &self.last_occs
    }
}

// ---------------------------------------------------------------------------
// AudioSubstitutor
// ---------------------------------------------------------------------------

/// Maps PCM batch hashes to HD audio asset paths inside the `.ay` pack.
///
/// Two sources of substitution (same priority model as SpriteSubstitutor):
///   1. `catalog` — loaded from `audio_substitutions.toml` at startup.
///   2. `overrides` — set per-frame by Lua or runtime API (beats catalog).
pub struct AudioSubstitutor {
    catalog: HashMap<u64, String>,   // hash → asset path (from TOML / pack)
    overrides: HashMap<u64, String>, // hash → asset path (runtime, per-frame)
    /// event signature → (asset, looping), from `audio_events.toml`. (C-A2)
    event_catalog: HashMap<u64, EventAsset>,
    /// event signature → override (authoring assign; persists, not per-tick).
    event_overrides: HashMap<u64, EventAsset>,
}

impl Default for AudioSubstitutor {
    fn default() -> Self {
        Self::new()
    }
}

/// The HD asset bound to an audio event signature (+ whether it loops).
#[derive(Clone, Debug)]
struct EventAsset {
    asset: String,
    looping: bool,
}

impl AudioSubstitutor {
    /// Creates an empty batch and event substitution catalog.
    pub fn new() -> Self {
        Self {
            catalog: HashMap::new(),
            overrides: HashMap::new(),
            event_catalog: HashMap::new(),
            event_overrides: HashMap::new(),
        }
    }

    /// Load substitutions from `audio_substitutions.toml` inside the pack.
    ///
    /// Expected TOML schema (same `[[sub]]` convention as tiles/sprites):
    ///
    /// ```toml
    /// [[sub]]
    /// hash  = "0x0123456789abcdef"
    /// asset = "audio/sfx/ring_pickup.wav"
    /// ```
    pub fn load_from_pack(&mut self, pack: &crate::archive_vfs::AyArchive) {
        // Per-batch substitutions (legacy).
        if let Some(data) = pack.read("audio_substitutions.toml")
            && let Ok(text) = std::str::from_utf8(&data)
        {
            self.parse_toml(text);
        }
        // Event substitutions (C-A2).
        if let Some(data) = pack.read("audio_events.toml")
            && let Ok(text) = std::str::from_utf8(&data)
        {
            self.parse_events_toml(text);
        }
    }

    /// Parse an `audio_substitutions.toml` body into the catalog (same `[[sub]]`
    /// schema as tiles/sprites). Split out of load_from_pack so the Deliver
    /// round-trip is unit-testable without building a pack.
    fn parse_toml(&mut self, text: &str) {
        let tbl: toml::Value = match toml::from_str(text) {
            Ok(t) => t,
            Err(e) => {
                eprintln!("[AudioSubstitutor] TOML parse error: {e}");
                return;
            }
        };

        let subs = match tbl.get("sub").and_then(|v| v.as_array()) {
            Some(a) => a,
            None => return,
        };

        for entry in subs {
            let hash_str = match entry.get("hash").and_then(|v| v.as_str()) {
                Some(s) => s,
                None => continue,
            };
            let asset = match entry.get("asset").and_then(|v| v.as_str()) {
                Some(s) => s.to_string(),
                None => continue,
            };
            if let Some(hash) = parse_hex_hash(hash_str) {
                self.catalog.insert(hash, asset);
            }
        }
    }

    /// Register a runtime override (e.g. from Lua `ayther.audio.replace()`).
    pub fn add_override(&mut self, hash: u64, asset_path: &str) {
        self.overrides.insert(hash, asset_path.to_string());
    }

    /// Clear all runtime overrides.  Call once per tick before processing.
    pub fn clear_overrides(&mut self) {
        self.overrides.clear();
    }

    /// Number of entries loaded from the TOML catalog.
    pub fn catalog_len(&self) -> usize {
        self.catalog.len()
    }

    // --- Event substitution (C-A2) -----------------------------------------

    /// Parse `audio_events.toml` into the event catalog. Schema:
    /// ```toml
    /// [[event]]
    /// signature = "0x1a2b3c4d5e6f7a8b"
    /// asset     = "audio/music/zone1.ogg"
    /// loop      = true   # optional (default false)
    /// ```
    fn parse_events_toml(&mut self, text: &str) {
        let tbl: toml::Value = match toml::from_str(text) {
            Ok(t) => t,
            Err(e) => {
                eprintln!("[AudioSubstitutor] events TOML parse error: {e}");
                return;
            }
        };
        let events = match tbl.get("event").and_then(|v| v.as_array()) {
            Some(a) => a,
            None => return,
        };
        for entry in events {
            let sig_str = match entry.get("signature").and_then(|v| v.as_str()) {
                Some(s) => s,
                None => continue,
            };
            let asset = match entry.get("asset").and_then(|v| v.as_str()) {
                Some(s) => s.to_string(),
                None => continue,
            };
            let looping = entry.get("loop").and_then(|v| v.as_bool()).unwrap_or(false);
            if let Some(sig) = parse_hex_hash(sig_str) {
                self.event_catalog
                    .insert(sig, EventAsset { asset, looping });
            }
        }
    }

    /// Bind an HD asset to an event signature (authoring assign; persists — this
    /// is NOT cleared by `clear_overrides`, which is the per-tick per-batch path).
    pub fn add_event_override(&mut self, signature: u64, asset_path: &str, looping: bool) {
        self.event_overrides.insert(
            signature,
            EventAsset {
                asset: asset_path.to_string(),
                looping,
            },
        );
    }

    /// Clear all event overrides.
    pub fn clear_event_overrides(&mut self) {
        self.event_overrides.clear();
    }

    /// Number of event substitutions in the TOML catalog.
    pub fn event_catalog_len(&self) -> usize {
        self.event_catalog.len()
    }

    /// Resolve detected audio events into substitution instructions. Overrides
    /// (authoring) beat the pack catalog. Each sub carries the event's frame
    /// range so the caller mutes `start..=end` of the emulator and plays the HD
    /// asset aligned to `start_frame`.
    pub fn resolve_events(&self, events: &[crate::audio_event::AudioEvent]) -> Vec<AudioEventSub> {
        events
            .iter()
            .filter_map(|ev| {
                let ea = self
                    .event_overrides
                    .get(&ev.signature)
                    .or_else(|| self.event_catalog.get(&ev.signature))?;
                Some(AudioEventSub {
                    signature: ev.signature,
                    asset_path: ea.asset.clone(),
                    looping: ea.looping,
                    start_frame: ev.start_frame as u64,
                    end_frame: ev.end_frame as u64,
                })
            })
            .collect()
    }

    /// Resolve a slice of `AudioOccurrence` into `AudioSub` instructions.
    ///
    /// Returns substitutions for every occurrence whose hash is found in the
    /// overrides or catalog.  Occurrences with no match are skipped.
    pub fn resolve(&self, occs: &[AudioOccurrence]) -> Vec<AudioSub> {
        let mut out = Vec::new();
        for occ in occs {
            let asset = self
                .overrides
                .get(&occ.hash)
                .or_else(|| self.catalog.get(&occ.hash));
            if let Some(path) = asset {
                out.push(AudioSub {
                    hash: occ.hash,
                    asset_path: path.clone(),
                    frame_count: occ.frame_count,
                });
            }
        }
        out
    }
}

// ---------------------------------------------------------------------------
// AudioSub — one resolved substitution
// ---------------------------------------------------------------------------

/// A resolved HD audio substitution: suppress the emulator batch identified
/// by `hash` and play `asset_path` from the.ay pack instead.
#[derive(Clone, Debug)]
pub struct AudioSub {
    /// Hash of the emulator PCM batch to suppress.
    pub hash: u64,
    /// Logical path inside the.ay pack, e.g. "audio/sfx/ring_pickup.wav".
    pub asset_path: String,
    /// Duration hint: stereo frames in the original batch.
    pub frame_count: usize,
}

/// A resolved HD substitution for a whole audio **event** (C-A2): mute the
/// emulator over `start_frame..=end_frame` and play `asset_path` aligned to
/// `start_frame` (looping while the run lasts if `looping`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AudioEventSub {
    /// Stable signature of the detected chip-audio event.
    pub signature: u64,
    /// Logical replacement asset path inside the pack.
    pub asset_path: String,
    /// Whether the replacement loops for the event's duration.
    pub looping: bool,
    /// First emulated frame covered by the replacement.
    pub start_frame: u64,
    /// Last emulated frame covered by the replacement, inclusive.
    pub end_frame: u64,
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Parse "0x0123456789abcdef" → u64.  Returns None on malformed input.
fn parse_hex_hash(s: &str) -> Option<u64> {
    let s = s
        .trim()
        .strip_prefix("0x")
        .or_else(|| s.trim().strip_prefix("0X"))
        .unwrap_or(s.trim());
    u64::from_str_radix(s, 16).ok()
}

/// Zero-cost reinterpret `&[i16]` as `&[u8]`.
///
/// Safe because i16 has no padding and the slice length is correctly scaled.
#[inline]
fn bytemuck_cast(s: &[i16]) -> &[u8] {
    // SAFETY: i16 is plain-old-data, no padding, alignment of u8 <= alignment of i16.
    unsafe { std::slice::from_raw_parts(s.as_ptr() as *const u8, std::mem::size_of_val(s)) }
}

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn make_sfx(len: usize, val: i16) -> Vec<i16> {
        vec![val; len]
    }

    // -------------------------------------------------------------------------
    // AudioHasher: distinct non-silent batches get different hashes
    // -------------------------------------------------------------------------
    #[test]
    fn different_batches_different_hashes() {
        let mut h = AudioHasher::new();
        let h1 = h.process_batch(&make_sfx(256, 1000));
        let h2 = h.process_batch(&make_sfx(256, 2000));
        assert_ne!(h1, 0, "non-silent batch must produce non-zero hash");
        assert_ne!(h2, 0);
        assert_ne!(h1, h2, "different content → different hash");
    }

    // -------------------------------------------------------------------------
    // AudioHasher: identical batches produce the same hash every time
    // -------------------------------------------------------------------------
    #[test]
    fn same_batch_same_hash() {
        let sfx: Vec<i16> = (0i16..512).collect();
        let mut h = AudioHasher::new();
        let h1 = h.process_batch(&sfx);
        let h2 = h.process_batch(&sfx);
        assert_eq!(h1, h2, "same PCM → same hash");
        assert_eq!(h.unique_count(), 1, "same batch counted once in catalog");
    }

    // -------------------------------------------------------------------------
    // AudioHasher: silent batches are skipped (hash = 0, not added to catalog)
    // -------------------------------------------------------------------------
    #[test]
    fn silent_batch_skipped() {
        let mut h = AudioHasher::new();
        let result = h.process_batch(&make_sfx(512, 0));
        assert_eq!(result, 0, "silent batch must return hash 0");
        assert_eq!(h.unique_count(), 0, "silent batch must not enter catalog");
    }

    #[test]
    fn audio_sub_parse_toml_round_trips_deliver_format() {
        // Exactly what the lab DeliverPanel writes for audio_substitutions.toml:
        // [[sub]] with hash = "0x%016llx" and asset by basename.
        let mut sub = AudioSubstitutor::new();
        sub.parse_toml(
            "[[sub]]\nhash = \"0x00000000deadbeef\"\nasset = \"ring.wav\"\n\n\
             [[sub]]\nhash = \"0x0123456789abcdef\"\nasset = \"jump.ogg\"\n\n",
        );
        assert_eq!(sub.catalog_len(), 2);
    }

    // -------------------------------------------------------------------------
    // AudioHasher: end_tick snapshots occurrences correctly
    // -------------------------------------------------------------------------
    #[test]
    fn end_tick_snapshots_occurrences() {
        let sfx_a: Vec<i16> = (0i16..256).collect();
        let sfx_b: Vec<i16> = (100i16..356).collect();

        let mut h = AudioHasher::new();
        let ha = h.process_batch(&sfx_a);
        let _ = h.process_batch(&sfx_a); // second hit
        let hb = h.process_batch(&sfx_b);
        h.end_tick();

        let occs = h.last_occurrences();
        assert_eq!(occs.len(), 2, "two distinct batches this tick");

        let occ_a = occs
            .iter()
            .find(|o| o.hash == ha)
            .expect("sfx_a in occurrences");
        assert_eq!(occ_a.hits, 2, "sfx_a appeared twice");

        let occ_b = occs
            .iter()
            .find(|o| o.hash == hb)
            .expect("sfx_b in occurrences");
        assert_eq!(occ_b.hits, 1, "sfx_b appeared once");
    }

    // -------------------------------------------------------------------------
    // AudioSubstitutor: resolve returns substitutions for matching hashes only
    // -------------------------------------------------------------------------
    #[test]
    fn event_substitution_parses_and_resolves() {
        use crate::audio_event::AudioEvent;

        let mut sub = AudioSubstitutor::new();
        sub.parse_events_toml(
            "[[event]]\nsignature = \"0x00000000deadbeef\"\nasset = \"music/zone1.ogg\"\nloop = true\n\n\
             [[event]]\nsignature = \"0x0000000000000abc\"\nasset = \"voice/hi.ogg\"\n");
        assert_eq!(sub.event_catalog_len(), 2);

        let events = vec![
            AudioEvent {
                signature: 0xdeadbeef,
                instrument: 0,
                chip: 0,
                channel: 0,
                start_frame: 10,
                end_frame: 40,
                pitch: crate::audio_event::NO_PITCH,
                velocity: crate::audio_event::NO_VELOCITY,
            },
            AudioEvent {
                signature: 0xabc,
                instrument: 0,
                chip: 0,
                channel: 1,
                start_frame: 50,
                end_frame: 55,
                pitch: crate::audio_event::NO_PITCH,
                velocity: crate::audio_event::NO_VELOCITY,
            },
            AudioEvent {
                signature: 0x9999,
                instrument: 0,
                chip: 1,
                channel: 0,
                start_frame: 60,
                end_frame: 61,
                pitch: crate::audio_event::NO_PITCH,
                velocity: crate::audio_event::NO_VELOCITY,
            }, // no sub
        ];
        let subs = sub.resolve_events(&events);
        assert_eq!(subs.len(), 2, "only the two known signatures resolve");

        let music = &subs[0];
        assert_eq!(music.asset_path, "music/zone1.ogg");
        assert!(music.looping);
        assert_eq!((music.start_frame, music.end_frame), (10, 40));

        let voice = &subs[1];
        assert_eq!(voice.asset_path, "voice/hi.ogg");
        assert!(!voice.looping, "loop defaults to false");
    }

    #[test]
    fn event_override_beats_catalog() {
        use crate::audio_event::AudioEvent;

        let mut sub = AudioSubstitutor::new();
        sub.parse_events_toml(
            "[[event]]\nsignature = \"0x00000000deadbeef\"\nasset = \"pack.ogg\"\n",
        );
        sub.add_event_override(0xdeadbeef, "authoring.ogg", true);

        let ev = [AudioEvent {
            signature: 0xdeadbeef,
            instrument: 0,
            chip: 0,
            channel: 0,
            start_frame: 0,
            end_frame: 5,
            pitch: crate::audio_event::NO_PITCH,
            velocity: crate::audio_event::NO_VELOCITY,
        }];
        let subs = sub.resolve_events(&ev);
        assert_eq!(subs.len(), 1);
        assert_eq!(subs[0].asset_path, "authoring.ogg");
        assert!(subs[0].looping);
    }

    #[test]
    fn substitutor_resolves_matching_hashes() {
        let mut sub = AudioSubstitutor::new();
        sub.add_override(0xDEADBEEF_CAFEBABE, "audio/sfx/ring.wav");

        let occs = vec![
            AudioOccurrence {
                hash: 0xDEADBEEF_CAFEBABE,
                frame_count: 512,
                hits: 1,
            },
            AudioOccurrence {
                hash: 0x0000_0000_0000_0001,
                frame_count: 256,
                hits: 1,
            },
        ];
        let subs = sub.resolve(&occs);
        assert_eq!(subs.len(), 1, "only the matching hash resolves");
        assert_eq!(subs[0].asset_path, "audio/sfx/ring.wav");
        assert_eq!(subs[0].hash, 0xDEADBEEF_CAFEBABE);
    }
}
