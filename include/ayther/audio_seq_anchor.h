#pragma once
// ---------------------------------------------------------------------------
// audio_seq_anchor.h — replay Sequence anchors with CLAIMS between Sequences.
// Header-only and pure: testable without SDL, a core, or a GPU.
//
// A Sequence (substitution) opens a window at every occurrence of its TRIGGER
// signature among the events detected in the take. Rules:
//
//  1. Greedy segmentation (2026-07-23 report): the step is the event SPAN. A
//     trigger occurrence inside the previous window's step is INTERNAL (the
//     melody repeats its first note) and does not re-anchor. A REAL repetition
//     after the step does re-anchor and retrigger.
//
//  2. CLAIM (2026-08-21 report): an occurrence of S's trigger inside ANOTHER
//     Sequence T's window (with HD), where T contains that signature as a
//     MEMBER, belongs to T. S neither anchors nor triggers: "the one already
//     playing wins." In Golden Axe, "The Battle - Intro" and "- Loop" share
//     26 signatures; the hi-hat that opens the Intro reappears every 63 frames
//     inside the Loop, while the bass that opens the Loop appears in the Intro,
//     causing both to play at once.
//
//  3. HEAD (2026-08-21 report, second pass): a lone trigger is fragile. On the
//     third Loop pass, the opening bass uses ANOTHER signature (a variant),
//     while the other five channels start identically. Without the trigger,
//     the Loop failed to re-anchor, its window expired, and the Intro leaked in
//     (intro, loop, intro, loop...). The head is the set of signatures that
//     start on the SAME frame as the trigger. A Sequence also anchors when a
//     MAJORITY of its head starts (>= ceil(n/2), with n >= 2), even without the
//     trigger.
//
//  4. Frame tie (two Sequences start on the SAME frame): CONTINUATION wins
//     first—a looping Sequence whose window expires on that frame continues
//     ("always keep the one already playing unless it ended or the events
//     changed"). Next comes the most SPECIFIC Sequence (fewest member
//     signatures), then ID as a deterministic tie-breaker.
//
// Events are traversed in ascending `start_frame` order. The detector does NOT
// return them sorted (it emits them per channel), so the table applies a stable
// sort. The same traversal drives playback, bare-frame muting, and export
// mixdown: ONE table and one policy.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ayther {

/// Minimal view of a substitution for anchoring (copied from AudioSeqSub: the
/// tests do not need the whole session).
struct SeqAnchorSub {
    uint64_t              key = 0;
    uint64_t              trigger_signature = 0;
    uint32_t              duration_frames = 1;   ///< window (with the HD)
    uint32_t              span_frames = 0;       ///< segmentation step (0 = duration)
    bool                  enabled = true;        ///< asset assigned
    bool                  looping = false;       ///< looping HD (continuation)
    std::vector<uint64_t> signatures;            ///< member signatures
    std::vector<uint64_t> head;                  ///< signatures starting with the trigger
};

/// How many head signatures are needed to anchor without the trigger?
/// 0 = never (a single-signature head: the trigger only).
inline size_t seq_head_quorum(const SeqAnchorSub& s) {
    return s.head.size() >= 2 ? (s.head.size() + 1) / 2 : 0;
}

/// Does `claimer` claim an occurrence of `sig`? Yes if it is its trigger or a
/// member signature.
inline bool seq_sub_claims(const SeqAnchorSub& claimer, uint64_t sig) {
    if (claimer.trigger_signature == sig) return true;
    return std::find(claimer.signatures.begin(), claimer.signatures.end(), sig)
           != claimer.signatures.end();
}

/// Priority on a frame tie (without continuation): the most specific one.
inline bool seq_sub_before(const SeqAnchorSub& a, const SeqAnchorSub& b) {
    if (a.signatures.size() != b.signatures.size())
        return a.signatures.size() < b.signatures.size();
    return a.key < b.key;
}

/// Per-substitution state across frames (replay: local to the table; live: the
/// session holds it and keeps it in sync with its windows).
struct SeqAnchorState {
    uint32_t next_free = 0;    ///< segmentation step: before this = internal
    uint32_t win_start = 0, win_end = 0;   ///< current window [start, end)
    bool     open = false;
};

