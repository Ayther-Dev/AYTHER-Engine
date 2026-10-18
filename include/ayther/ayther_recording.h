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
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ayther {

/// Per-frame occurrence summary — drives the timeline's multitrack lanes
/// without re-simulating (lab.md §7.3). Compact: 12 bytes/frame (.arp v6).
struct FrameStat {
  uint16_t sprites = 0; ///< sprite occurrences this frame
  uint16_t tiles = 0;   ///< tile occurrences this frame
  uint16_t audio = 0;   ///< audio occurrences this frame
  uint16_t plane_a = 0; ///< Plane A: non-empty nametable cells (coverage) — v5
  uint16_t plane_b = 0; ///< Plane B: same — v5
  uint16_t plane_w = 0; ///< Window (HUD): same — v6
};

struct AytherRecording {
  std::string game_id;                ///< pack/game id this take belongs to
  std::string name;                   ///< display name (e.g. "take_003")
  std::vector<uint8_t> initial_state; ///< raw savestate at record start
  /// YM2612 register shadow at record start (.arp v9): the 512 bytes the
  /// audio event detector held when the take began (bank 0, then bank 1), or
  /// empty when the recorder had no detector (pre-v9 files load empty). The
  /// instrument and pitch of every FM signature are hashed from that shadow at
  /// key-on, and a game loads its patches once per song: a take that starts
  /// with the music already playing never sees those writes again, so without
  /// this seed its signatures do not match the ones authored from a take that
  /// started before the first note (report 2026-10-14, Golden Axe «The
  /// Battle»). Replay and analysis seed their detectors from it.
  std::vector<uint8_t> audio_fm_regs;
  static constexpr size_t kAudioFmRegisterBytes = 0x200;
  /// The take's stable id (.arp v10, spec 007): the frontend's catalogue id,
  /// written INTO the file so it survives a rename, a copy to another project
  /// and the loss of the frontend's catalogue. 0 = not assigned (every file
  /// older than v10 loads 0). slice() leaves it 0: the tail is a new take.
  uint64_t take_id = 0;
  /// First format version that carries `take_id`.
  static constexpr uint32_t kFormatVersionTakeId = 10;
  std::vector<uint16_t> inputs; ///< port-0 RetroPad bitmask per frame
  std::vector<FrameStat> stats; ///< per-frame occurrence summary (R7b)
  uint32_t trim_in = 0;         ///< non-destructive in-mark  (frame)
  uint32_t trim_out = 0;        ///< non-destructive out-mark (frame, exclusive)

  // Per-frame sprite hashes (R7c "occurrence history") — drives the per-hash
  // presence lane so the timeline can show *which* frames a sprite is on
  // screen without re-simulating. Stored CSR-style to avoid nested vectors:
  //   hashes[hash_offsets[f] .. hash_offsets[f+1])  are frame f's sprite
  //   hashes.
  std::vector<uint64_t> sprite_hashes;
  std::vector<uint32_t> hash_offsets; ///< size = frame_count()+1 (or empty)

  /// CURRENT version of the sprite hash algorithm (1 = flip-invariant, raw
  /// VRAM pattern — 2026-07-10). It is persisted in the .arp (v8) to detect
  /// old histories: a take with a lower `hash_algo` holds hashes from ANOTHER
  /// function (mirrored faces used to hash differently) → present() does not
  /// find the re-captured poses and the timeline marks never light up. The
  /// Lab re-bakes it on load (replay_rebake_history_step) and saves again.
  static constexpr uint32_t kSpriteHashAlgo = 1;
  uint32_t hash_algo =
      kSpriteHashAlgo; ///< algorithm of the captured history (0 = pre-v8)

  // Per-frame AUDIO hashes (.arp v7) — drives the per-sound presence rows under
  // the AUDIO lane so the timeline shows *which* frames a sound plays and lets
  // the user mute it by hash. Same CSR layout as sprite_hashes above:
  //   audio_hashes[audio_offsets[f] .. audio_offsets[f+1])  are frame f's audio
  //   hashes.
  std::vector<uint64_t> audio_hashes;
  std::vector<uint32_t> audio_offsets; ///< size = frame_count()+1 (or empty)

  /// Baked replay keyframe (R7e): a savestate (zstd-compressed) that gives
  /// frame `frame` its starting point — unserialize + run [frame, target)
  /// yields the sought frame WITHOUT re-simulating from 0. Compressed
  /// separately so only the one a seek needs is decompressed (bounded RAM on
  /// long takes).
  struct Keyframe {
    uint32_t frame = 0;        ///< frame it gives a start to
    uint32_t raw_size = 0;     ///< size of the uncompressed savestate
    std::vector<uint8_t> comp; ///< zstd-compressed savestate
  };
  std::vector<Keyframe> keyframes; ///< sorted asc. by frame (may be empty)

  uint32_t frame_count() const { return static_cast<uint32_t>(inputs.size()); }
  bool empty() const { return inputs.empty() || initial_state.empty(); }

  /// Compresses `raw_state` and adds it as the keyframe of frame `frame`.
  /// Called by the motor when closing/migrating a take. No-op if
  /// (de)compression fails.
  void add_keyframe(uint32_t frame, const std::vector<uint8_t> &raw_state);

  /// Decompresses keyframes[idx] into `out`. false if idx is out of range or
  /// decompression fails. The motor calls it on demand, per seek.
  bool decompress_keyframe(size_t idx, std::vector<uint8_t> &out) const;

  /// True if sprite `hash` is present on frame `f` (R7c). False when the take
  /// has no captured hash history.
  bool present(uint32_t f, uint64_t hash) const {
    const std::size_t frame = f;
    if (hash_offsets.size() < 2 || frame >= hash_offsets.size() - 1U)
      return false;
    const std::size_t begin = hash_offsets[frame];
    const std::size_t end = hash_offsets[frame + 1U];
    if (begin > end || end > sprite_hashes.size())
      return false;
    for (std::size_t i = begin; i < end; ++i)
      if (sprite_hashes[i] == hash)
        return true;
    return false;
  }

  /// Sub-take [begin, end) with `state` as the initial savestate (it must be
  /// the machine state PRE-frame `begin`) and `fm_regs` as the detector's
  /// YM2612 shadow at that same point (512 bytes, or empty when unknown; for
  /// begin == 0 pass `audio_fm_regs`). Rebases inputs/stats/CSR history and
  /// resets trim marks to 0. Precondition: begin < end <= frame_count().
  AytherRecording slice(uint32_t begin, uint32_t end,
                        std::vector<uint8_t> state,
                        std::vector<uint8_t> fm_regs = {}) const;

  /// Write to `path` as a `.arp` file (initial state zstd-compressed). The
  /// complete image is serialized and flushed through a sibling before an
  /// existing regular destination is replaced or a missing one is created.
  /// Uses the same per-destination interprocess lock as patch_name() and
  /// rejects symlink/reparse-point destinations. Returns false on compression,
  /// serialization or publication failure; crash durability and atomic
  /// replacement remain filesystem/platform dependent. A false result after
  /// publication was attempted can be indeterminate; callers should reload.
  bool save(const std::string &path) const;

  /// Rewrites ONLY the serialized `name` field of an existing .arp header —
  /// renaming a take must not recompress its savestate or alter other payload
  /// bytes.
  /// Successful no-op if the name already matches. Rejects symlinks/reparse
  /// points and format versions outside the supported [2, current] range.
  /// Writers using save()/patch_name() are serialized per destination across
  /// processes. The replacement preserves Windows metadata supported by
  /// ReplaceFileW and POSIX owner/mode plus supported Linux user/ACL xattrs.
  /// A complete sibling is flushed before the platform replacement call;
  /// neither crash durability nor atomic replacement is promised on every
  /// filesystem. false may follow an attempted publication, so callers must
  /// reload rather than infer which name is present.
  static bool patch_name(const std::string &path, const std::string &new_name);

  /// Writes `take_id` into an existing file without recompressing anything.
  /// v10: rewrites the trailing 8 bytes (successful no-op when equal). v8/v9:
  /// appends the fields the file lacks (v9's empty FM shadow for a v8 file)
  /// plus the id and stamps version 10. Older files are rewritten through
  /// load()+save(). Same lock, sibling and replacement discipline as
  /// patch_name(); false leaves the file as it was or in an indeterminate
  /// state only where patch_name() would, so callers must re-read.
  static bool patch_take_id(const std::string &path, uint64_t take_id);

  /// Reads the embedded take id without decompressing anything: 0 for a file
  /// older than v10 (or one never assigned), std::nullopt when the file is not
  /// a readable recording (bad magic, unsupported version, too short).
  static std::optional<uint64_t> peek_take_id(const std::string &path);

  /// Load a `.arp` file. Returns std::nullopt on any error (bad magic,
  /// truncated, decompression failure).
  static std::optional<AytherRecording> load(const std::string &path);

  /// `sizeof(AytherRecording)` as compiled INTO the engine library. A frontend
  /// that compiles this struct from another copy of the header (the Lab kept a
  /// vendored rc.6 copy while the package did not ship this file) reads every
  /// field at the wrong offset once a member is added — frame_count() comes
  /// back 0 and a take silently "does not open" (2026-10-17, audio_fm_regs).
  /// Compare it with your own sizeof at start-up and refuse to run on mismatch.
  static std::size_t abi_size() noexcept;
};

/// Marks that AytherRecording::abi_size() exists (frontends guard the check).
#define AYTHER_RECORDING_ABI_SIZE 1
/// Marks that AytherRecording carries `take_id` and patch/peek_take_id (v10).
#define AYTHER_RECORDING_TAKE_ID 1

} // namespace ayther
