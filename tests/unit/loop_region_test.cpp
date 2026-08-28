// ---------------------------------------------------------------------------
// loop_region_test.cpp — #304: regiones de loop dentro del archivo.
//
// Repetir el asset entero y repetir una REGIÓN son cosas distintas: lo primero
// sirve para un efecto, lo segundo es cómo se hace que un tema tenga intro y
// después cicle. Sin puntos in/out eso no se puede autorar — hay que exportar
// dos archivos.
//
// El mixer es header-only y no toca SDL, así que esto corre sin device: se le
// da un PCM cuyas muestras dicen su propio índice y se lee qué salió.
// ---------------------------------------------------------------------------
#include "audio_hd_mixer.h"

#include <cstdio>
#include <memory>
#include <vector>

namespace {

int g_pass = 0, g_fail = 0;
void check(bool ok, const char* what) {
    if (ok) ++g_pass; else ++g_fail;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

/// Un PCM estéreo de `n` cuadros donde la muestra del cuadro i vale i+1. Así,
/// leyendo la salida se sabe exactamente qué cuadro del asset sonó.
HdMixPcm ramp(int n) {
    auto v = std::make_shared<std::vector<int16_t>>();
    v->resize(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n; ++i) {
        (*v)[i * 2 + 0] = static_cast<int16_t>(i + 1);
        (*v)[i * 2 + 1] = static_cast<int16_t>(i + 1);
    }
    return v;
}

/// Mezcla `frames` cuadros y devuelve el canal izquierdo, que con `ramp()` es
/// la lista de cuadros del asset que se oyeron, en orden.
std::vector<int> play(HdMixer& m, int frames) {
    std::vector<int16_t> out(static_cast<size_t>(frames) * 2, 0);
    m.mix_into(out.data(), static_cast<size_t>(frames), 0);
    std::vector<int> got;
    got.reserve(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) got.push_back(out[i * 2]);
    return got;
}

}  // namespace

int main() {
    std::printf("== loop_region_test (#304) ==\n");

    // -- 1. Sin región: el asset entero, como siempre -----------------------
    std::printf("\n[1] sin region se repite el asset entero\n");
    {
        HdMixer m;
        m.start(1, ramp(4), 0, 0, 1.0f, /*looping=*/true, false,
                UINT64_MAX, UINT64_MAX);
        const auto got = play(m, 10);
        // 1 2 3 4 1 2 3 4 1 2
        check(got[0] == 1 && got[3] == 4 && got[4] == 1,
              "vuelve al principio del archivo");
        check(got[8] == 1 && got[9] == 2, "…y sigue ciclando");
    }

    // -- 2. Con región: intro + bucle ---------------------------------------
    std::printf("\n[2] con region hay intro y despues cicla\n");
    {
        HdMixer m;
        // Asset de 6 cuadros; el bucle es [2,5) → cuadros 3,4,5 del archivo.
        m.start(1, ramp(6), 0, 0, 1.0f, true, false,
                UINT64_MAX, UINT64_MAX, 0, /*loop_begin=*/2, /*loop_end=*/5);
        const auto got = play(m, 11);
        // intro: 1 2 · bucle: 3 4 5 · 3 4 5 · 3 4 5
        check(got[0] == 1 && got[1] == 2, "la INTRO suena una sola vez");
        check(got[2] == 3 && got[3] == 4 && got[4] == 5, "y despues la region");
        check(got[5] == 3 && got[6] == 4 && got[7] == 5,
              "que vuelve al INICIO DE LA REGION, no al del archivo");
        check(got[8] == 3, "…una y otra vez");
        // Lo que NUNCA tiene que volver a sonar es la intro.
        bool intro_repetida = false;
        for (size_t i = 2; i < got.size(); ++i)
            if (got[i] == 1 || got[i] == 2) intro_repetida = true;
        check(!intro_repetida, "la intro no vuelve a sonar NUNCA");
        // Y la cola posterior al loop tampoco: el cuadro 6 queda afuera.
        bool cola = false;
        for (const int v : got) if (v == 6) cola = true;
        check(!cola, "lo que esta despues del loop_end no suena");
    }

    // -- 3. La region no aplica a un one-shot -------------------------------
    std::printf("\n[3] una region en un one-shot no lo recorta\n");
    {
        HdMixer m;
        m.start(1, ramp(6), 0, 0, 1.0f, /*looping=*/false, false,
                UINT64_MAX, UINT64_MAX, 0, 2, 5);
        const auto got = play(m, 8);
        // Un one-shot suena entero: recortarlo por una region seria cortar el
        // sonido a la mitad, y la region no querria decir nada ahi.
        check(got[0] == 1 && got[5] == 6, "suena el asset COMPLETO");
        check(got[6] == 0 && got[7] == 0, "…y despues silencio, sin repetir");
    }

    // -- 4. Regiones imposibles ---------------------------------------------
    std::printf("\n[4] una region invalida cae al asset entero\n");
    {
        // Invertida, fuera del asset, o vacia: se ignora en vez de leer fuera
        // del buffer. Se sanea al ARRANCAR y no al mezclar: comprobarlo por
        // muestra seria pagar el chequeo 44 100 veces por segundo por algo que
        // no cambia en toda la vida de la voz.
        {
            HdMixer m;
            m.start(1, ramp(4), 0, 0, 1.0f, true, false, UINT64_MAX, UINT64_MAX,
                    0, /*begin=*/3, /*end=*/1);
            const auto got = play(m, 6);
            check(got[0] == 1 && got[4] == 1, "invertida: se ignora");
        }
        {
            HdMixer m;
            m.start(1, ramp(4), 0, 0, 1.0f, true, false, UINT64_MAX, UINT64_MAX,
                    0, /*begin=*/1, /*end=*/99);
            const auto got = play(m, 8);
            // end se recorta al largo del asset: el bucle es [1,4).
            check(got[0] == 1 && got[3] == 4 && got[4] == 2,
                  "un end mas alla del asset se recorta a su largo");
        }
    }

    // -- 5. Reanudar dentro de la region (#385) ------------------------------
    std::printf("\n[5] reanudar entra por la fase DE LA REGION\n");
    {
        HdMixer m;
        // Asset de 6, bucle [2,5). Reanudar en el cuadro 8 (ya pasado el
        // final): tiene que entrar dentro del bucle, no volver a la intro.
        m.start(1, ramp(6), 0, /*offset=*/8, 1.0f, true, false,
                UINT64_MAX, UINT64_MAX, 0, 2, 5);
        const auto got = play(m, 4);
        check(got[0] != 1 && got[0] != 2,
              "no vuelve a la intro al reanudar pasado el final");
        check(got[0] >= 3 && got[0] <= 5, "…entra dentro de la region");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
