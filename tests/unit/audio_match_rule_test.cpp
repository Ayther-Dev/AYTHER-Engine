// ---------------------------------------------------------------------------
// audio_match_rule_test — reglas de match de la sustitución por evento (#389
// Fase 3).
//
// POR QUÉ EXISTE. La firma exacta fragmenta la identidad: el MISMO instrumento
// en otra nota, canal o pan es OTRA firma, y el reemplazo autorado no dispara
// (medido en la transición pantalla→demo de Amazona: un instrumento asignado
// sonando en original con ≥6 firmas hermanas; una melodía parte un instrumento
// en hasta 16 firmas). La regla — opt-in y persistida con la asignación —
// resuelve esas variantes al MISMO asset sin tocar la clave legacy.
//
// Fija el contrato de audio_match_rule.h: prioridad (exacta la chequea el
// caller y SIEMPRE gana; después instrumento+nota; después instrumento),
// negativas (timbre desconocido o distinto jamás matchea — sin falsos
// reemplazos entre instrumentos), y determinismo en empates.
// ---------------------------------------------------------------------------
#include "audio_match_rule.h"

#include <cstdio>
#include <string>

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}

}  // namespace

int main() {
    std::printf("=== audio_match_rule_test — reglas de match (#389 F3) ===\n");
    using ayther::AudioMatchIndex;
    using ayther::AudioMatchRule;
    using ayther::kAudioNoPitch;

    constexpr uint64_t kInstrFm5  = 0x2f354fbdd69a202d;   // el de la fuga medida
    constexpr uint64_t kInstrLead = 0x878badc3054224bb;   // 1 instrumento, 16 firmas
    constexpr uint64_t kSigHit    = 0x1111;
    constexpr uint64_t kSigNoteA4 = 0x2222;

    // ---- Regla por INSTRUMENTO: canal/nota/pan no fragmentan ---------------
    {
        AudioMatchIndex idx;
        idx.add(kSigHit, AudioMatchRule::kInstrument, kInstrFm5, kAudioNoPitch);
        uint64_t out = 0;
        check(idx.resolve(kInstrFm5, 69, &out) && out == kSigHit,
              "misma voz en otra nota resuelve a la firma autorada");
        check(idx.resolve(kInstrFm5, kAudioNoPitch, &out) && out == kSigHit,
              "sin altura (DAC) también matchea la regla por instrumento");
        check(!idx.resolve(kInstrLead, 69, &out),
              "un instrumento DISTINTO jamás matchea (sin falsos reemplazos)");
        check(!idx.resolve(0, 69, &out),
              "timbre desconocido (0) jamás matchea");
    }

    // ---- Regla INSTRUMENTO+NOTA: la nota gatea -----------------------------
    {
        AudioMatchIndex idx;
        idx.add(kSigNoteA4, AudioMatchRule::kInstrumentPitch, kInstrLead, 69);
        uint64_t out = 0;
        check(idx.resolve(kInstrLead, 69, &out) && out == kSigNoteA4,
              "misma nota en otro canal/pan resuelve");
        check(!idx.resolve(kInstrLead, 70, &out),
              "otra nota NO matchea la regla instrumento+nota");
        check(!idx.resolve(kInstrLead, kAudioNoPitch, &out),
              "voz sin altura NO matchea una regla con nota");
    }

    // ---- Prioridad: instrumento+nota gana sobre instrumento ----------------
    {
        AudioMatchIndex idx;
        idx.add(kSigHit,    AudioMatchRule::kInstrument,      kInstrLead,
                kAudioNoPitch);
        idx.add(kSigNoteA4, AudioMatchRule::kInstrumentPitch, kInstrLead, 69);
        uint64_t out = 0;
        check(idx.resolve(kInstrLead, 69, &out) && out == kSigNoteA4,
              "con la nota coincidente gana la regla más específica");
        check(idx.resolve(kInstrLead, 70, &out) && out == kSigHit,
              "con otra nota cae a la regla por instrumento");
    }

    // ---- Determinismo: empate = la firma autorada más baja -----------------
    {
        AudioMatchIndex idx;
        idx.add(0x9999, AudioMatchRule::kInstrument, kInstrFm5, kAudioNoPitch);
        idx.add(0x0100, AudioMatchRule::kInstrument, kInstrFm5, kAudioNoPitch);
        idx.add(0x5000, AudioMatchRule::kInstrument, kInstrFm5, kAudioNoPitch);
        uint64_t out = 0;
        check(idx.resolve(kInstrFm5, 60, &out) && out == 0x0100,
              "dos reglas del mismo instrumento resuelven determinista (menor)");
    }

    // ---- Entradas que no pueden matchear no entran al índice ---------------
    {
        AudioMatchIndex idx;
        idx.add(kSigHit, AudioMatchRule::kExact, kInstrFm5, 69);
        check(idx.empty(), "una regla exacta no indexa (la resuelve el caller)");
        idx.add(kSigHit, AudioMatchRule::kInstrument, 0, 69);
        check(idx.empty(), "regla sin instrumento no indexa");
        idx.add(kSigHit, AudioMatchRule::kInstrumentPitch, kInstrFm5,
                kAudioNoPitch);
        check(idx.empty(), "instrumento+nota SIN nota no indexa");
        uint64_t out = 0;
        check(!idx.resolve(kInstrFm5, 69, &out), "índice vacío no resuelve");
    }

    // ---- clear vuelve al legacy (todo exacto) ------------------------------
    {
        AudioMatchIndex idx;
        idx.add(kSigHit, AudioMatchRule::kInstrument, kInstrFm5, kAudioNoPitch);
        idx.clear();
        uint64_t out = 0;
        check(!idx.resolve(kInstrFm5, 69, &out),
              "clear borra las reglas (packs legacy: solo firma exacta)");
    }

    if (g_fail) {
        std::printf("\n%d FALLOS\n", g_fail);
        return 1;
    }
    std::printf("\nTodo OK\n");
    return 0;
}
