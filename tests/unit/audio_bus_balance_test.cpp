// ---------------------------------------------------------------------------
// audio_bus_balance_test.cpp — #291: la normalización ENTRE BUSES.
//
// Lo que este cálculo tiene que atrapar es el pack donde cada asset está bien y
// el conjunto no: la música a -20 dB y los efectos a -8, cada uno impecable por
// su cuenta. Por eso los casos de acá son de CONJUNTO, y los dos que más
// importan son los que separan este cálculo de «promediar los dB»:
//
//   · promediar decibeles da un número que no es el que se oye (el fuerte
//     domina), y
//   · contar un golpe de 200 ms igual que un tema de 3 minutos hace que un
//     pack con muchos efectos parezca más fuerte de lo que suena.
//
// Los dos tienen su caso con el número de la regla vieja escrito al lado, para
// que la diferencia sea demostrable y no una afirmación del comentario.
// ---------------------------------------------------------------------------
#include "audio_bus_balance.h"

#include <cstdio>
#include <cmath>

using namespace ayther;

namespace {

int g_pass = 0, g_fail = 0;
void check(bool ok, const char* what) {
    if (ok) ++g_pass; else ++g_fail;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}
bool near(float a, float b, float tol = 0.15f) { return std::fabs(a - b) <= tol; }

// Los buses, como en ayther_session.h: 0 sin clasificar · 1 Música · 2 Efectos
// · 3 Voces.
constexpr uint32_t kBuses = 4;
constexpr uint8_t  kUnclassified = 0, kMusic = 1, kEffects = 2, kVoices = 3;

AudioBusSample sample(uint8_t bus, float rms_db, double dur, float gain = 1.0f) {
    return AudioBusSample{ bus, ay_db_to_lin(rms_db), dur, gain };
}

}  // namespace

