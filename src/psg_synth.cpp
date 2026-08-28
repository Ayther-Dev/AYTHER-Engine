// ---------------------------------------------------------------------------
// PsgSynth — SN76489. Ver psg_synth.h para el porqué y la tasa interna.
//
// Todas las constantes salen de gpgx-src/core/sound/psg.c, que a su vez las
// tiene medidas contra hardware. Se copian los NÚMEROS y el comportamiento, no
// el código (licencia).
// ---------------------------------------------------------------------------

#include "psg_synth.h"

namespace ayther {
namespace {

// 16 pasos de 2 dB desde el máximo por canal (2800, ajustado en psg.c:52 al
// balance PSG/FM de una MD1 VA4). El último paso es SILENCIO, no −30 dB.
constexpr int kChanVolume[16] = {
    2800, 2224, 1766, 1403, 1114,  885,  703,  558,
     443,  352,  280,  222,  176,  140,  111,    0,
};

// Variante INTEGRADA (la del VDP de la Mega Drive), no la discreta:
// LFSR de 15 bits, realimentación por los bits 0 y 3.
constexpr int kNoiseShiftWidth = 15;
constexpr int kNoiseBitMask    = 0x9;
constexpr int kNoiseFeedback[10] = { 0, 1, 1, 0, 1, 0, 0, 1, 1, 0 };

// En la versión integrada un período de 0 se comporta como 1 (en la discreta
// sería 0x400). psg.c:117.
constexpr int kZeroFreqInc = 1;

}  // namespace

void PsgSynth::reset() {
    for (int i = 0; i < 8; ++i) regs_[i] = 0;
    latch_ = 3;
    for (int i = 0; i < kChannels; ++i) {
        inc_[i]     = (i < 3) ? kZeroFreqInc : 16;
        counter_[i] = inc_[i];
    }
    for (int i = 0; i < 3; ++i) polarity_[i] = 1;
    noise_pol_   = 1;
    noise_shift_ = 1 << kNoiseShiftWidth;
}

void PsgSynth::write(uint8_t data) {
    int index;
    if (data & 0x80) latch_ = index = (data >> 4) & 0x07;   // 1xxx---- : latchea registro
    else             index = latch_;                        // 0------- : continúa el anterior

    switch (index) {
        case 0: case 2: case 4: {          // frecuencia de un canal de tono
            int v;
            if (data & 0x80) v = (regs_[index] & 0x3F0) | (data & 0x0F);        // LSB
            else             v = (regs_[index] & 0x00F) | ((data & 0x3F) << 4); // MSB
            inc_[index >> 1] = v ? v : kZeroFreqInc;
            // El ruido puede estar enganchado al tono 3: si lo está, lo sigue.
            if (index == 4 && (regs_[6] & 0x03) == 0x03) inc_[3] = inc_[2];
            regs_[index] = v;
            break;
        }
        case 6: {                          // control de ruido
            const int nf = data & 0x03;
            if (nf == 0x03) {              // el ruido corre a la frecuencia del tono 3
                inc_[3]     = inc_[2];
                counter_[3] = counter_[2];
            } else {
                inc_[3] = 0x10 << nf;      // 16 / 32 / 64 ticks
            }
            // Escribir el control REINICIA el registro de desplazamiento — la
            // salida del canal de ruido se fuerza a bajo.
            noise_shift_ = 1 << kNoiseShiftWidth;
            regs_[6] = data;
            break;
        }
        default:                           // 1/3/5/7: atenuación de 4 bits
            regs_[index] = kChanVolume[data & 0x0F];
            break;
    }
}

void PsgSynth::shift_noise() {
    const int out = noise_shift_ & 0x01;
    if (regs_[6] & 0x04) {
        // Ruido BLANCO: desplazar y realimentar por la red XOR.
        noise_shift_ = (noise_shift_ >> 1) |
                       (kNoiseFeedback[noise_shift_ & kNoiseBitMask] << kNoiseShiftWidth);
    } else {
        // Ruido PERIÓDICO: desplazar realimentando la propia salida — da un
        // tono zumbón, no ruido.
        noise_shift_ = (noise_shift_ >> 1) | (out << kNoiseShiftWidth);
    }
}

void PsgSynth::tick(float out[kChannels]) {
    constexpr float kScale = 1.0f / 32768.0f;

    for (int i = 0; i < 3; ++i) {
        if (--counter_[i] <= 0) {
            counter_[i]  = inc_[i];
            polarity_[i] = -polarity_[i];
        }
        // Unipolar, como el chip: o el volumen del canal, o nada.
        out[i] = (polarity_[i] > 0) ? float(regs_[i * 2 + 1]) * kScale : 0.0f;
    }

    if (--counter_[3] <= 0) {
        counter_[3] = inc_[3];
        noise_pol_  = -noise_pol_;
        // El registro se desplaza SÓLO en el flanco positivo: media frecuencia.
        if (noise_pol_ > 0) shift_noise();
    }
    out[3] = (noise_shift_ & 1) ? float(regs_[7]) * kScale : 0.0f;
}

}  // namespace ayther
