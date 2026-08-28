// ---------------------------------------------------------------------------
// parallax_bands_test.cpp — #231 EM-8.0: la columna de nivel POR BANDA.
//
// Con una cámara única por plano, las bandas de parallax del plano B colapsan
// en las mismas columnas. MEDIDO en Sonic 2: el plano A reconstruía 607 columnas
// de nivel y el B sólo 37 —menos de una pantalla— con 45 bandas por frame.
//
// Este test corre SIN ROM, y las dos veces que valió la pena fue por eso:
//
//   * La primera versión de la regla vivía adentro del bucle de
//     `ayther_session.cpp`, donde ni el oráculo del stitcher la veía. Medida
//     contra la ROM no movió un solo número, y el motivo no era que estuviera
//     mal: no se estaba ejecutando.
//
//   * La segunda restaba los H de dos bandas del mismo frame. Contra la ROM
//     «funcionaba» —el plano B pasaba de 37 a 165 columnas— y fue este test el
//     que mostró que -64 en un campo de 10 bits es 960, y que la separación
//     entre bandas crece más allá de una vuelta. El número lindo estaba mal.
// ---------------------------------------------------------------------------
#include "parallax_bands.h"

#include <cstdio>
#include <map>
#include <vector>

namespace {

int g_pass = 0, g_fail = 0;
void check(bool ok, const char* what) {
    if (ok) ++g_pass; else ++g_fail;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

/// Una tabla Hscroll de mentira: dirección → palabra larga (A en la palabra
/// baja, B en la alta), como la entrega la VRAM ya des-swapeada.
struct FakeTable {
    std::map<uint32_t, uint32_t> w;
    uint32_t operator()(uint32_t a) const {
        const auto it = w.find(a);
        return it == w.end() ? 0u : it->second;
    }
    /// Escribe la entrada de la LÍNEA `line` (el índice ya enmascarado por quien
    /// llama, igual que hace el VDP).
    void line(uint32_t base, int line, int hA, int hB) {
        w[base + (uint32_t)line * 4] =
            ((uint32_t)(hB & 0x3FF) << 16) | (uint32_t)(hA & 0x3FF);
    }
};

constexpr uint32_t kBase = 0xFC00;
constexpr int      kW    = 512;   // ancho de plano de Sonic 2 (64 celdas)
constexpr int      kRows = 28;

}  // namespace

int main() {
    using namespace ayther;
    std::printf("== parallax_bands_test (#231 EM-8.0) ==\n");

    // -- 1. Los modos de la tabla --------------------------------------------
    std::printf("\n[1] el registro $0B dice cómo está organizada la tabla\n");
    {
        check(hscroll_mask(0) == 0x00, "modo 0: un solo scroll para todo el plano");
        check(hscroll_mask(1) == 0x07, "modo 1: por celda dentro del primer tile");
        check(hscroll_mask(2) == 0xF8, "modo 2: por celda (cada 8 líneas)");
        check(hscroll_mask(3) == 0xFF, "modo 3: por línea");
        // Los bits altos del registro son otra cosa; mirarlos elige otro modo.
        check(hscroll_mask(0xFC | 3) == 0xFF, "los bits altos del registro no participan");
        check(hscroll_base(0x3F) == 0xFC00, "la base sale del $0D, alineada a 1 KB");
        check(hscroll_base(0x00) == 0x0000, "…y puede ser 0");
    }

    // -- 2. El campo del VDP es de 10 bits -----------------------------------
    std::printf("\n[2] lo que el VDP ignora, la regla también\n");
    {
        FakeTable t;
        // El juego escribe 16 bits; el VDP usa 10. Enmascarar en el llamador y
        // no acá es como se cuela un valor de 16 bits en un cálculo de 10.
        t.w[kBase] = (0xF040u << 16) | 0xF041u;
        check(hscroll_of_line(t, kBase, 0xFF, 1, 0) == 0x040, "plano B: 10 bits");
        check(hscroll_of_line(t, kBase, 0xFF, 0, 0) == 0x041, "plano A: su propia mitad");
    }

    // -- 3. Sin parallax por bandas no hay nada que separar -------------------
    std::printf("\n[3] modo 0: una sola banda, offset 0\n");
    {
        FakeTable t;
        t.line(0, 0, 0, 0);
        // Aunque la tabla tuviera basura más abajo, en modo 0 el VDP no la lee.
        t.line(0, 8, 999, 999);
        check(band_count(t, 0, hscroll_mask(0), 1, kRows) == 1, "una sola banda");

        BandCameras cam;
        cam.configure(kRows, kW);
        cam.observe(t, 0, hscroll_mask(0), 1);
        check(cam.offset_from_top(20) == 0, "todas las filas comparten columna");
    }

    // -- 4. El caso que motiva todo ------------------------------------------
    std::printf("\n[4] bandas a distintos H → columnas distintas\n");
    {
        FakeTable t;
        // Banda 0 (filas 0-9) sin correr; banda 1 (10-19) 64 px; banda 2, 128.
        for (int r = 0; r < kRows; ++r) {
            const int h = (r < 10) ? 0 : (r < 20 ? 64 : 128);
            t.line(kBase, r * 8, 0, h);
        }
        check(band_count(t, kBase, 0xFF, 1, kRows) == 3, "cuenta las 3 bandas");

        BandCameras cam;
        cam.configure(kRows, kW);
        cam.observe(t, kBase, 0xFF, 1);
        check(cam.offset_from_top(0)  == 0,   "la banda de arriba es el origen");
        check(cam.offset_from_top(12) == -8,  "una banda corrida 64 px → 8 columnas");
        check(cam.offset_from_top(25) == -16, "y una a 128 px → 16 columnas");

        // El plano A comparte la tabla y acá no se mueve: leer la palabra
        // equivocada arrastraría el parallax de B al plano de juego.
        BandCameras camA;
        camA.configure(kRows, kW);
        camA.observe(t, kBase, 0xFF, 0);
        check(camA.offset_from_top(25) == 0, "el plano A lee su propia mitad");
    }

    // -- 5. La vuelta: lo que la resta cruda no puede saber -------------------
    std::printf("\n[5] la banda da la vuelta al plano y la cuenta sigue\n");
    {
        FakeTable t;
        BandCameras cam;
        cam.configure(kRows, kW);
        // Una banda que avanza 32 px por frame durante 40 frames: 1280 px, o sea
        // DOS vueltas y media de un plano de 512. El campo del VDP habrá
        // envuelto dos veces.
        for (int f = 0; f < 40; ++f) {
            const int h = (f * 32) & 0x3FF;
            for (int r = 0; r < kRows; ++r) t.line(kBase, r * 8, 0, r < 14 ? 0 : h);
            cam.observe(t, kBase, 0xFF, 1);
        }
        // 39 frames × 32 px = 1248 px = 156 columnas. Restar los H del último
        // frame daría 1248 & 0x3FF = 224 px = 28 columnas: el arte de 156
        // columnas apretado en 28, que es exactamente el bug original.
        check(cam.offset_from_top(20) == -156, "156 columnas, no 28: las vueltas se cuentan");
        check(cam.offset_from_top(2)  == 0,    "y la banda quieta no se movió");
    }

    // -- 6. La siembra y el reinicio -----------------------------------------
    std::printf("\n[6] el arranque\n");
    {
        FakeTable t;
        for (int r = 0; r < kRows; ++r) t.line(kBase, r * 8, 0, r < 14 ? 0 : 96);

        BandCameras cam;
        cam.configure(kRows, kW);
        // Sin ningún frame consumido todo vale 0: el llamador que pregunte antes
        // de tiempo obtiene la cámara única de siempre, no un valor inventado.
        check(cam.offset_from_top(20) == 0, "antes del primer frame, offset 0");
        cam.observe(t, kBase, 0xFF, 1);
        check(cam.offset_from_top(20) == -12, "el primer frame SIEMBRA la separación real");

        // Reconfigurar a otro plano vuelve a sembrar: si arrastrara el absoluto
        // viejo, el primer frame del nivel nuevo saldría corrido cientos de
        // columnas y el stitcher escribiría en el vacío.
        cam.configure(kRows, 1024);
        check(cam.offset_from_top(20) == 0, "otro tamaño de plano reinicia");
        check(cam.rows() == kRows, "…sin perder las filas");
    }

    // -- 7. Alimentarlo todos los frames -------------------------------------
    std::printf("\n[7] el des-enrollado pide TODOS los frames\n");
    {
        FakeTable t;
        BandCameras cam;
        cam.configure(kRows, kW);
        // 8 px por frame es scroll normal y se cuenta entero.
        for (int f = 0; f < 20; ++f) {
            for (int r = 0; r < kRows; ++r) t.line(kBase, r * 8, 0, (f * 8) & 0x3FF);
            cam.observe(t, kBase, 0xFF, 1);
        }
        check(cam.absolute_px(0) == -152, "19 frames × 8 px, sin vueltas de por medio");
    }

    // -- 8. Los bordes -------------------------------------------------------
    std::printf("\n[8] los bordes\n");
    {
        FakeTable t;
        t.line(kBase, 0, 0, 0);
        BandCameras cam;
        cam.configure(kRows, kW);
        cam.observe(t, kBase, 0xFF, 1);
        check(cam.column(-3) == 0,     "una fila negativa no lee fuera del vector");
        check(cam.column(999) == 0,    "ni una fila de más");
        check(wrap_px(-1, 512) == 511, "el resto es siempre positivo");
        check(wrap_px(-513, 512) == 511, "…también más allá de una vuelta");

        // Una tabla ilegible da 0 en todo: el fallback es la cámara única, que
        // es el comportamiento anterior — nunca un salto.
        FakeTable empty_table;
        BandCameras c2;
        c2.configure(kRows, kW);
        c2.observe(empty_table, kBase, 0xFF, 1);
        check(c2.offset_from_top(20) == 0, "tabla ilegible → cámara única");

        // Y configurar 0 filas no revienta al preguntar.
        BandCameras c3;
        c3.configure(0, kW);
        c3.observe(t, kBase, 0xFF, 1);
        check(c3.column(0) == 0, "sin filas configuradas tampoco revienta");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
