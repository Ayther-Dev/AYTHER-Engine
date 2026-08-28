// ---------------------------------------------------------------------------
// audio_match_rule.h — event-substitution matching policy.
//
// Exact signatures distinguish note, channel, and pan changes. An author can
// opt into broader matching per assignment, persisted as `match` in
// audio_events.toml. Legacy packs remain exact without migration because the
// persisted primary key is still `signature`.
//
// This header has no SDL dependency. Its deterministic table lookup is covered
// by tests/audio_match_rule_test.cpp.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <unordered_map>

namespace ayther {

/// Match policy for one authored signature. Numeric values cross FFI/TOML and
/// are ABI data; do not reorder them.
enum class AudioMatchRule : uint8_t {
    kExact           = 0,  ///< Exact signature only; legacy default.
    kInstrument      = 1,  ///< Any voice using the same instrument.
    kInstrumentPitch = 2,  ///< Same instrument and pitch.
};

/// Detector sentinel for unpitched DAC/noise events.
inline constexpr uint8_t kAudioNoPitch = 255;

/// Policy metadata persisted with an authored signature assignment.
struct AudioMatchRuleInfo {
    AudioMatchRule rule       = AudioMatchRule::kExact;
    uint64_t       instrument = 0;             ///< Timbre identity.
    uint8_t        pitch      = kAudioNoPitch; ///< Used only by kInstrumentPitch.
};

/// Instrument-to-assignment index for active-voice matching.
///
/// The caller checks exact signatures first. This index then prefers matching
/// `kInstrumentPitch` entries over `kInstrument`; ties select the numerically
/// lowest authored signature for deterministic results.
class AudioMatchIndex {
public:
    void clear() { by_instr_.clear(); }
    [[nodiscard]] bool empty() const noexcept { return by_instr_.empty(); }

    /// Adds a broad-match assignment. Exact or incomplete policies are ignored.
    void add(uint64_t authored_sig, AudioMatchRule rule, uint64_t instrument,
             uint8_t pitch) {
        if (rule == AudioMatchRule::kExact || instrument == 0) return;
        if (rule == AudioMatchRule::kInstrumentPitch && pitch == kAudioNoPitch)
            return;
        by_instr_.emplace(instrument, Entry{authored_sig, pitch, rule});
    }

    /// Resolves one instrument/pitch pair. Unknown instruments never match.
    /// Returns true and writes the canonical authored signature when found.
    [[nodiscard]] bool resolve(uint64_t instrument, uint8_t pitch,
                               uint64_t* out) const {
        if (instrument == 0 || by_instr_.empty()) return false;
        int      best_rank = -1;
        uint64_t best_sig  = 0;
        const auto range = by_instr_.equal_range(instrument);
        for (auto it = range.first; it != range.second; ++it) {
            const Entry& e = it->second;
            int rank = 0;
            if (e.rule == AudioMatchRule::kInstrumentPitch) {
                if (pitch == kAudioNoPitch || pitch != e.pitch) continue;
                rank = 1;
            }
            if (rank > best_rank ||
                (rank == best_rank && e.sig < best_sig)) {
                best_rank = rank;
                best_sig  = e.sig;
            }
        }
        if (best_rank < 0) return false;
        if (out) *out = best_sig;
        return true;
    }

private:
    struct Entry {
        uint64_t       sig;
        uint8_t        pitch;
        AudioMatchRule rule;
    };
    std::unordered_multimap<uint64_t, Entry> by_instr_;
};

}  // namespace ayther
