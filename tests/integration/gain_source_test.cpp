// ---------------------------------------------------------------------------
// gain_source_test.cpp — #417: bajar un bus alcanza al audio ORIGINAL.
//
// Hasta acá, el volumen de un bus escalaba la ganancia del stream HD. El audio
// del juego pasa por el router de voces (#327), y ahí una voz sólo podía ser
// COPIADA o CALLADA — no había nada en el medio. Resultado: la música del juego
// o sonaba entera o se callaba, y el slider de volumen no la tocaba.
//
// `GainSource` es ese medio. Lo que este test fija es la trampa que el repo ya
// pagó dos veces:
//
//   EL FACTOR SE LEE POR RENDER, NO SE COPIA AL ARRANCAR.
//
// En un loop musical «al arrancar» es NUNCA: el usuario mueve el slider y no
// pasa nada hasta la próxima vez que la música empiece de cero, que puede ser
// jamás.
// ---------------------------------------------------------------------------
#include "voice_router.h"

#include <cstdio>
#include <vector>

namespace {

int g_pass = 0, g_fail = 0;
void check(bool ok, const char* what) {
    if (ok) ++g_pass; else ++g_fail;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

/// Un bloque estéreo donde cada muestra vale 1.
std::vector<float> ones(size_t frames) {
    return std::vector<float>(frames * 2, 1.0f);
}

}  // namespace

int main() {
    using namespace ayther;
    std::printf("== gain_source_test (#417) ==\n");

    // -- 1. Escala ------------------------------------------------------------
    std::printf("\n[1] la copia, con un factor\n");
    {
        float g = 0.5f;
        GainSource src(&g);
        const auto in = ones(4);
        std::vector<float> out(4 * 2, 0.0f);
        src.render(in.data(), out.data(), 4);
        bool all_scaled = true;
        for (const float v : out) if (v < 0.49f || v > 0.51f) all_scaled = false;
        check(all_scaled, "0,5 baja a la mitad, en TODAS las muestras del bloque");

        // SUMA sobre lo que ya hay: el mixer acumula voces y una fuente que
        // pisara el buffer se llevaría puestas las demás.
        std::vector<float> acc(4 * 2, 1.0f);
        src.render(in.data(), acc.data(), 4);
        check(acc[0] > 1.49f && acc[0] < 1.51f, "y SUMA, no pisa");
    }

    // -- 2. La trampa que motiva el test -------------------------------------
    std::printf("\n[2] el factor se lee POR RENDER\n");
    {
        float g = 1.0f;
        GainSource src(&g);
        VoiceContext ctx{};
        src.begin(ctx);          // la voz arranca con el volumen al máximo…
        g = 0.25f;               // …y el usuario mueve el slider con ella sonando
        const auto in = ones(2);
        std::vector<float> out(2 * 2, 0.0f);
        src.render(in.data(), out.data(), 2);
        // Si el factor se copiara en begin(), acá saldría 1.0 y el slider no
        // haría nada hasta el próximo key-on — que en un loop puede no llegar.
        check(out[0] > 0.24f && out[0] < 0.26f,
              "mover el volumen con la voz SONANDO se oye ahora");

        g = 2.0f;
        std::vector<float> out2(2 * 2, 0.0f);
        src.render(in.data(), out2.data(), 2);
        check(out2[0] > 1.99f, "…y subirlo tambien, sin re-arrancar la voz");
    }

    // -- 3. El caso normal no paga -------------------------------------------
    std::printf("\n[3] con el volumen en 1 el resultado es la copia\n");
    {
        float g = 1.0f;
        GainSource src(&g);
        CopySource copy;
        const auto in = ones(8);
        std::vector<float> a(8 * 2, 0.0f), b(8 * 2, 0.0f);
        src.render(in.data(), a.data(), 8);
        copy.render(in.data(), b.data(), 8);
        check(a == b, "byte a byte igual que CopySource");
    }

    // -- 4. Extremos ----------------------------------------------------------
    std::printf("\n[4] los extremos\n");
    {
        float g = 0.0f;
        GainSource src(&g);
        const auto in = ones(4);
        std::vector<float> out(4 * 2, 0.0f);
        src.render(in.data(), out.data(), 4);
        bool muted = true;
        for (const float v : out) if (v != 0.0f) muted = false;
        check(muted, "volumen 0 no suena");

        // Sin puntero de ganancia se copia tal cual: la degradacion es «se oye
        // el original», que es la misma de todo el router — nunca silencio.
        GainSource huerfano(nullptr);
        std::vector<float> o2(4 * 2, 0.0f);
        huerfano.render(in.data(), o2.data(), 4);
        check(o2[0] == 1.0f, "sin factor, copia tal cual (nunca silencio)");

        // Y punteros nulos no revientan: el router llama a render en caminos
        // donde el canal puede no existir todavia.
        float g2 = 1.0f;
        GainSource s2(&g2);
        s2.render(nullptr, o2.data(), 4);
        s2.render(in.data(), nullptr, 4);
        check(true, "punteros nulos no revientan");
    }

    // -- 5. El contrato de la fuente -----------------------------------------
    std::printf("\n[5] es una copia: no termina sola\n");
    {
        float g = 1.0f;
        GainSource src(&g);
        VoiceContext ctx{};
        check(src.begin(ctx), "begin siempre puede");
        src.key_off();
        // Una copia nunca termina —el espejo ya modela su propio release— asi
        // que el slot se libera recien en el proximo key-on. Si dijera que
        // termino, el router la sacaria y el original dejaria de oirse.
        check(!src.finished(), "y no termina tras el key-off");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
