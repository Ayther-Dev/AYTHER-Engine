// ---------------------------------------------------------------------------
// widescreen_test.cpp — #231 EM-8.1: el plan del área extendida.
//
// Sin GPU y sin ROM. La lección de EM-8.0 fue que una regla que sólo se puede
// ejercitar contra una ROM en marcha es una regla que casi nadie ejercita: la
// versión rota del cálculo por bands «funcionaba» contra Sonic 2 y la volteó un
// test de mesa.
//
// Lo que se fija acá es lo que la medición de la issue ya dejó claro:
//
//   * el área extendida se full_sheet desde la LÁMINA de nivel, no desde la nametable
//     viva (medido: 182 celdas rancias contra 0);
//   * en el plano B la columna depende de la FILA, porque cada banda de parallax
//     resuelve la suya;
//   * y la cobertura se reporta aunque falte arte — es el número que decide si
//     el ensanchado se puede mostrar.
// ---------------------------------------------------------------------------
#include "widescreen.h"

#include <cstdio>
#include <set>
#include <utility>
#include <vector>

namespace {

int g_pass = 0, g_fail = 0;
void check(bool ok, const char* what) {
    if (ok) ++g_pass; else ++g_fail;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
}

/// El plano A: sin parallax por bands, todas las filas comparten columna.
auto no_bands = [](int) { return 0; };

}  // namespace

