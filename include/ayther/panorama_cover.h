#pragma once
// ---------------------------------------------------------------------------
// panorama_cover.h — la regla de COBERTURA de una Panorámica ().
//
// «¿Lo que se ve en esta posición ES la lámina, o es otra cosa dibujada encima
// del mismo plano?» Una vez que la cámara ancló, hay que contestarla celda por
// celda, y de esa cuenta sale `FrameView.panorama_cover`.
//
// VIVE EN UN HEADER Y NO ADENTRO DE LA SESIÓN porque es una regla del FORMATO
// de la tira —igual que `ayther_plane_tile_hash_variants`, con el que se
// apoya— y porque el defecto que arregla no se podía probar sin una ROM y una
// toma de veinte minutos. Acá se prueba con tres hashes inventados.
//
// EL DEFECTO (, medido en Sonic 3 & Knuckles f2092). Una posición de la
// tira puede tener VARIOS hashes: una celda animada tiene uno por estado, y un
// barrido que cruzó de zona apila dos tramos del nivel en la misma posición. El
// índice los guarda todos —cada estado tiene que poder ANCLAR— pero el PNG
// conserva UNO (`Cell::last` del stitcher).
//
// Aceptar cualquiera para verificar la cobertura declara «anclada, cobertura
// 100 %» sobre una lámina que muestra otro tramo del nivel: el recorte exportado
// era Angel Island —cielo, agua, pasto— mientras el frame era una cueva.
//
// POR QUÉ CASI NUNCA SE VE: el área nativa se corrige sola, porque las celdas
// vivas que la tira no reclamó se dibujan encima y tapan el anclaje flojo. Lo
// delata el ensanchado ( EM-8.1), donde el área extendida no tiene con qué
// corregirse — ahí se ve exactamente lo que la tira tiene.
//
// LO QUE NO ES EL ARREGLO: un piso de cobertura. Se probaron los dos números
// disponibles y ninguno separa los casos (Golden Axe extiende BIEN con 69 %;
// Sonic 3 & K extiende MAL con 100 %). Un umbral afinado contra dos puntos es
// un parche frágil disfrazado de arreglo.
//
// EL ARREGLO es alinear el índice con el dibujo: se verifica contra el hash que
// la lámina CONSERVA y no contra cualquiera de los que pasaron por ahí. Los
// demás no se tiran — siguen en el índice de anclaje, donde la multiplicidad
// ayuda a votar dónde está la cámara y un voto de más se compensa con los otros
// treinta. Lo que no pueden es decidir QUÉ SE DIBUJA donde nadie va a
// corregirlo.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <vector>

namespace ayther {

/// Las 4 lecturas de `h` bajo la línea de paleta `pal`, con `out[0]` = `h` tal
/// cual. La implementación real vive en el core (aritmética exacta: el PRIME es
/// invertible mod 2⁶⁴); esto es sólo el tipo de la función para poder inyectarla
/// en la prueba sin arrastrar el core.
using PanoHashVariantsFn = void (*)(uint64_t h, uint8_t pal, uint64_t out[4]);

/// ¿La celda que la LÁMINA DIBUJA en esta posición es la observada?
///
/// `strip` son los hashes de la tira en esa posición, **con el dibujado
/// primero** — el orden lo garantiza `AytherSession::bg_cells`, y para un pack
/// horneado, el TOML que salió de ahí.
///
/// Una tira vacía en esa posición **no** matchea: no hay con qué comparar, y
/// «no sé» no es «sí». Es la diferencia entre no cubrir una celda y afirmar que
/// la lámina la explica.
inline bool panorama_pos_matches(const std::vector<uint64_t>& strip,
                                 uint64_t h, uint8_t pal,
                                 PanoHashVariantsFn variants) {
    if (strip.empty()) return false;
    // El camino DIRECTO primero: sin repaletado —el caso normal— esto no cuesta
    // nada, y el trabajo extra lo pagan sólo las celdas que ya iban a
    // descartarse.
    const uint64_t rendered_hash = strip[0];
    if (rendered_hash == h) return true;
    // : el mismo dibujo bajo otra línea CRAM produce otro hash (la línea
    // entra al final del FNV). Las cuatro lecturas son exactas, no aproximadas.
    uint64_t var[4];
    variants(h, pal, var);
    for (int i = 1; i < 4; ++i)
        if (rendered_hash == var[i]) return true;
    return false;
}

}  // namespace ayther
