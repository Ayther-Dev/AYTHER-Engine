// ---------------------------------------------------------------------------
// video_bt601_test — la fórmula BT.601 que el shader del video tiene que dar
// (#378).
//
// POR QUÉ EXISTE. La conversión I420→RGB se fue de la CPU al fragment shader:
// era el 62% del costo por frame. Un shader que convierta MAL no rompe nada
// visible — el video se ve, con los colores corridos — así que el modo de falla
// es que nadie se entere hasta mirarlo con atención. Este test fija la
// referencia ejecutable (`ayther::video_i420_to_bgra_px`) contra la que
// `runtime/src/vulkan_backend/shaders/video.frag` está escrito línea por línea.
//
// LO QUE ESTE TEST **NO** HACE, y hay que decirlo: no ejecuta el shader. Fija la
// especificación —los valores que la GPU tiene que producir— para que la
// comparación contra el readback sea contra números y no contra otra
// implementación de lo mismo. Los casos de acá son los que el shader tiene que
// reproducir; la corrida en GPU es trabajo aparte.
//
// LAS TRAMPAS QUE CUBRE:
//
//  · `>> 8` sobre NEGATIVO. En C++ es un corrimiento aritmético; en GLSL el
//    corrimiento de un signed negativo es implementation-defined (§5.9). El
//    shader usa `floor(x/256.0)` para no depender de eso, y los casos de abajo
//    incluyen los que producen numerador negativo — que no son raros: cualquier
//    píxel oscuro con croma bajo lo consigue.
//  · El CLAMP. Los extremos de YUV salen fuera de [0,255] en RGB, y saturar mal
//    se ve como halos en las zonas quemadas.
//  · El ORDEN de los canales. La referencia entrega BGRA; invertir R y B es el
//    error clásico de esta conversión y da una imagen perfectamente nítida con
//    los colores dados vuelta.
// ---------------------------------------------------------------------------
#include "ayther_video.h"

#include <cstdio>
#include <string>

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}

/// La misma cuenta, escrita a mano con doubles y redondeo hacia abajo. Existe
/// para que el test no sea «la función comparada consigo misma»: si la
/// referencia se tocara, esto no la sigue.
void reference(int Y, int U, int V, int out[3]) {
    const double C = Y - 16, D = U - 128, E = V - 128;
    auto fl = [](double v) { return int(v < 0 ? -int(-v + 0.9999999) : int(v)); };
    auto div256_floor = [&](double num) {
        double q = num / 256.0;
        int    i = int(q);
        if (q < 0 && double(i) != q) --i;   // floor, no truncamiento
        (void)fl;
        return i;
    };
    auto cl = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
    out[0] = cl(div256_floor(298 * C + 409 * E + 128));          // R
    out[1] = cl(div256_floor(298 * C - 100 * D - 208 * E + 128)); // G
    out[2] = cl(div256_floor(298 * C + 516 * D + 128));          // B
}

}  // namespace

