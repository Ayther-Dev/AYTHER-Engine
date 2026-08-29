#pragma once
// ---------------------------------------------------------------------------
// failure_escalation.h — when to stop trying.
//
// The fallback already prevents a broken asset from cutting the session short:
// the original is heard and that is that. What is missing is ESCALATION — a
// pack with many broken assets retries each one, every frame, and pays for the
// full resolution of something already known not to work. That is the risk
// noted here.
//
// THE RULE, which is the only thing this header holds:
//
//   DISTINCT ASSETS are counted, not occurrences.
//
// One broken file that plays a thousand times is ONE problem; twelve distinct
// files is a badly assembled pack or a folder that never arrived. Counting
// occurrences would shut the subsystem down over a single asset that repeats a
// lot — which is exactly the case NOT to punish, because the fallback already
// handles it well.
//
// And the count is PER SUBSYSTEM: missing music says nothing about sound
// effects, and shutting both down over one would take out what does work.
//
// Header-only and dependency-free: it is tested without a session, without
// audio and without a ROM.
// ---------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ayther {

class FailureEscalation {
public:
    /// Twelve distinct files. A couple of broken assets is not a disaster and
    /// the fallback covers them unnoticed; a dozen is already a pack that came
    /// out wrong, and continuing to retry them costs more than it yields.
    static constexpr size_t kDefaultThreshold = 12;

    explicit FailureEscalation(size_t threshold = kDefaultThreshold)
        : threshold_(threshold ? threshold : 1) {}

    /// Records that `asset` failed in `subsystem`.
    ///
    /// Returns true ONLY on the failure that crosses the threshold — once, not
    /// on every subsequent call. If it always returned true, the caller would
    /// shut down an already-shut-down subsystem every frame and the log would
    /// grow without saying anything new.
    bool note(uint32_t subsystem, const std::string& asset) {
        if (asset.empty()) return false;
        auto& seen = failed_[subsystem];
        if (!seen.insert(asset).second) return false;   // already counted
        return seen.size() == threshold_;
    }

    /// How many distinct assets failed in that subsystem.
    size_t count(uint32_t subsystem) const {
        const auto it = failed_.find(subsystem);
        return it == failed_.end() ? 0 : it->second.size();
    }

    /// The total, for the message shown to the user.
    size_t total() const {
        size_t n = 0;
        for (const auto& [s, set] : failed_) { (void)s; n += set.size(); }
        return n;
    }

    size_t threshold() const { return threshold_; }
    void   clear() { failed_.clear(); }

private:
    size_t threshold_;
    std::unordered_map<uint32_t, std::unordered_set<std::string>> failed_;
};

}  // namespace ayther
