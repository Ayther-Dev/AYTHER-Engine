#pragma once
// ---------------------------------------------------------------------------
// widescreen.h — qué celdas de nivel llenan el área extendida ( EM-8.1).
//
// El ancho de más NO se dibuja con lo que hay en la nametable viva. Eso ya se
// midió y no cierra: del lado hacia el que vas, el juego streamea 1-2 celdas por
// delante, y 16:9 sobre 224 px pide 5 por lado (7 si se preserva la relación del
// 4:3 mostrado). El plano A —el gameplay— directamente no tiene arte lateral.
//
//   fuente del lateral    celdas rancias    sin arte
//   nametable viva              182            185
//   lámina de nivel               0            185
//
// «Rancia» es arte de OTRO tramo del nivel: la nametable envuelve cada 512 px, y
// leerla de más devuelve una banda que no corresponde. La lámina del stitcher
// —lo que cada posición mostró cuando estuvo en pantalla— pone el nivel real.
//
// Este archivo es sólo el PLAN: qué posición de nivel va en cada celda del área
// extendida. No dibuja, no lee VRAM y no toca Vulkan, para que se pueda medir
// sin GPU y sin ROM — que es como se encontró el bug de bandas de EM-8.0.
//
// Y la fila importa: en el plano B cada banda de parallax resuelve su propia
// columna (ver `parallax_bands.h`). Un área extendida que pidiera una sola
// columna por lado dejaría huecos justo donde el parallax separa las bandas.
// ---------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>

namespace ayther {

/// Cuántas celdas de 8 px hay que agregar POR LADO para llegar a `target_w_px`.
///
/// Redondea PARA ARRIBA: dibujar de más y recortar deja el borde limpio, y
/// quedarse corto deja una franja negra contra el marco que se lee como un bug
/// de render. La asimetría no es opción — un ensanchado asimétrico se invertiría
/// cada vez que el jugador cambia de dirección, porque el lado que sobra es el
/// trasero (el rastro por donde ya pasó).
inline int widescreen_cols_per_side(int emu_w_px, int target_w_px) {
    if (target_w_px <= emu_w_px) return 0;
    const int per_side = (target_w_px - emu_w_px + 1) / 2;
    return (per_side + 7) / 8;
}

/// El ancho de pantalla que pide una relación `num:den` sobre `emu_h_px`.
///
/// `par_num/par_den` es la relación del PÍXEL (pixel aspect ratio). En píxel
/// cuadrado 16:9 sobre 224 px da 398; preservando la relación del 4:3 mostrado
/// —que es lo que el jugador ve en un CRT— da 426. Son 5 celdas por lado contra
/// 7, y la diferencia se nota: es la decisión que el pack declara.
inline int widescreen_target_width(int emu_h_px, int num, int den,
                                   int par_num = 1, int par_den = 1) {
    if (emu_h_px <= 0 || den <= 0 || num <= 0 || par_num <= 0 || par_den <= 0)
        return 0;
    // ancho_visual = alto · num/den  →  ancho_en_px = ancho_visual · par_den/par_num
    const int64_t w = (int64_t)emu_h_px * num * par_den;
    const int64_t d = (int64_t)den * par_num;
    return (int)((w + d / 2) / d);
}

/// Una celda del área extendida: dónde va en pantalla y de dónde sale.
struct ExtCell {
    int dst_col;    ///< columna de pantalla, NEGATIVA a la izquierda y ≥ emu_cols a la derecha
    int dst_row;    ///< fila de pantalla, 0..rows-1
    int level_col;  ///< columna de nivel a buscar en la lámina
    int level_row;  ///< fila de nivel
};

/// Arma el plan del área extendida para UN plano.
///
/// `band_offset(row)` devuelve cuánto separa a la banda de esa fila de la banda
/// de arriba (`BandCameras::offset_from_top`). En un plano sin parallax por
/// bandas es 0 y todas las filas comparten columna, que es el caso del plano A.
///
/// `emit(ExtCell)` recibe cada celda. Se emiten TODAS, tenga o no arte la
/// lámina: quién dibuja decide qué hacer con un hueco, y esa decisión —dibujar,
/// congelar o dejar el backdrop— no es de este archivo. Devolver sólo las que
/// tienen arte escondería cuánta cobertura falta, que es justo el número que
/// hay que vigilar (medido: 185 huecos, todos verticales).
template <typename BandOffset, typename Emit>
inline void widescreen_plan(int cols_per_side, int emu_cols, int rows,
                            int cam_col, int cam_row,
                            const BandOffset& band_offset, const Emit& emit) {
    if (cols_per_side <= 0 || emu_cols <= 0 || rows <= 0) return;
    for (int r = 0; r < rows; ++r) {
        const int lv_row = cam_row + r;
        const int dx     = band_offset(r);
        for (int c = 1; c <= cols_per_side; ++c) {
            // Izquierda: las columnas ANTERIORES a la pantalla.
            emit(ExtCell{ -c, r, cam_col + dx - c, lv_row });
            // Derecha: las posteriores. `emu_cols + c - 1` y no `+ c`, o la
            // primera columna nueva pisaría la última de la pantalla.
            emit(ExtCell{ emu_cols + c - 1, r, cam_col + dx + emu_cols + c - 1, lv_row });
        }
    }
}

/// Cuántas celdas del plan tienen arte en la lámina, sobre el total.
///
/// Es la medida que decide si el área extendida se puede mostrar: con una toma
/// corta la lámina sale PEOR que la nametable (medido: 259 huecos contra 240),
/// no por el método sino por falta de recorrido. El pack necesita una toma que
/// cubra el nivel, y esto es lo que lo dice antes de que se vea en pantalla.
struct Coverage {
    int total = 0;
    int filled = 0;
    /// Sin celdas pedidas la cobertura es COMPLETA, no cero: no hay nada que
    /// llenar. Devolver 0 haría que «no ensancho» se leyera como «me faltó arte».
    bool complete() const { return filled == total; }
    int  missing() const { return total - filled; }
};

template <typename BandOffset, typename Lookup>
inline Coverage widescreen_coverage(int cols_per_side, int emu_cols, int rows,
                                    int cam_col, int cam_row,
                                    const BandOffset& band_offset,
                                    const Lookup& has_cell) {
    Coverage cv;
    widescreen_plan(cols_per_side, emu_cols, rows, cam_col, cam_row, band_offset,
                    [&](const ExtCell& e) {
                        ++cv.total;
                        if (has_cell(e.level_col, e.level_row)) ++cv.filled;
                    });
    return cv;
}

}  // namespace ayther