int main() {
    using namespace ayther;
    std::printf("== widescreen_test (#231 EM-8.1) ==\n");

    // -- 1. Cuánto hay que ensanchar ----------------------------------------
    std::printf("\n[1] las dos anchuras de la issue\n");
    {
        // Píxel cuadrado: 224 × 16/9 = 398 px → 5 celdas por lado.
        const int w_sq = widescreen_target_width(224, 16, 9);
        check(w_sq == 398, "16:9 en píxel cuadrado sobre 224 px → 398");
        check(widescreen_cols_per_side(320, w_sq) == 5, "…que son 5 celdas por lado");

        // Preservando la relación del 4:3 MOSTRADO: el píxel no es cuadrado.
        // 320 px se ven como 4/3 × 224, o sea un píxel de 298.67/320.
        const int w_43 = widescreen_target_width(224, 16, 9, 224 * 4, 320 * 3);
        // El exacto es 426,67. La issue anotó 426 (truncado) y acá redondea al
        // más cercano, que queda a 0,33 px del ideal en vez de a 0,67. La
        // diferencia no cambia empty_coverage aguas abajo —son 7 celdas por lado en los
        // dos casos— pero el criterio conviene que esté escrito: si mañana
        // alguien compara este número con el de la issue, no es un bug.
        check(w_43 == 427, "16:9 preservando el 4:3 mostrado → 427 (426,67 exacto)");
        check(widescreen_cols_per_side(320, w_43) == 7, "…que son 7 celdas por lado");
    }

    // -- 2. Redondear para arriba --------------------------------------------
    std::printf("\n[2] se dibuja de MÁS y se recorta\n");
    {
        // 398-320 = 78 px, 39 por lado: 4 celdas dan 32 px y dejan una franja
        // negra de 7 px contra el marco, que se lee como un bug de render.
        check(widescreen_cols_per_side(320, 398) == 5, "39 px por lado → 5 celdas, no 4");
        check(widescreen_cols_per_side(320, 336) == 1, "8 px por lado → 1 celda exacta");
        check(widescreen_cols_per_side(320, 322) == 1, "1 px por lado → igual 1 celda");
        // Y un objetivo que no ensancha no pide empty_coverage. Sin esto un pack que
        // declare 4:3 pediría columnas laterales de cero ancho.
        check(widescreen_cols_per_side(320, 320) == 0, "mismo ancho → empty_coverage que agregar");
        check(widescreen_cols_per_side(320, 200) == 0, "más angosto tampoco");
    }

    // -- 3. El plan, sin bands ----------------------------------------------
    std::printf("\n[3] el plan de un plano sin parallax por bands\n");
    {
        std::vector<ExtCell> cells;
        widescreen_plan(2, 40, 28, 100, 10, no_bands,
                        [&](const ExtCell& e) { cells.push_back(e); });
        check((int)cells.size() == 2 * 2 * 28, "2 columnas por lado × 28 filas");

        std::set<std::pair<int, int>> dst;
        bool left_ok = true, right_ok = true, row_ok = true;
        for (const ExtCell& e : cells) {
            dst.insert({ e.dst_col, e.dst_row });
            if (e.dst_col < 0 && e.level_col != 100 + e.dst_col) left_ok = false;
            // La primera columna nueva de la derecha es la 40, no la 41: `+c`
            // en vez de `+c-1` dejaría un hueco de una celda contra la pantalla.
            if (e.dst_col >= 40 && e.level_col != 100 + e.dst_col) right_ok = false;
            if (e.level_row != 10 + e.dst_row) row_ok = false;
        }
        check(left_ok, "la izquierda son las columnas ANTERIORES a la cámara");
        check(right_ok, "y la derecha arranca pegada a la pantalla, sin hueco");
        check(row_ok, "la fila de nivel es la de cámara más la de pantalla");
        check((int)dst.size() == (int)cells.size(), "ninguna celda de destino repetida");

        bool inside = false;
        for (const ExtCell& e : cells)
            if (e.dst_col >= 0 && e.dst_col < 40) inside = true;
        check(!inside, "y ninguna cae DENTRO de la pantalla original");
    }

    // -- 4. El plan con bands -----------------------------------------------
    std::printf("\n[4] con parallax por bands la columna depende de la FILA\n");
    {
        // Dos bands: las filas de abajo van 6 columnas atrás.
        auto bands = [](int row) { return row < 14 ? 0 : -6; };
        std::vector<ExtCell> cells;
        widescreen_plan(1, 40, 28, 100, 0, bands,
                        [&](const ExtCell& e) { cells.push_back(e); });

        int top = -1, bottom = -1;
        for (const ExtCell& e : cells) {
            if (e.dst_col == -1 && e.dst_row == 0)  top = e.level_col;
            if (e.dst_col == -1 && e.dst_row == 20) bottom  = e.level_col;
        }
        check(top == 99, "la banda de arriba pide la columna 99");
        check(bottom == 93,  "la de abajo pide la 93, no la 99");
        // Es EL punto de EM-8.0 llevado al lateral: una sola columna por lado
        // dejaría huecos justo donde el parallax separa las bands, y el síntoma
        // —filas sueltas sin arte— no apunta a la causa.
        check(top != bottom, "una sola columna por lado no alcanza");
    }

    // -- 5. La cobertura -----------------------------------------------------
    std::printf("\n[5] la cobertura se REPORTA, no se esconde\n");
    {
        // Una lámina que sólo tiene las filas 14 en adelante: es exactamente la
        // forma de los huecos medidos —los 185 caen en las filas de arriba y
        // ninguno en la 14-31—, así que no falta arte a los costados: falta
        // ARRIBA, en filas que la ventana vertical nunca mostró en ese tramo.
        auto level_sheet = [](int, int row) { return row >= 14; };
        const Coverage cv = widescreen_coverage(5, 40, 28, 100, 0, no_bands, level_sheet);
        check(cv.total == 5 * 2 * 28, "cuenta TODAS las celdas pedidas");
        check(cv.filled == 5 * 2 * 14, "y sólo las que la lámina tiene");
        check(cv.missing() == 5 * 2 * 14, "el faltante es el número a vigilar");
        check(!cv.complete(), "con huecos, no está completa");

        auto full_sheet = [](int, int) { return true; };
        check(widescreen_coverage(5, 40, 28, 100, 0, no_bands, full_sheet).complete(),
              "una lámina que cubre todo, sí");

        // Sin celdas pedidas la cobertura es COMPLETA, no cero: no hay empty_coverage que
        // llenar. Contarlo como 0% haría que «no ensancho» se leyera como «me
        // faltó arte», y el gate de EM-8.2 apagaría por la razón equivocada.
        const Coverage empty_coverage = widescreen_coverage(0, 40, 28, 100, 0, no_bands, full_sheet);
        check(empty_coverage.total == 0 && empty_coverage.complete(), "sin ensanchado, cobertura completa");
    }

    // -- 6. Los bordes -------------------------------------------------------
    std::printf("\n[6] los bordes\n");
    {
        int n = 0;
        auto count_cell = [&](const ExtCell&) { ++n; };
        widescreen_plan(0, 40, 28, 0, 0, no_bands, count_cell);
        check(n == 0, "0 columnas no emite empty_coverage");
        widescreen_plan(3, 0, 28, 0, 0, no_bands, count_cell);
        check(n == 0, "…ni una pantalla de 0 columnas");
        widescreen_plan(3, 40, 0, 0, 0, no_bands, count_cell);
        check(n == 0, "…ni una de 0 filas");
        widescreen_plan(-2, 40, 28, 0, 0, no_bands, count_cell);
        check(n == 0, "un ancho negativo tampoco");

        check(widescreen_target_width(0, 16, 9) == 0, "sin alto no hay ancho");
        check(widescreen_target_width(224, 16, 0) == 0, "ni con una relación inválida");
        check(widescreen_target_width(224, 16, 9, 0, 1) == 0, "ni con un píxel inválido");

        // Cámara negativa: al principio de un nivel las columnas de la izquierda
        // caen antes del origen. Se piden igual — la lámina dirá que no hay arte
        // y el conteo de huecos lo va a mostrar, que es mejor que inventar un
        // recorte silencioso acá.
        std::vector<ExtCell> cells;
        widescreen_plan(2, 40, 1, 1, 0, no_bands,
                        [&](const ExtCell& e) { cells.push_back(e); });
        bool has_negative_column = false;
        for (const ExtCell& e : cells) if (e.level_col < 0) has_negative_column = true;
        check(has_negative_column, "columnas antes del origen se piden igual");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
