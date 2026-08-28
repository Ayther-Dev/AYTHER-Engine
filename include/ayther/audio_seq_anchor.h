#pragma once
// ---------------------------------------------------------------------------
// audio_seq_anchor.h — anclas de las Secuencias en replay, con RECLAMO entre
// Secuencias (). Header-only y puro: testeable sin SDL, sin core, sin GPU.
//
// Una Secuencia (sub) abre una ventana en cada ocurrencia de su firma
// DISPARADORA dentro de los eventos detectados de la toma. Reglas:
//
//  1. Segmentación greedy (reporte 2026-07-23): el paso es el SPAN de los
//     eventos; una ocurrencia del disparador que cae dentro del paso de la
//     ventana anterior es INTERNA (la melodía repite su primera nota) y no
//     re-ancla. Una repetición REAL (tras el paso) sí re-ancla y re-dispara.
//
//  2. RECLAMO (, reporte 2026-08-21): una ocurrencia del disparador de S
//     que cae dentro de la ventana (con HD) de OTRA Secuencia T que tiene esa
//     firma como MIEMBRO es de T — S no ancla ni dispara. «La que se estaba
//     escuchando gana.» Caso real (Golden Axe): «The Battle - Intro» y
//     «- Loop» comparten 26 firmas; el hi-hat que abre la Intro reaparece
//     cada 63 frames dentro del Loop, y el bajo que abre el Loop aparece
//     dentro de la Intro → sonaban las dos a la vez.
//
//  3. CABEZA (reporte 2026-08-21, 2ª vuelta): el disparador solo es frágil —
//     en la 3ª pasada del Loop el bajo que lo abre es OTRA firma (variante)
//     y los otros 5 canales arrancan idénticos; sin disparador el Loop no
//     re-anclaba, su ventana vencía y la Intro se colaba (intro, loop,
//     intro, loop…). La cabeza = las firmas que arrancan en el MISMO frame
//     que el disparador; una Secuencia también ancla cuando arranca la
//     MAYORÍA de su cabeza (≥ ⌈n/2⌉, con n ≥ 2) aunque falte el disparador.
//
//  4. Empate de frame (dos Secuencias arrancan en el MISMO frame): primero
//     la CONTINUACIÓN — una en loop cuya ventana vence justo en ese frame
//     sigue («siempre la que viene sonando, salvo que haya terminado o
//     cambien los eventos»); después la más ESPECÍFICA (menos firmas
//     miembro); desempate por id. Determinista.
//
// Los eventos se recorren en orden ascendente de start_frame — el detector
// NO los entrega ordenados (los emite por canal), así que la tabla los ordena
// (estable). El mismo recorrido alimenta el playback, el mute de los frames
// bare y el mixdown del export: UNA tabla, un solo criterio.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ayther {

/// Vista mínima de una sub para el anclaje (copiada de AudioSeqSub: las
/// pruebas no necesitan la sesión entera).
struct SeqAnchorSub {
    uint64_t              key = 0;
    uint64_t              trigger_signature = 0;
    uint32_t              duration_frames = 1;   ///< ventana (con el HD)
    uint32_t              span_frames = 0;       ///< paso de segmentación (0 = duration)
    bool                  enabled = true;        ///< asset asignado
    bool                  looping = false;       ///< HD en loop (continuación)
    std::vector<uint64_t> signatures;            ///< firmas miembro
    std::vector<uint64_t> head;                  ///< firmas que arrancan con el disparador
};

/// ¿Cuántas firmas de la cabeza hacen falta para anclar sin el disparador?
/// 0 = nunca (cabeza de una sola firma: sólo el disparador).
inline size_t seq_head_quorum(const SeqAnchorSub& s) {
    return s.head.size() >= 2 ? (s.head.size() + 1) / 2 : 0;
}

/// ¿`claimer` reclama una ocurrencia de `sig`? Sí si es su disparador o una
/// firma miembro.
inline bool seq_sub_claims(const SeqAnchorSub& claimer, uint64_t sig) {
    if (claimer.trigger_signature == sig) return true;
    return std::find(claimer.signatures.begin(), claimer.signatures.end(), sig)
           != claimer.signatures.end();
}

/// Prioridad en el empate de frame (sin continuación): la más específica.
inline bool seq_sub_before(const SeqAnchorSub& a, const SeqAnchorSub& b) {
    if (a.signatures.size() != b.signatures.size())
        return a.signatures.size() < b.signatures.size();
    return a.key < b.key;
}

/// Estado por sub entre frames (replay: local a la tabla; vivo: lo guarda la
/// sesión y lo sincroniza con sus ventanas).
struct SeqAnchorState {
    uint32_t next_free = 0;    ///< paso de segmentación: antes de esto = interna
    uint32_t win_start = 0, win_end = 0;   ///< ventana vigente [start, end)
    bool     open = false;
};

/// UN frame: dadas las firmas que ARRANCAN en `f` (key-ons), decide qué subs
/// anclan (en orden de prioridad) y actualiza `st`. Vale para el replay (la
/// tabla) y para el vivo (el flanco de subida del detector) — un solo criterio.
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
            if (f < st[i].next_free) continue;   // ocurrencia interna
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
    // Disparador presente, o quórum de la cabeza.
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
    auto continues = [&](size_t i) {   // en loop y su ventana vence acá
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

/// Tabla de anclas: key de la sub → starts de sus ventanas (ascendentes).
/// `sig_of(i)` / `start_of(i)` leen el evento i de los `n` detectados.
template <class SigOf, class StartOf>
inline std::unordered_map<uint64_t, std::vector<uint32_t>>
seq_anchor_table(size_t n, SigOf sig_of, StartOf start_of,
                 const std::vector<SeqAnchorSub>& subs) {
    std::unordered_map<uint64_t, std::vector<uint32_t>> out;
    std::vector<SeqAnchorState> st(subs.size());
    std::vector<uint64_t>       sigs;
    for (const auto& s : subs) out[s.key];
    // Orden ascendente de frame, estable (el detector emite por canal).
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
