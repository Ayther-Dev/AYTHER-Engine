#pragma once
// ---------------------------------------------------------------------------
// ayther_recording.h — deterministic gameplay recording (.arp).  Ayther R7.
//
// An Ayther Replay Package is NOT a video: it is the deterministic *input* to
// the Lab. A recording = an initial savestate + the per-frame input stream.
// Replaying = restore the state and re-inject the inputs → the exact same
// gameplay, frame for frame, scrubbable (see lab.md §4, lab-engine-split §7).
//
//   take.arp = game_id + name
//            + initial savestate (zstd-compressed on disk)
//            + input stream (one RetroPad bitmask per frame)
//
// The occurrence history ({slot,hash,anim_group} per frame) + trim marks land
// in R7b; this is the recording/replay foundation.
//
// In memory the initial state is kept RAW (ready to unserialize); compression
// happens only at save() time. The motor (AytherSession) records into this and
// replays from it via replay_seek().
// ---------------------------------------------------------------------------
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ayther {

/// Per-frame occurrence summary — drives the timeline's multitrack lanes
/// without re-simulating (lab.md §7.3). Compact: 12 bytes/frame (.arp v6).
struct FrameStat {
    uint16_t sprites = 0;   ///< sprite occurrences this frame
    uint16_t tiles   = 0;   ///< tile occurrences this frame
    uint16_t audio   = 0;   ///< audio occurrences this frame
    uint16_t plane_a = 0;   ///< Plane A: non-empty nametable cells (coverage) — v5
    uint16_t plane_b = 0;   ///< Plane B: same — v5
    uint16_t plane_w = 0;   ///< Window (HUD): same — v6
};

struct AytherRecording {
    std::string            game_id;       ///< pack/game id this take belongs to
    std::string            name;          ///< display name (e.g. "take_003")
    std::vector<uint8_t>   initial_state; ///< raw savestate at record start
    std::vector<uint16_t>  inputs;        ///< port-0 RetroPad bitmask per frame
    std::vector<FrameStat> stats;         ///< per-frame occurrence summary (R7b)
    uint32_t               trim_in  = 0;  ///< non-destructive in-mark  (frame)
    uint32_t               trim_out = 0;  ///< non-destructive out-mark (frame, exclusive)

    // Per-frame sprite hashes (R7c "occurrence history") — drives the per-hash
    // presence lane so the timeline can show *which* frames a sprite is on
    // screen without re-simulating. Stored CSR-style to avoid nested vectors:
    //   hashes[hash_offsets[f] .. hash_offsets[f+1])  are frame f's sprite hashes.
    std::vector<uint64_t> sprite_hashes;
    std::vector<uint32_t> hash_offsets;   ///< size = frame_count()+1 (or empty)

    /// CURRENT version of the sprite hash algorithm (1 = flip-invariant, raw
    /// VRAM pattern — 2026-07-10). It is persisted in the .arp (v8) to detect
    /// old histories: a take with a lower `hash_algo` holds hashes from ANOTHER
    /// function (mirrored faces used to hash differently) → present() does not
    /// find the re-captured poses and the timeline marks never light up. The
    /// Lab re-bakes it on load (replay_rebake_history_step) and saves again.
    static constexpr uint32_t kSpriteHashAlgo = 1;
    uint32_t hash_algo = kSpriteHashAlgo;   ///< algorithm of the captured history (0 = pre-v8)

    // Per-frame AUDIO hashes (.arp v7) — drives the per-sound presence rows under
    // the AUDIO lane so the timeline shows *which* frames a sound plays and lets
    // the user mute it by hash. Same CSR layout as sprite_hashes above:
    //   audio_hashes[audio_offsets[f] .. audio_offsets[f+1])  are frame f's audio hashes.
    std::vector<uint64_t> audio_hashes;
    std::vector<uint32_t> audio_offsets;  ///< size = frame_count()+1 (or empty)

    /// Baked replay keyframe (R7e): a savestate (zstd-compressed) that gives
    /// frame `frame` its starting point — unserialize + run [frame, target)
    /// yields the sought frame WITHOUT re-simulating from 0. Compressed
    /// separately so only the one a seek needs is decompressed (bounded RAM on
    /// long takes).
    struct Keyframe {
        uint32_t             frame    = 0;   ///< frame it gives a start to
        uint32_t             raw_size = 0;   ///< size of the uncompressed savestate
        std::vector<uint8_t> comp;           ///< zstd-compressed savestate
    };
    std::vector<Keyframe> keyframes;         ///< sorted asc. by frame (may be empty)

    uint32_t frame_count() const { return static_cast<uint32_t>(inputs.size()); }
    bool     empty()       const { return inputs.empty() || initial_state.empty(); }

    /// Compresses `raw_state` and adds it as the keyframe of frame `frame`.
    /// Called by the motor when closing/migrating a take. No-op if
    /// (de)compression fails.
    void add_keyframe(uint32_t frame, const std::vector<uint8_t>& raw_state);

    /// Decompresses keyframes[idx] into `out`. false if idx is out of range or
    /// decompression fails. The motor calls it on demand, per seek.
    bool decompress_keyframe(size_t idx, std::vector<uint8_t>& out) const;

    /// True if sprite `hash` is present on frame `f` (R7c). False when the take
    /// has no captured hash history.
    bool present(uint32_t f, uint64_t hash) const {
        if (hash_offsets.size() < 2 || f + 1 >= hash_offsets.size()) return false;
        for (uint32_t i = hash_offsets[f]; i < hash_offsets[f + 1]; ++i)
            if (sprite_hashes[i] == hash) return true;
        return false;
    }

    /// Sub-take [begin, end) with `state` as the initial savestate (it must be
    /// the machine state PRE-frame `begin`). Rebases inputs/stats/CSR history
    /// and resets trim marks to 0. Precondition: begin < end <= frame_count().
    AytherRecording slice(uint32_t begin, uint32_t end,
                          std::vector<uint8_t> state) const;

    /// Write to `path` as a `.arp` file (initial state zstd-compressed).
    /// Returns false on I/O or compression failure.
    bool save(const std::string& path) const;

    /// Rewrites ONLY the `name` field of an existing .arp header — renaming a
    /// take must not recompress its savestate nor touch anything else.
    /// Successful no-op if the name already matches. false if the file is
    /// missing, the header is invalid or I/O fails (the original is left
    /// intact).
    static bool patch_name(const std::string& path, const std::string& new_name);

    /// Load a `.arp` file. Returns std::nullopt on any error (bad magic,
    /// truncated, decompression failure).
    static std::optional<AytherRecording> load(const std::string& path);
};

}  // namespace ayther