int main() {
    std::printf("=== video_bt601_test — la formula que el shader debe dar (#378) ===\n");

    // ---- Barrido completo contra la referencia independiente ---------------
    // Todo el espacio seria 16,7 millones de combinaciones; se barre con paso 5,
    // que son ~130 mil y cubren igual los bordes (0 y 255 entran por el paso).
    int worst = 0, worst_y = 0, worst_u = 0, worst_v = 0;
    for (int Y = 0; Y <= 255; Y += 5)
      for (int U = 0; U <= 255; U += 5)
        for (int V = 0; V <= 255; V += 5) {
            uint8_t bgra[4];
            ayther::video_i420_to_bgra_px(Y, U, V, bgra);
            int ref[3];
            reference(Y, U, V, ref);
            const int dr = bgra[2] - ref[0];
            const int dg = bgra[1] - ref[1];
            const int db = bgra[0] - ref[2];
            const int d = (dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db);
            if (d > worst) { worst = d; worst_y = Y; worst_u = U; worst_v = V; }
        }
    check(worst == 0, "la referencia coincide EXACTAMENTE con la cuenta independiente");
    if (worst) std::printf("       peor caso: Y=%d U=%d V=%d (delta %d)\n",
                           worst_y, worst_u, worst_v, worst);

    // ---- Casos con nombre, que son los que el shader tiene que reproducir ---
    struct Case { int y, u, v; int b, g, r; const char* what; };
    const Case cases[] = {
        // Negro de limited range: Y=16 es el piso, no el 0.
        {  16, 128, 128,   0,   0,   0, "negro (Y=16 limited range)" },
        // Blanco: Y=235 es el techo; 255 se pasa y satura.
        { 235, 128, 128, 255, 255, 255, "blanco (Y=235)" },
        // Gris medio: C=110 -> (298*110 + 128) >> 8 = 32908 >> 8 = 128.
        { 126, 128, 128, 128, 128, 128, "gris medio" },
        // NUMERADOR NEGATIVO en R y en B — el caso que en GLSL no se puede
        // resolver con `>>` sobre signed, y que el shader hace con floor():
        //   C=-16 D=-128 E=-128
        //   R: -4768 - 52352 + 128 = -56992 -> floor(-222,6) = -223 -> clamp 0
        //   G: -4768 + 12800 + 26624 + 128 = 34784 -> 135
        //   B: -4768 - 66048 + 128 = -70688 -> negativo -> clamp 0
        // Si el corrimiento truncara hacia cero en vez de hacia abajo, R y B
        // seguirian dando 0 por el clamp: por eso el que delata es G.
        {   0,   0,   0,   0, 135,   0, "Y bajo el piso + croma al minimo (numerador negativo)" },
        // Saturacion por arriba: R y B se pasan de 255, G no.
        //   C=239 D=127 E=127
        //   R: 71222 + 51943 + 128 = 123293 -> 481 -> clamp 255
        //   G: 71222 - 12700 - 26416 + 128 = 32234 -> 125
        //   B: 71222 + 65532 + 128 = 136882 -> 534 -> clamp 255
        { 255, 255, 255, 255, 125, 255, "extremo alto (satura el clamp en R y B, no en G)" },
    };
    for (const Case& c : cases) {
        uint8_t px[4];
        ayther::video_i420_to_bgra_px(c.y, c.u, c.v, px);
        const bool ok = px[0] == c.b && px[1] == c.g && px[2] == c.r && px[3] == 255;
        check(ok, std::string(c.what));
        if (!ok)
            std::printf("       Y=%d U=%d V=%d -> B=%u G=%u R=%u (esperado %d %d %d)\n",
                        c.y, c.u, c.v, px[0], px[1], px[2], c.b, c.g, c.r);
    }

    // ---- El orden de canales, explicito ------------------------------------
    // Un rojo puro tiene que salir con R alto y B bajo. Si estuvieran invertidos
    // todo lo demas de este test seguiria pasando: la conversion es simetrica en
    // el barrido, pero la IMAGEN saldria con los colores dados vuelta.
    {
        uint8_t px[4];
        ayther::video_i420_to_bgra_px(81, 90, 240, px);   // rojo BT.601
        check(px[2] > 200 && px[0] < 60,
              "el orden es BGRA: un rojo sale con R alto y B bajo");
        ayther::video_i420_to_bgra_px(41, 240, 110, px);  // azul BT.601
        check(px[0] > 200 && px[2] < 60,
              "y un azul, al reves");
    }

    // ---- Opacidad -----------------------------------------------------------
    {
        uint8_t px[4];
        ayther::video_i420_to_bgra_px(128, 128, 128, px);
        check(px[3] == 255, "el video es OPACO: alpha 255 siempre");
    }

    if (g_fail) {
        std::printf("=== %d FALLA(S) ===\n", g_fail);
        return 1;
    }
    std::printf("=== ok ===\n");
    return 0;
}
