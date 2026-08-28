// ---------------------------------------------------------------------------
// audio_match_identity_test — el timbre que emite el DETECTOR abre la cerradura
// del ÍNDICE de reglas (#389 Fase 1).
//
// POR QUÉ EXISTE. Había dos mitades probadas por separado y nadie probaba que
// encajaran:
//
//   · core/src/audio_event.rs (tests Rust) fija que el detector agrupa «la misma
//     voz» — instrument igual a través de nota, canal y pan;
//   · audio_match_rule_test.cpp fija el contrato del AudioMatchIndex — prioridad,
//     negativas y determinismo — pero con timbres CONSTANTES escritos a mano.
//
// Entre las dos queda el hueco donde vive el bug real: que el `instrument` que
// el detector calcula a partir de escrituras de chip sea el MISMO valor con el
// que el índice fue armado. Si esos dos hashes divergen, cada mitad sigue verde
// y la sustitución por regla no dispara nunca — que es exactamente el síntoma
// medido en vivo el 2026-08-11 sobre Golden Axe (`live_match_rule = 0` con
// siete reglas puestas), aunque allí la causa resultó ser otra.
//
// Ejercita la cadena entera sin ROM, sin SDL y sin GPU: escrituras de chip
// sintéticas → detector real (FFI del core) → AudioMatchIndex del engine.
//
// El caso de manual del issue es la ROTACIÓN DE CANAL: el driver mueve la misma
// voz de FM0 a FM1, la firma cambia (el canal entra en el hash) y la asignación
// autorada deja de disparar. Con la regla por instrumento, la variante resuelve
// a la firma autorada; sin ella, no resuelve — y esa diferencia es la razón de
// ser de la feature.
//
// Y lo hace sobre los canales ACTIVOS (ayther_audio_event_active), no sobre los
// eventos cerrados: el runtime live resuelve en el key-on, sin esperar al
// key-off (#389 F2/F3). Un test sobre eventos cerrados no probaría el camino
// que corre en vivo.
// ---------------------------------------------------------------------------
#include "audio_match_rule.h"
#include "ayther_core_ffi.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}

// --- Escrituras de chip: el mismo protocolo que usan los tests del core -----
constexpr uint8_t kChipFm = 0;

AytherAudioWrite fm(uint16_t addr, uint8_t data) {
    return AytherAudioWrite{/*cycle=*/0, addr, data, kChipFm};
}

constexpr uint8_t kChipPsg = 1;

AytherAudioWrite psg(uint8_t data) {
    return AytherAudioWrite{/*cycle=*/0, /*addr=*/0, data, kChipPsg};
}

// Registro FM del banco 0 (canales 0-2). UNA escritura: el core entrega el
// registro ya latcheado desde el fork 3fc6ee89 (2026-08-11) — el protocolo de
// puertos dejó de viajar por acá, y replicarlo dejaba al detector sin ver un
// solo evento.
void fm_write(std::vector<AytherAudioWrite>& w, uint8_t reg, uint8_t val) {
    w.push_back(fm(reg, val));
}

// Key-on/off del canal `ch` por el registro 0x28 (operadores todos on).
uint8_t fm_keyon(uint8_t ch)  { return uint8_t(0xF0 | (ch % 3) | (ch >= 3 ? 0x04 : 0)); }
uint8_t fm_keyoff(uint8_t ch) { return uint8_t((ch % 3) | (ch >= 3 ? 0x04 : 0)); }

void key(std::vector<AytherAudioWrite>& w, uint8_t byte) {
    w.push_back(fm(0x28, byte));
}

// Nota: 0xA4+ch (block + fnum hi) SIEMPRE antes de 0xA0+ch (fnum lo) — el chip
// latchea la palabra completa al escribir el registro bajo.
void fm_note(std::vector<AytherAudioWrite>& w, uint8_t ch,
             uint8_t hi, uint8_t lo) {
    fm_write(w, uint8_t(0xA4 + ch), hi);
    fm_write(w, uint8_t(0xA0 + ch), lo);
}

