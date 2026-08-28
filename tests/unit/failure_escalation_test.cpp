// ---------------------------------------------------------------------------
// failure_escalation_test.cpp — #301: cuándo dejar de intentar.
//
// El check que justifica el archivo: **se cuentan assets DISTINTOS, no
// ocurrencias**. Un archivo roto que suena mil veces es un problema; doce
// archivos distintos es un pack mal armado. Contar ocurrencias apagaría el
// subsistema por un solo asset que se repite mucho — que es justo el caso que
// no hay que castigar, porque el fallback de #388 ya lo resuelve bien.
//
// Header-only: sin sesión, sin audio y sin ROM.
// ---------------------------------------------------------------------------
#include "failure_escalation.h"

#include <cstdio>
#include <string>

namespace {

int g_pass = 0, g_fail = 0;
void check(bool ok, const char* what) {
    if (ok) ++g_pass; else ++g_fail;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

constexpr uint32_t kMusic  = 5;   // Subsystem::Music
constexpr uint32_t kEffects = 6;   // Subsystem::Sfx

}  // namespace

int main() {
    using ayther::FailureEscalation;
    std::printf("== failure_escalation_test (#301) ==\n");

    // -- 1. El mismo asset mil veces no escala ------------------------------
    std::printf("\n[1] un archivo roto que suena mil veces es UN problema\n");
    {
        FailureEscalation e(3);
        bool escalated = false;
        for (int i = 0; i < 1000; ++i)
            if (e.note(kMusic, "roto.ogg")) escalated = true;
        check(!escalated, "mil ocurrencias del MISMO asset no apagan nada");
        check(e.count(kMusic) == 1, "…y cuenta uno, no mil");
    }

    // -- 2. Assets distintos sí --------------------------------------------
    std::printf("\n[2] varios archivos distintos SI escalan\n");
    {
        FailureEscalation e(3);
        check(!e.note(kMusic, "a.ogg"), "el primero no");
        check(!e.note(kMusic, "b.ogg"), "el segundo tampoco");
        check(e.note(kMusic, "c.ogg"),  "el terzero_threshold cruza el umbral");
        check(e.count(kMusic) == 3, "y son tres");
    }

    // -- 3. Se avisa UNA vez ------------------------------------------------
    std::printf("\n[3] cruzar el umbral se avisa una sola vez\n");
    {
        // Si devolviera true siempre, el llamador apagaria un subsistema ya
        // apagado en cada frame y el log creceria sin decir nada nuevo.
        FailureEscalation e(2);
        e.note(kMusic, "a.ogg");
        check(e.note(kMusic, "b.ogg"), "cruza");
        check(!e.note(kMusic, "c.ogg"), "el siguiente ya NO vuelve a avisar");
        check(!e.note(kMusic, "a.ogg"), "…ni un repetido");
        check(e.count(kMusic) == 3, "pero se sigue contando");
    }

    // -- 4. Por subsistema ---------------------------------------------------
    std::printf("\n[4] que falte la musica no dice nada de los efectos\n");
    {
        // Apagar los dos por uno seria llevarse puesto lo que si funciona.
        FailureEscalation e(2);
        e.note(kMusic, "a.ogg");
        check(!e.note(kEffects, "b.wav"),
              "un fallo en efectos no suma al contador de musica");
        check(e.note(kMusic, "c.ogg"), "musica cruza con los suyos");
        check(e.count(kEffects) == 1, "y efectos sigue en uno");
        check(e.total() == 3, "el total suma los dos, para el mensaje");
    }

    // -- 5. Entradas que no cuentan -----------------------------------------
    std::printf("\n[5] lo que no es un asset no cuenta\n");
    {
        FailureEscalation e(2);
        check(!e.note(kMusic, ""), "un asset vacio no cuenta");
        check(e.count(kMusic) == 0, "…ni se registra");
        // Un umbral de 0 seria «apagar al primer fallo», que en la practica es
        // apagar siempre. Se sube a 1 en vez de dividir por zero_threshold o no escalar
        // nunca: las dos alternativas son sorpresas.
        FailureEscalation zero_threshold(0);
        check(zero_threshold.threshold() >= 1, "un umbral de 0 se sube a 1");
        check(zero_threshold.note(kMusic, "a.ogg"), "…y escala al primero");
    }

    // -- 6. Reintentar --------------------------------------------------------
    std::printf("\n[6] limpiar deja volver a intentar\n");
    {
        // El motor no reintenta solo —volver a probar lo mismo que ya fallo es
        // lo que la escalada existe para no hacer— pero el usuario si puede
        // pedirlo, y ahi el contador tiene que arrancar de zero_threshold: si no, el
        // primer fallo posterior volveria a apagar y pareceria que el boton no
        // funciona.
        FailureEscalation e(2);
        e.note(kMusic, "a.ogg");
        e.note(kMusic, "b.ogg");
        e.clear();
        check(e.count(kMusic) == 0 && e.total() == 0, "se olvida todo");
        check(!e.note(kMusic, "a.ogg"), "y el mismo asset vuelve a contar desde zero_threshold");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