int main() {
    std::printf("== audio_bus_balance_test (#291) ==\n");

    // -- El caso de la issue: cada asset bien, el conjunto mal ---------------
    {
        std::vector<AudioBusSample> v = {
            sample(kMusic,  -20.0f, 120.0),
            sample(kMusic,  -20.0f, 100.0),
            sample(kEffects,  -8.0f,   1.0),
            sample(kEffects,  -8.0f,   1.0),
        };
        const AudioBusBalance b = audio_bus_balance(v, kBuses);
        check(b.has_reference && b.reference == kMusic,
              "la MÚSICA es la referencia: es el material continuo");
        check(near(b.buses[kMusic].level_db, -20.0f),
              "el nivel de la música es el medido");
        check(near(b.buses[kEffects].level_db, -8.0f),
              "el de los efectos, también");
        check(b.buses[kEffects].out_of_range,
              "12 dB de diferencia: se avisa (el margen es 6)");
        check(near(b.buses[kEffects].correction_db, -12.0f),
              "y la corrección los baja 12 dB hasta la música");
        check(b.buses[kMusic].correction_db == 0.0f,
              "la referencia no se corrige a sí misma");
    }

    // -- Se promedia ENERGÍA, no decibeles ------------------------------------
    {
        // Un bus con un asset a -6 y otro a -30. El promedio de dB daría -18;
        // lo que se oye está dominado por el de -6, y la energía lo dice:
        // sqrt((10^-0.6 + 10^-3)/2) ≈ -9,0 dB.
        std::vector<AudioBusSample> v = {
            sample(kEffects,  -6.0f, 10.0),
            sample(kEffects, -30.0f, 10.0),
            sample(kMusic,   -9.0f, 10.0),
        };
        const AudioBusBalance b = audio_bus_balance(v, kBuses);
        check(near(b.buses[kEffects].level_db, -9.0f, 0.3f),
              "energía: el bus mide ~-9 dB, no el -18 del promedio de dB");
        check(!b.buses[kEffects].out_of_range,
              "…y por eso NO se avisa contra una música a -9: están parejos");
        // El control: con el promedio de dB, el bus habría dado -18 y la
        // diferencia con la música (9 dB) habría disparado la advertencia.
        const float average_db = (-6.0f + -30.0f) / 2.0f;
        check(std::fabs(-9.0f - average_db) > 6.0f,
              "control: la regla vieja SÍ habría avisado (falso positivo)");
    }

    // -- Se pondera por DURACIÓN ---------------------------------------------
    {
        // Un tema largo y flojo, y un golpe corto y fuerte, en el mismo bus. Si
        // pesaran igual, el bus mediría near del golpe.
        std::vector<AudioBusSample> v = {
            sample(kEffects, -24.0f, 180.0),   // 3 minutos
            sample(kEffects,  -6.0f,   0.2),   // 200 ms
            sample(kMusic,  -24.0f,  60.0),
        };
        const AudioBusBalance b = audio_bus_balance(v, kBuses);
        check(near(b.buses[kEffects].level_db, -24.0f, 0.5f),
              "duración: los 200 ms fuertes casi no mueven el nivel del bus");
        check(!b.buses[kEffects].out_of_range,
              "…así que no se pide corregir un bus que está bien");
        // Sin ponderar, la energía media de los dos daría ≈ -9,0 dB: 15 dB de
        // diferencia con la música y una corrección inventada.
        const double e = (std::pow(10.0, -2.4) + std::pow(10.0, -0.6)) / 2.0;
        check(ay_lin_to_db(std::sqrt(e)) > -12.0f,
              "control: sin ponderar por duración el bus daría > -12 dB");
    }

    // -- La ganancia ya autorada cuenta ---------------------------------------
    {
        // El autor ya bajó los efectos a la mitad (-6 dB): el balance mira lo
        // que se va a oír, no lo que dice el archivo.
        std::vector<AudioBusSample> v = {
            sample(kMusic,  -18.0f, 60.0),
            sample(kEffects, -12.0f, 10.0, /*gain=*/ay_db_to_lin(-6.0f)),
        };
        const AudioBusBalance b = audio_bus_balance(v, kBuses);
        check(near(b.buses[kEffects].level_db, -18.0f, 0.2f),
              "el nivel del bus incluye la ganancia autorada del asset");
        check(!b.buses[kEffects].out_of_range,
              "con la corrección del autor puesta, ya no hay desbalance");
    }

    // -- Sin música, la referencia es el bus con más material -----------------
    {
        std::vector<AudioBusSample> v = {
            sample(kEffects, -10.0f, 30.0),
            sample(kVoices,   -20.0f,  5.0),
        };
        const AudioBusBalance b = audio_bus_balance(v, kBuses);
        check(b.has_reference && b.reference == kEffects,
              "sin música clasificada, manda el bus con más segundos");
        check(near(b.buses[kVoices].correction_db, 10.0f),
              "y las voces suben 10 dB hasta esa referencia");
    }

    // -- Lo no clasificado no define nada -------------------------------------
    {
        std::vector<AudioBusSample> v = {
            sample(kUnclassified,     -3.0f, 300.0),    // mucho material, sin Tipo
            sample(kMusic, -20.0f,  60.0),
            sample(kEffects,-19.0f,  10.0),
        };
        const AudioBusBalance b = audio_bus_balance(v, kBuses);
        check(b.reference == kMusic,
              "un bus sin clasificar NO puede ser la referencia, ni con 5 "
              "minutos de material");
        check(b.buses[kUnclassified].count == 1 && b.buses[kUnclassified].correction_db == 0.0f,
              "se mide para poder mostrarlo, pero no se corrige: mover junto "
              "material que no tiene nada que ver sería peor que no tocarlo");
    }

    // -- Un solo bus: no hay nada que balancear -------------------------------
    {
        std::vector<AudioBusSample> v = { sample(kMusic, -20.0f, 60.0) };
        const AudioBusBalance b = audio_bus_balance(v, kBuses);
        check(!b.comparable,
              "con un solo bus con material, «comparable» es false — que es "
              "distinto de «está balanceado»");
        check(b.has_reference, "…pero el nivel del bus se mide igual");
    }

    // -- Nada medido ----------------------------------------------------------
    {
        const AudioBusBalance b = audio_bus_balance({}, kBuses);
        check(!b.has_reference && !b.comparable &&
              b.buses.size() == kBuses && b.buses[kMusic].count == 0,
              "sin assets medidos no se inventa una referencia");
    }

    // -- La corrección está acotada -------------------------------------------
    {
        // 40 dB de diferencia no es un balance: es material equivocado.
        // Ofrecer la corrección entera lo taparía.
        std::vector<AudioBusSample> v = {
            sample(kMusic,  -50.0f, 60.0),
            sample(kEffects, -10.0f, 10.0),
        };
        const AudioBusBalance b = audio_bus_balance(v, kBuses);
        check(b.buses[kEffects].out_of_range, "40 dB: se avisa");
        check(near(b.buses[kEffects].correction_db, -12.0f),
              "y la corrección se acota a 12 dB — el resto es un problema del "
              "material, no del balance");
    }

    // -- Un asset vacío no vota ----------------------------------------------
    {
        std::vector<AudioBusSample> v = {
            sample(kMusic, -20.0f, 60.0),
            AudioBusSample{ kEffects, 0.0f, 0.0, 1.0f },   // silencio, 0 s
        };
        const AudioBusBalance b = audio_bus_balance(v, kBuses);
        check(b.buses[kEffects].count == 0 && !b.comparable,
              "un asset sin duración ni nivel no cuenta como material medido");
    }

    std::printf("== %d OK, %d FAIL ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