/// Patch de un canal del banco 0: DT/MUL del operador (lo que define el timbre).
void fm_patch(std::vector<AytherAudioWrite>& w, uint8_t ch, uint8_t mul) {
    fm_write(w, uint8_t(0x30 + ch), mul);
}

/// La voz activa del canal `ch` en este instante (firma + timbre + nota), tal
/// como la ve el runtime live. `false` si el canal no está sonando.
bool active_voice_on(AytherAudioEventDetector* d, uint8_t chip, uint8_t ch,
                     AytherAudioActive* out) {
    AytherAudioActive buf[16];
    const uint32_t n = ayther_audio_event_active(d, buf, 16);
    for (uint32_t i = 0; i < n && i < 16; ++i)
        if (buf[i].chip == chip && buf[i].channel == ch) {
            *out = buf[i];
            return true;
        }
    return false;
}

bool active_voice(AytherAudioEventDetector* d, uint8_t ch,
                  AytherAudioActive* out) {
    return active_voice_on(d, kChipFm, ch, out);
}

}  // namespace

int main() {
    std::printf("=== audio_match_identity_test — detector ↔ índice (#389 F1) ===\n");

    using ayther::AudioMatchIndex;
    using ayther::AudioMatchRule;
    using ayther::kAudioNoPitch;

    constexpr uint8_t kMulVozA = 0x71;   // el «patch» de la voz autorada
    constexpr uint8_t kMulVozB = 0x09;   // otra voz, otro timbre

    // -----------------------------------------------------------------------
    // 1. ROTACIÓN DE CANAL — el caso de manual del issue.
    //    La misma voz suena en FM0 (donde se autoró) y después en FM1.
    // -----------------------------------------------------------------------
    AytherAudioActive voice_ch0{}, voice_ch1{};
    {
        AytherAudioEventDetector* d = ayther_audio_event_new();

        std::vector<AytherAudioWrite> w;
        fm_patch(w, 0, kMulVozA);
        fm_note(w, 0, 0x24, 0x3B);            // A4 (fnum 1083 · block 4)
        key(w, fm_keyon(0));
        ayther_audio_event_process_frame(d, 0, w.data(), uint32_t(w.size()));
        check(active_voice(d, 0, &voice_ch0), "FM0 suena: hay voz activa al key-on");
        check(voice_ch0.instrument != 0,
              "el timbre viaja EN VIVO, sin esperar al key-off (#389 F2)");

        w.clear();
        key(w, fm_keyoff(0));
        ayther_audio_event_process_frame(d, 1, w.data(), uint32_t(w.size()));

        // El driver rota la voz a FM1: mismo patch, misma nota, otro canal.
        w.clear();
        fm_patch(w, 1, kMulVozA);
        fm_note(w, 1, 0x24, 0x3B);
        key(w, fm_keyon(1));
        ayther_audio_event_process_frame(d, 2, w.data(), uint32_t(w.size()));
        check(active_voice(d, 1, &voice_ch1), "FM1 suena: la voz rotó de canal");

        ayther_audio_event_free(d);
    }

    check(voice_ch0.signature != voice_ch1.signature,
          "el canal entra en la FIRMA: rotar de canal la fragmenta");
    check(voice_ch0.instrument == voice_ch1.instrument,
          "pero el TIMBRE es el mismo — es «el mismo sonido»");

    // La asignación autorada apunta a la firma que sonó en FM0.
    const uint64_t kSigAutorada = voice_ch0.signature;

    // -----------------------------------------------------------------------
    // 2. Sin regla (legacy exacto) la variante NO dispara; con la regla, sí.
    //    Ésta es la diferencia que justifica la feature.
    // -----------------------------------------------------------------------
    {
        AudioMatchIndex legacy;   // packs viejos: nada indexado
        uint64_t out = 0;
        check(!legacy.resolve(voice_ch1.instrument, voice_ch1.pitch, &out),
              "legacy: la voz rotada de canal queda SIN sustituir");

        AudioMatchIndex idx;
        idx.add(kSigAutorada, AudioMatchRule::kInstrument, voice_ch0.instrument,
                kAudioNoPitch);
        out = 0;
        check(idx.resolve(voice_ch1.instrument, voice_ch1.pitch, &out) &&
                  out == kSigAutorada,
              "con la regla: la voz rotada resuelve al asset autorado");
    }

    // -----------------------------------------------------------------------
    // 3. OTRA NOTA — el mismo patch tocando otra altura.
    // -----------------------------------------------------------------------
    AytherAudioActive voice_other_note{};
    {
        AytherAudioEventDetector* d = ayther_audio_event_new();
        std::vector<AytherAudioWrite> w;
        fm_patch(w, 0, kMulVozA);
        fm_note(w, 0, 0x2C, 0x00);            // otra octava, mismo patch
        key(w, fm_keyon(0));
        ayther_audio_event_process_frame(d, 0, w.data(), uint32_t(w.size()));
        check(active_voice(d, 0, &voice_other_note), "la voz suena en otra nota");
        ayther_audio_event_free(d);
    }

    check(voice_other_note.signature != kSigAutorada,
          "otra nota = otra firma (la frecuencia entra en el hash)");
    check(voice_other_note.instrument == voice_ch0.instrument,
          "otra nota, MISMO timbre");

    {
        AudioMatchIndex idx;
        idx.add(kSigAutorada, AudioMatchRule::kInstrument, voice_ch0.instrument,
                kAudioNoPitch);
        uint64_t out = 0;
        check(idx.resolve(voice_other_note.instrument, voice_other_note.pitch, &out) &&
                  out == kSigAutorada,
              "regla por instrumento: la nota no fragmenta");

        // La regla más específica sí distingue: instrumento + NOTA.
        AudioMatchIndex pitched;
        pitched.add(kSigAutorada, AudioMatchRule::kInstrumentPitch,
                    voice_ch0.instrument, voice_ch0.pitch);
        out = 0;
        check(pitched.resolve(voice_ch1.instrument, voice_ch1.pitch, &out) &&
                  out == kSigAutorada,
              "instrumento+nota: otra rotación de canal con la MISMA nota sí resuelve");
        out = 0;
        check(!pitched.resolve(voice_other_note.instrument, voice_other_note.pitch, &out),
              "instrumento+nota: otra ALTURA no resuelve (el autor pidió esa nota)");
    }

    // -----------------------------------------------------------------------
    // 4. NEGATIVA — un instrumento distinto jamás se sustituye por error.
    // -----------------------------------------------------------------------
    {
        AytherAudioEventDetector* d = ayther_audio_event_new();
        std::vector<AytherAudioWrite> w;
        fm_patch(w, 0, kMulVozB);             // otro timbre
        fm_note(w, 0, 0x24, 0x3B);            // MISMA nota que la autorada
        key(w, fm_keyon(0));
        ayther_audio_event_process_frame(d, 0, w.data(), uint32_t(w.size()));

        AytherAudioActive other_voice{};
        check(active_voice(d, 0, &other_voice), "suena una voz de otro timbre");
        check(other_voice.instrument != voice_ch0.instrument,
              "patch distinto → timbre distinto");

        AudioMatchIndex idx;
        idx.add(kSigAutorada, AudioMatchRule::kInstrument, voice_ch0.instrument,
                kAudioNoPitch);
        uint64_t out = 0;
        check(!idx.resolve(other_voice.instrument, other_voice.pitch, &out),
              "otro instrumento NO dispara el asset (sin falsos reemplazos)");

        ayther_audio_event_free(d);
    }

    // -----------------------------------------------------------------------
    // 5. PSG — el criterio de #389 pide comportamiento probado, no sólo FM.
    //    El tono del SN76489 es una cuadrada pura: NO tiene timbre propio, así
    //    que todos los canales de tono comparten instrumento. La consecuencia
    //    para el autor es fuerte y hay que fijarla: una regla por instrumento
    //    sobre un tono PSG alcanza a CUALQUIER tono PSG. El ruido sí tiene
    //    identidad propia (su registro de control), y no se confunde con tono.
    // -----------------------------------------------------------------------
    {
        AytherAudioEventDetector* d = ayther_audio_event_new();
        std::vector<AytherAudioWrite> w;

        w = {psg(uint8_t(0x80 | 0x0E)), psg(0x90)};   // ch0 tono, volumen on
        ayther_audio_event_process_frame(d, 0, w.data(), uint32_t(w.size()));
        AytherAudioActive psg0{};
        check(active_voice_on(d, kChipPsg, 0, &psg0), "PSG ch0 suena");

        w = {psg(uint8_t(0xA0 | 0x05)), psg(0xB0)};   // ch1 tono, otra nota
        ayther_audio_event_process_frame(d, 1, w.data(), uint32_t(w.size()));
        AytherAudioActive psg1{};
        check(active_voice_on(d, kChipPsg, 1, &psg1), "PSG ch1 suena");

        w = {psg(0xE4), psg(0xF0)};                   // ch3 RUIDO
        ayther_audio_event_process_frame(d, 2, w.data(), uint32_t(w.size()));
        AytherAudioActive ruido{};
        check(active_voice_on(d, kChipPsg, 3, &ruido), "PSG ruido suena");

        check(psg0.signature != psg1.signature, "PSG: canal y nota parten la FIRMA");
        check(psg0.instrument == psg1.instrument,
              "PSG: el tono es cuadrada pura — UN solo instrumento");
        check(ruido.instrument != psg0.instrument,
              "PSG: el ruido tiene identidad propia, no se confunde con tono");

        AudioMatchIndex idx;
        idx.add(psg0.signature, AudioMatchRule::kInstrument, psg0.instrument,
                kAudioNoPitch);
        uint64_t out = 0;
        check(idx.resolve(psg1.instrument, psg1.pitch, &out) &&
                  out == psg0.signature,
              "PSG: la regla alcanza a CUALQUIER tono (documentado: es amplia)");
        out = 0;
        check(!idx.resolve(ruido.instrument, ruido.pitch, &out),
              "PSG: …pero nunca al ruido");

        ayther_audio_event_free(d);
    }

    // -----------------------------------------------------------------------
    // 6. DAC — el stream PCM de FM6 no tiene patch: su identidad ES su firma
    //    (convención del detector). Consecuencia para el autor: una regla por
    //    instrumento sobre un disparador DAC no amplía NADA — se comporta como
    //    exacta. Es inofensiva, pero conviene que esté fijada por un test para
    //    que nadie prometa en la UI algo que el DAC no puede dar.
    // -----------------------------------------------------------------------
    {
        AytherAudioEventDetector* d = ayther_audio_event_new();
        std::vector<AytherAudioWrite> w;
        fm_write(w, 0x2B, 0x80);            // DAC enable
        fm_write(w, 0x2A, 0x40);            // muestras que VARÍAN = con señal
        fm_write(w, 0x2A, 0xC0);
        ayther_audio_event_process_frame(d, 0, w.data(), uint32_t(w.size()));

        AytherAudioActive dac{};
        check(active_voice_on(d, kChipFm, 5, &dac), "el DAC suena (FM6 → canal 5)");
        check(dac.instrument == dac.signature,
              "DAC: la identidad del timbre ES la firma (sin patch que hashear)");
        check(dac.pitch == kAudioNoPitch, "DAC: sin altura");

        AudioMatchIndex idx;
        idx.add(dac.signature, AudioMatchRule::kInstrument, dac.instrument,
                kAudioNoPitch);
        uint64_t out = 0;
        check(idx.resolve(dac.instrument, dac.pitch, &out) &&
                  out == dac.signature,
              "DAC: la regla resuelve su propia firma…");
        out = 0;
        check(!idx.resolve(dac.signature ^ 0x1ull, kAudioNoPitch, &out),
              "…y NADA más: sobre DAC, `instrument` equivale a exacta");

        ayther_audio_event_free(d);
    }

    if (g_fail) {
        std::printf("\n%d FALLOS\n", g_fail);
        return 1;
    }
    std::printf("\nTodo OK\n");
    return 0;
}
