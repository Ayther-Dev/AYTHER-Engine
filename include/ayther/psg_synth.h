// ---------------------------------------------------------------------------
// PsgSynth — SN76489 (PSG) propio, con salida POR CANAL. Fase 1 de .
//
// POR QUÉ PROPIO. ymfm cubre el YM2612 pero no el SN76489, y las
// implementaciones que andan dando vueltas (MAME, el propio GPGX) son GPL:
// ayther_engine es una lib ESTÁTICA y meterle GPL la contamina entera. El chip
// es chico de verdad —tres generadores de onda cuadrada y un LFSR— así que
// escribirlo sale más barato que discutir licencias.
//
// `gpgx-src/core/sound/psg.c` es la referencia de COMPORTAMIENTO (constantes,
// tabla de volumen, red de realimentación del ruido), no de código.
//
// TASA INTERNA. El chip corre a MCLK/15 y divide por 16, o sea un tick cada
// 15*16 = 240 M-cycles → 223721,56 Hz con el reloj NTSC. Es exactamente 4,2×
// la tasa del YM2612 (MCLK/1008), y no es casualidad: 1008/240 = 4,2.
//
// Ese detalle importa. TODOS los incrementos de frecuencia del PSG son
// múltiplos de 240 M-cycles, así que cada transición de la onda cae JUSTO en un
// borde de tick — muestrear a 223721 Hz es exacto, sin jitter de sub-muestra y
// sin necesidad de síntesis band-limited acá. El aliasing se maneja después, al
// decimar a la tasa de salida con el resampler.
// ---------------------------------------------------------------------------

#pragma once

#include <cstdint>

namespace ayther {

class PsgSynth {
public:
    static constexpr int      kChannels       = 4;        // 3 tonos + ruido
    static constexpr uint32_t kMCyclesPerTick = 15 * 16;  // 240

    PsgSynth() { reset(); }

    void reset();

    /// Un byte al puerto del PSG, tal como lo registra el fork (chip == PSG,
    /// addr == 0). El chip tiene un solo puerto de escritura: el byte se
    /// auto-describe (bit 7 = latch de registro, o dato de continuación).
    void write(uint8_t data);

    /// Avanza un tick interno y deja en `out` la salida de los 4 canales,
    /// normalizada al mismo orden de magnitud que ymfm.
    ///
    /// Cada canal sale UNIPOLAR (0 o volumen), igual que el modelo del chip.
    /// Eso mete continua, y el que la saca es el bloqueo de continua del router
    /// — la misma corrección que necesita el FM aislado y por el mismo motivo:
    /// una voz que arranca o para sería un escalón, o sea un clic.
    void tick(float out[kChannels]);

private:
    void shift_noise();

    int regs_[8]      = {};   // 0/2/4 = período de tono · 1/3/5/7 = volumen · 6 = control de ruido
    int latch_        = 3;
    int inc_[4]       = {};   // período en TICKS (no en M-cycles)
    int counter_[4]   = {};
    int polarity_[3]  = {};
    int noise_pol_    = 0;
    int noise_shift_  = 0;
};

}  // namespace ayther
