#pragma once
// ---------------------------------------------------------------------------
// audio_bus_balance.h — normalización ENTRE BUSES (, segunda mitad).
//
// La primera mitad —medir cada asset (peak, RMS, clipping, corrección
// sugerida)— ya está en `audio_asset_level.h` y se ve en Mezclar. Con eso el
// autor arregla un asset que clipea o uno que se pierde. Lo que no arregla es
// el problema que aparece recién cuando el pack está completo: **la música y
// los efectos, cada uno bien por su cuenta, no se llevan bien entre sí**. Un
// pack donde cada golpe tapa el tema está compuesto de assets impecables.
//
// Por qué es un cálculo aparte y no «el mismo, promediado»:
//
//   · Se promedia ENERGÍA, no decibeles. El promedio de dB de -6 y -30 da -18,
//     que no es el nivel que se oye: el de -6 domina. Promediar la energía y
//     recién ahí pasar a dB es lo que da un número que corresponde a lo que
//     suena.
//   · Se pondera por DURACIÓN. Un tema de tres minutos y un golpe de 200 ms
//     no aportan lo mismo a la sensación de volumen de su bus, y contarlos
//     igual haría que un pack con muchos efectos cortos pareciera fuerte.
//   · Cuenta la ganancia YA AUTORADA. El autor pudo bajar un asset a mano; el
//     balance tiene que mirar lo que se va a oír, no lo que dice el archivo.
//
// LA REFERENCIA ES LA MÚSICA. Es el material continuo: es contra lo que el
// oído fija el nivel de la escena, y es lo que sigue sonando cuando no pasa
// nada. Sin música clasificada, la referencia es el bus con más material
// medido — y si sólo hay uno, no hay nada que balancear y se dice.
//
// Lo que este cálculo NO hace: tocar el archivo, ni la ganancia por asset. Su
// salida es una corrección POR BUS, que es exactamente lo que
// `AytherSession::set_bus_volume` aplica en vivo y el proyecto persiste.
// Corregir por bus y no por asset también es lo honesto con el dato: lo que se
// midió desbalanceado es el bus.
//
// Sin estado y en el header público —con su oráculo, `audio_bus_balance`—
// porque la regla es del CÁLCULO y así se prueba sin motor de audio.
// ---------------------------------------------------------------------------
#include <cmath>
#include <cstdint>
#include <vector>

namespace ayther {

/// Un asset medido, con su bus y su ganancia autorada.
struct AudioBusSample {
    uint8_t bus        = 0;      ///< AudioBus (0 = Unclassified)
    float   rms        = 0.0f;   ///< 0..1 lineal, del análisis del asset
    double  duration_s = 0.0;
    float   gain       = 1.0f;   ///< ganancia autorada del asset/Secuencia (lineal)
};

/// Cómo quedó un bus.
struct AudioBusLevel {
    uint32_t count      = 0;         ///< assets medidos que cayeron acá
    double   seconds    = 0.0;       ///< material total
    float    level_db   = -120.0f;   ///< nivel efectivo (energía ponderada)
    /// Corrección para alinearlo con la referencia. 0 en la referencia misma
    /// y en un bus sin material.
    float    correction_db = 0.0f;
    /// El desbalance supera el margen: es lo que se muestra como advertencia.
    bool     out_of_range  = false;
};

struct AudioBusBalance {
    /// Indexado por AudioBus (0..Count). El bus 0 (`Unclassified`) se mide
    /// igual que los demás pero NUNCA es la referencia ni recibe corrección:
    /// «no sé qué es esto» no puede definir el nivel del pack, y corregirlo
    /// sería mover junto material que no tiene nada que ver entre sí.
    std::vector<AudioBusLevel> buses;
    uint8_t reference     = 0;       ///< bus tomado como referencia (0 = ninguno)
    bool    has_reference = false;
    /// Menos de dos buses con material: no hay nada que balancear. Es distinto
    /// de «está balanceado», y decirlo evita que la UI muestre un visto bueno
    /// que no se ganó.
    bool    comparable    = false;
};

/// dBFS de una amplitud lineal. -120 es el piso (silencio).
inline float ay_lin_to_db(double lin) {
    return lin > 1e-6 ? (float)(20.0 * std::log10(lin)) : -120.0f;
}
inline float ay_db_to_lin(float db) {
    return db <= -120.0f ? 0.0f : (float)std::pow(10.0, db / 20.0);
}

/// El balance entre buses. `bus_count` = `kAudioBusCount`.
///
/// `margin_db` es cuánto desbalance se tolera antes de avisar (6 dB por
/// defecto: es el doble de amplitud, el punto donde un bus deja de acompañar y
/// empieza a tapar). `max_correction_db` acota lo que se ofrece corregir: una
/// corrección de 30 dB no es un balance, es un material equivocado, y
/// aplicarla en silencio taparía el problema real.
inline AudioBusBalance audio_bus_balance(const std::vector<AudioBusSample>& samples,
                                         uint32_t bus_count,
                                         float margin_db = 6.0f,
                                         float max_correction_db = 12.0f) {
    AudioBusBalance out;
    out.buses.assign(bus_count, AudioBusLevel{});
    if (bus_count == 0) return out;

    // Energía ponderada por duración: sum(rms² · s) / sum(s).
    std::vector<double> energy(bus_count, 0.0);
    for (const AudioBusSample& s : samples) {
        if (s.bus >= bus_count) continue;
        // Un asset sin duración no aporta: contarlo con peso cero es lo mismo
        // que no medirlo, y contarlo con peso uno le daría a un archivo vacío
        // el mismo voto que a un tema.
        if (!(s.duration_s > 0.0) || !(s.rms > 0.0f)) continue;
        const double eff = (double)s.rms * (double)(s.gain < 0.0f ? 0.0f : s.gain);
        energy[s.bus]  += eff * eff * s.duration_s;
        out.buses[s.bus].seconds += s.duration_s;
        out.buses[s.bus].count   += 1;
    }
    for (uint32_t b = 0; b < bus_count; ++b) {
        AudioBusLevel& L = out.buses[b];
        if (L.count == 0 || L.seconds <= 0.0) continue;
        L.level_db = ay_lin_to_db(std::sqrt(energy[b] / L.seconds));
    }

    // La referencia: Música (bus 1) si tiene material; si no, el bus
    // clasificado con más segundos. `Unclassified` (0) nunca.
    uint32_t medidos = 0;
    for (uint32_t b = 1; b < bus_count; ++b) if (out.buses[b].count) ++medidos;
    out.comparable = medidos >= 2;

    uint8_t ref = 0;
    if (bus_count > 1 && out.buses[1].count) {
        ref = 1;
    } else {
        double best = 0.0;
        for (uint32_t b = 1; b < bus_count; ++b)
            if (out.buses[b].count && out.buses[b].seconds > best) {
                best = out.buses[b].seconds; ref = (uint8_t)b;
            }
    }
    if (!ref) return out;                 // nada clasificado: no hay referencia
    out.reference = ref; out.has_reference = true;

    const float ref_db = out.buses[ref].level_db;
    for (uint32_t b = 1; b < bus_count; ++b) {
        AudioBusLevel& L = out.buses[b];
        if (!L.count || b == ref) continue;
        const float delta = ref_db - L.level_db;      // + = hay que subirlo
        L.out_of_range = std::fabs(delta) > margin_db;
        L.correction_db = delta >  max_correction_db ?  max_correction_db
                        : delta < -max_correction_db ? -max_correction_db
                        : delta;
    }
    return out;
}

}  // namespace ayther