/// ONE frame: given the signatures that START at `f` (key-ons), decides which
/// substitutions anchor (in priority order) and updates `st`. It serves replay
/// (the table) and live playback (the detector rising edge) — one single
/// criterion.
inline std::vector<size_t>
seq_anchor_frame(uint32_t f, const std::vector<uint64_t>& sigs,
                 const std::vector<SeqAnchorSub>& subs, std::vector<SeqAnchorState>& st) {
    std::vector<size_t>   cand, order, anchored;
    std::vector<uint64_t> cand_sig;
    std::vector<size_t>   head_hits(subs.size(), 0);
    std::vector<uint8_t>  trig_hit(subs.size(), 0);
    if (st.size() != subs.size()) st.assign(subs.size(), SeqAnchorState{});
    for (const uint64_t sig : sigs) {
        for (size_t i = 0; i < subs.size(); ++i) {
            const SeqAnchorSub& sq = subs[i];
            if (!sq.enabled) continue;
            if (f < st[i].next_free) continue;   // internal occurrence
            const bool is_trig = sq.trigger_signature == sig;
            const bool is_head = is_trig ||
                std::find(sq.head.begin(), sq.head.end(), sig) != sq.head.end();
            if (!is_head) continue;
            if (is_trig) trig_hit[i] = 1;
            ++head_hits[i];
            const auto c = std::find(cand.begin(), cand.end(), i);
            if (c != cand.end()) {
                if (is_trig) cand_sig[static_cast<size_t>(c - cand.begin())] = sig;
                continue;
            }
            cand.push_back(i); cand_sig.push_back(sig);
        }
    }
    if (cand.empty()) return anchored;
    // Trigger present, or head quorum.
    for (size_t k = 0; k < cand.size();) {
        const size_t i = cand[k];
        const size_t q = seq_head_quorum(subs[i]);
        if (trig_hit[i] || (q && head_hits[i] >= q)) { ++k; continue; }
        cand.erase(cand.begin() + static_cast<std::ptrdiff_t>(k));
        cand_sig.erase(cand_sig.begin() + static_cast<std::ptrdiff_t>(k));
    }
    if (cand.empty()) return anchored;
    order.resize(cand.size());
    for (size_t k = 0; k < order.size(); ++k) order[k] = k;
    auto continues = [&](size_t i) {   // looping and its window expires here
        return subs[i].looping && st[i].open && st[i].win_end == f;
    };
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const bool ca = continues(cand[a]), cb = continues(cand[b]);
        if (ca != cb) return ca;
        return seq_sub_before(subs[cand[a]], subs[cand[b]]);
    });
    for (const size_t k : order) {
        const size_t        i  = cand[k];
        const SeqAnchorSub& sq = subs[i];
        bool claimed = false;
        for (size_t j = 0; j < subs.size() && !claimed; ++j) {
            if (j == i || !st[j].open) continue;
            if (f < st[j].win_start || f >= st[j].win_end) continue;
            claimed = seq_sub_claims(subs[j], cand_sig[k]);
        }
        if (claimed) continue;
        const uint32_t seg = sq.span_frames ? sq.span_frames : sq.duration_frames;
        st[i].next_free = f + (seg ? seg : 1u);
        st[i].open      = true;
        st[i].win_start = f;
        st[i].win_end   = f + sq.duration_frames;
        anchored.push_back(i);
    }
    return anchored;
}

/// Anchor table: substitution key → the starts of its windows (ascending).
/// `sig_of(i)` / `start_of(i)` read event i out of the `n` detected.
template <class SigOf, class StartOf>
inline std::unordered_map<uint64_t, std::vector<uint32_t>>
seq_anchor_table(size_t n, SigOf sig_of, StartOf start_of,
                 const std::vector<SeqAnchorSub>& subs) {
    std::unordered_map<uint64_t, std::vector<uint32_t>> out;
    std::vector<SeqAnchorState> st(subs.size());
    std::vector<uint64_t>       sigs;
    for (const auto& s : subs) out[s.key];
    // Ascending frame order, stable (the detector emits per channel).
    std::vector<size_t> idx(n);
    for (size_t i = 0; i < n; ++i) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(),
                     [&](size_t a, size_t b) { return start_of(a) < start_of(b); });
    for (size_t ei = 0; ei < n;) {
        const uint32_t f = start_of(idx[ei]);
        sigs.clear();
        size_t ej = ei;
        for (; ej < n && start_of(idx[ej]) == f; ++ej) sigs.push_back(sig_of(idx[ej]));
        ei = ej;
        for (const size_t i : seq_anchor_frame(f, sigs, subs, st))
            out[subs[i].key].push_back(f);
    }
    return out;
}

}  // namespace ayther
