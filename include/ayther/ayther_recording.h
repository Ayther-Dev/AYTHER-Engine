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
    uint16_t plane_a = 0;   ///< Plano A: celdas de nametable no vacías (cobertura) — v5
    uint16_t plane_b = 0;   ///< Plano B: idem — v5
    uint16_t plane_w = 0;   ///< Window (HUD): idem — v6
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

    /// Versión VIGENTE del algoritmo de hash de sprites (1 = flip-invariante,
    /// patrón crudo de VRAM — 2026-07-10). Se persiste en el .arp (v8) para
    /// detectar historias viejas: una toma con `hash_algo` menor tiene hashes
    /// de OTRA función (las caras espejadas hashaban distinto) → present() no
    /// encuentra las poses re-capturadas y los marks del timeline no encienden.
    /// El Lab la re-hornea al cargarla (replay_rebake_history_step) y re-guarda.
    static constexpr uint32_t kSpriteHashAlgo = 1;
    uint32_t hash_algo = kSpriteHashAlgo;   ///< algo de la historia capturada (0 = pre-v8)

    // Per-frame AUDIO hashes (.arp v7) — drives the per-sound presence rows under
    // the AUDIO lane so the timeline shows *which* frames a sound plays and lets
    // the user mute it by hash. Same CSR layout as sprite_hashes above:
    //   audio_hashes[audio_offsets[f] .. audio_offsets[f+1])  are frame f's audio hashes.
    std::vector<uint64_t> audio_hashes;
    std::vector<uint32_t> audio_offsets;  ///< size = frame_count()+1 (or empty)

    /// Keyframe de replay horneado (R7e): un savestate (zstd-comprimido) que da
    /// arranque al frame `frame` — unserializar + correr [frame, target) rinde el
    /// frame buscado SIN re-simular desde 0. Comprimidos por separado para
    /// descomprimir sólo el que un seek necesita (RAM acotada en tomas largas).
    struct Keyframe {
        uint32_t             frame    = 0;   ///< frame al que da arranque
        uint32_t             raw_size = 0;   ///< tamaño del savestate descomprimido
        std::vector<uint8_t> comp;           ///< savestate zstd-comprimido
    };
    std::vector<Keyframe> keyframes;         ///< ordenados asc. por frame (puede estar vacío)

    uint32_t frame_count() const { return static_cast<uint32_t>(inputs.size()); }
    bool     empty()       const { return inputs.empty() || initial_state.empty(); }

    /// Comprime `raw_state` y lo agrega como keyframe del frame `frame`. Llamado
    /// por el motor al cerrar/migrar una toma. No-op si la (de)compresión falla.
    void add_keyframe(uint32_t frame, const std::vector<uint8_t>& raw_state);

    /// Descomprime keyframes[idx] en `out`. false si idx fuera de rango o falla
    /// la descompresión. El motor lo llama on-demand por seek.
    bool decompress_keyframe(size_t idx, std::vector<uint8_t>& out) const;

    /// True if sprite `hash` is present on frame `f` (R7c). False when the take
    /// has no captured hash history.
    bool present(uint32_t f, uint64_t hash) const {
        if (hash_offsets.size() < 2 || f + 1 >= hash_offsets.size()) return false;
        for (uint32_t i = hash_offsets[f]; i < hash_offsets[f + 1]; ++i)
            if (sprite_hashes[i] == hash) return true;
        return false;
    }

    /// Sub-toma [begin, end) con `state` como savestate inicial (debe ser el
    /// estado de máquina PRE-frame `begin`). Rebasa inputs/stats/historia CSR
    /// y trim marks a 0. Precondición: begin < end <= frame_count().
    AytherRecording slice(uint32_t begin, uint32_t end,
                          std::vector<uint8_t> state) const;

    /// Write to `path` as a `.arp` file (initial state zstd-compressed).
    /// Returns false on I/O or compression failure.
    bool save(const std::string& path) const;

    /// Reescribe SOLO el campo `name` del header de un .arp existente —
    /// renombrar una toma no debe recomprimir su savestate ni tocar el resto.
    /// No-op exitoso si el nombre ya coincide. false si el archivo falta, el
    /// header es inválido o falla la E/S (el original queda intacto).
    static bool patch_name(const std::string& path, const std::string& new_name);

    /// Load a `.arp` file. Returns std::nullopt on any error (bad magic,
    /// truncated, decompression failure).
    static std::optional<AytherRecording> load(const std::string& path);
};

}  // namespace ayther
